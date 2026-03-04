import math
import time

import numpy as np


# --- Validation ---

def validate_gate_params(params: dict) -> list[str]:
    """Validate noise gate parameters. Returns list of error strings (empty = valid)."""
    errors = []
    if "open_threshold_dbfs" in params:
        v = params["open_threshold_dbfs"]
        if not (-80 <= v <= 0):
            errors.append(f"open_threshold_dbfs must be in [-80, 0], got {v}")
    if "close_threshold_dbfs" in params:
        v = params["close_threshold_dbfs"]
        if not (-80 <= v <= 0):
            errors.append(f"close_threshold_dbfs must be in [-80, 0], got {v}")
    if "open_threshold_dbfs" in params and "close_threshold_dbfs" in params:
        if params["open_threshold_dbfs"] < params["close_threshold_dbfs"] + 3:
            errors.append("open_threshold_dbfs must be >= close_threshold_dbfs + 3")
    if "floor_db" in params:
        v = params["floor_db"]
        if not (-80 <= v <= 0):
            errors.append(f"floor_db must be in [-80, 0], got {v}")
    if "attack_ms" in params:
        v = params["attack_ms"]
        if not (0.5 <= v <= 50):
            errors.append(f"attack_ms must be in [0.5, 50], got {v}")
    if "release_ms" in params:
        v = params["release_ms"]
        if not (10 <= v <= 1000):
            errors.append(f"release_ms must be in [10, 1000], got {v}")
    if "hold_ms" in params:
        v = params["hold_ms"]
        if not (0 <= v <= 1000):
            errors.append(f"hold_ms must be in [0, 1000], got {v}")
    return errors


def validate_comp_params(params: dict) -> list[str]:
    """Validate compressor parameters. Returns list of error strings (empty = valid)."""
    errors = []
    if "threshold_dbfs" in params:
        v = params["threshold_dbfs"]
        if not (-60 <= v <= 0):
            errors.append(f"threshold_dbfs must be in [-60, 0], got {v}")
    if "ratio" in params:
        v = params["ratio"]
        if not (1.0 <= v <= 20.0):
            errors.append(f"ratio must be in [1.0, 20.0], got {v}")
    if "makeup_gain_db" in params:
        v = params["makeup_gain_db"]
        if not (0 <= v <= 24):
            errors.append(f"makeup_gain_db must be in [0, 24], got {v}")
    if "attack_ms" in params:
        v = params["attack_ms"]
        if not (0.5 <= v <= 50):
            errors.append(f"attack_ms must be in [0.5, 50], got {v}")
    if "release_ms" in params:
        v = params["release_ms"]
        if not (10 <= v <= 1000):
            errors.append(f"release_ms must be in [10, 1000], got {v}")
    return errors


# --- NoiseGate ---

class NoiseGate:
    """Expander-style noise gate with hysteresis, smoothed detector, and vectorized gain ramp."""

    MAX_BLOCKSIZE = 4096

    def __init__(self, sample_rate: int = 48000, enabled: bool = True,
                 open_threshold_dbfs: float = -45.0, close_threshold_dbfs: float = -50.0,
                 floor_db: float = -25.0, hold_ms: float = 100.0,
                 attack_ms: float = 5.0, release_ms: float = 150.0):
        self.enabled = enabled
        self.sample_rate = sample_rate
        self.open_threshold_dbfs = open_threshold_dbfs
        self.close_threshold_dbfs = close_threshold_dbfs
        self.floor_db = floor_db
        self.hold_ms = hold_ms
        self.attack_ms = attack_ms
        self.release_ms = release_ms

        # Derived
        self._floor_lin = 10.0 ** (floor_db / 20.0)
        self._hold_samples = int(hold_ms * sample_rate / 1000.0)
        self._recompute_coefficients()

        # Pre-allocated index array for vectorized gain ramp
        self._indices = np.arange(self.MAX_BLOCKSIZE, dtype=np.float32)

        # State
        self._gate_open = False
        self._hold_remaining = 0
        self._current_gain = self._floor_lin
        self._detector_env = 0.0

        # Metering
        self._input_dbfs = -80.0
        self._output_dbfs = -80.0

        # Calibration — silence phase
        self._calibrating = False
        self._cal_rms_values: list[float] = []
        self._cal_start_time = 0.0
        self._cal_duration_s = 2.0
        self.calibrated_noise_floor_dbfs = -80.0

        # Calibration — speech phase
        self._speech_calibrating = False
        self._speech_cal_rms_values: list[float] = []
        self._speech_cal_start_time = 0.0
        self._speech_cal_duration_s = 3.0
        self.calibrated_speech_dbfs = -80.0

    def _recompute_coefficients(self):
        tau_attack = self.attack_ms * self.sample_rate / 1000.0
        tau_release = self.release_ms * self.sample_rate / 1000.0
        self._a_attack = math.exp(-1.0 / max(tau_attack, 1.0))
        self._a_release = math.exp(-1.0 / max(tau_release, 1.0))
        # Detector envelope: 10ms time constant
        tau_det = 0.010 * self.sample_rate
        self._a_det = math.exp(-1.0 / max(tau_det, 1.0))

    def process_inplace(self, raw_block: np.ndarray, out_buf: np.ndarray):
        """Process audio. Detector reads raw_block, output written to out_buf."""
        n = len(raw_block)

        # 1. Input metering (raw, pre-gate)
        input_rms = float(np.sqrt(np.mean(raw_block ** 2)))
        self._input_dbfs = 20.0 * math.log10(max(input_rms, 1e-10))

        # 2. Calibration tap (raw input)
        if self._calibrating:
            self._cal_rms_values.append(input_rms)
            if time.time() - self._cal_start_time >= self._cal_duration_s:
                self._calibrating = False
        if self._speech_calibrating:
            self._speech_cal_rms_values.append(input_rms)
            if time.time() - self._speech_cal_start_time >= self._speech_cal_duration_s:
                self._speech_calibrating = False

        # 3. Smoothed detector envelope
        if input_rms > self._detector_env:
            self._detector_env = input_rms  # instant attack
        else:
            self._detector_env *= self._a_det ** n  # block-corrected release
        det_dbfs = 20.0 * math.log10(max(self._detector_env, 1e-10))

        # 4. Hysteresis state machine
        if not self._gate_open:
            if det_dbfs >= self.open_threshold_dbfs:
                self._gate_open = True
                self._hold_remaining = self._hold_samples
        else:
            if det_dbfs < self.close_threshold_dbfs:
                self._hold_remaining -= n
                if self._hold_remaining <= 0:
                    self._gate_open = False
                    self._hold_remaining = 0
            else:
                self._hold_remaining = self._hold_samples

        # 5. Target gain
        target = 1.0 if self._gate_open else self._floor_lin

        # 6. Vectorized gain ramp
        a = self._a_attack if target > self._current_gain else self._a_release
        ramp = a ** self._indices[:n]
        gains = target + (self._current_gain - target) * ramp
        self._current_gain = float(gains[-1])

        # Write to output buffer in-place
        np.multiply(raw_block, gains, out=out_buf)

        # 7. Output metering (post-gate)
        out_rms = float(np.sqrt(np.mean(out_buf ** 2)))
        self._output_dbfs = 20.0 * math.log10(max(out_rms, 1e-10))

    def reset(self):
        """Reset state. Call on stream restart."""
        self._gate_open = False
        self._hold_remaining = 0
        self._current_gain = self._floor_lin
        self._detector_env = 0.0
        self._input_dbfs = -80.0
        self._output_dbfs = -80.0

    def configure(self, **kwargs):
        """Update parameters at runtime. Raises ValueError on invalid params."""
        # Check cross-parameter constraints with merged values
        check = {}
        for k in ("open_threshold_dbfs", "close_threshold_dbfs"):
            check[k] = kwargs.get(k, getattr(self, k))
        if "open_threshold_dbfs" in kwargs or "close_threshold_dbfs" in kwargs:
            if check["open_threshold_dbfs"] < check["close_threshold_dbfs"] + 3:
                raise ValueError("open_threshold_dbfs must be >= close_threshold_dbfs + 3")

        errors = validate_gate_params(kwargs)
        if errors:
            raise ValueError("; ".join(errors))

        for k, v in kwargs.items():
            if hasattr(self, k):
                setattr(self, k, v)

        self._floor_lin = 10.0 ** (self.floor_db / 20.0)
        self._hold_samples = int(self.hold_ms * self.sample_rate / 1000.0)
        self._recompute_coefficients()

    def start_calibration(self):
        self._cal_rms_values = []
        self._cal_start_time = time.time()
        self._calibrating = True

    def finish_silence_calibration(self) -> dict | None:
        """Finalize silence phase. Returns noise floor or None if failed."""
        self._calibrating = False
        if not self._cal_rms_values:
            return None

        rms_array = np.array(self._cal_rms_values)

        # Reject if speech was detected (any sample > floor + 20dB in linear)
        floor_rms = np.percentile(rms_array, 50)
        speech_threshold = floor_rms * 10.0  # +20dB
        if np.any(rms_array > speech_threshold) and floor_rms > 1e-8:
            self._cal_rms_values = []
            return None

        # 95th percentile as noise floor (robust to transients)
        noise_rms = float(np.percentile(rms_array, 95))
        noise_dbfs = 20.0 * math.log10(max(noise_rms, 1e-10))
        self.calibrated_noise_floor_dbfs = round(noise_dbfs, 1)

        return {"noise_floor_dbfs": round(noise_dbfs, 1)}

    def start_speech_calibration(self):
        """Begin collecting RMS for speech level measurement."""
        self._speech_cal_rms_values = []
        self._speech_cal_start_time = time.time()
        self._speech_calibrating = True

    def finish_calibration(self) -> dict | None:
        """Finalize speech phase and compute thresholds from noise + speech."""
        self._speech_calibrating = False
        noise_dbfs = self.calibrated_noise_floor_dbfs

        if self._speech_cal_rms_values:
            rms_array = np.array(self._speech_cal_rms_values)
            # Use 75th percentile of speech samples (captures typical speech level)
            speech_rms = float(np.percentile(rms_array, 75))
            speech_dbfs = 20.0 * math.log10(max(speech_rms, 1e-10))
            self.calibrated_speech_dbfs = round(speech_dbfs, 1)
        else:
            speech_dbfs = noise_dbfs + 20.0  # fallback
            self.calibrated_speech_dbfs = round(speech_dbfs, 1)

        self._speech_cal_rms_values = []

        # Compute thresholds using both measurements
        close_threshold = noise_dbfs + 6.0
        # Place open threshold 30% of the way from noise to speech
        gap = speech_dbfs - noise_dbfs
        if gap > 8.0:
            open_threshold = noise_dbfs + gap * 0.3
        else:
            open_threshold = close_threshold + 4.0  # fallback if gap too small

        # Ensure minimum hysteresis
        if open_threshold < close_threshold + 3.0:
            open_threshold = close_threshold + 3.0

        close_threshold = max(-70.0, min(-10.0, close_threshold))
        open_threshold = max(-66.0, min(-6.0, open_threshold))

        self._cal_rms_values = []

        return {
            "noise_floor_dbfs": round(noise_dbfs, 1),
            "speech_level_dbfs": round(speech_dbfs, 1),
            "close_threshold_dbfs": round(close_threshold, 1),
            "open_threshold_dbfs": round(open_threshold, 1),
        }

    def get_state(self) -> dict:
        gain_db = 20.0 * math.log10(max(self._current_gain, 1e-10))
        return {
            "enabled": self.enabled,
            "gate_open": self._gate_open,
            "input_dbfs": round(self._input_dbfs, 1),
            "output_dbfs": round(self._output_dbfs, 1),
            "current_gain_db": round(gain_db, 1),
            "attenuation_db": round(-gain_db, 1),
            "open_threshold_dbfs": self.open_threshold_dbfs,
            "close_threshold_dbfs": self.close_threshold_dbfs,
            "floor_db": self.floor_db,
            "hold_ms": self.hold_ms,
            "attack_ms": self.attack_ms,
            "release_ms": self.release_ms,
            "calibrating": self._calibrating,
            "speech_calibrating": self._speech_calibrating,
            "calibrated_noise_floor_dbfs": self.calibrated_noise_floor_dbfs,
            "calibrated_speech_dbfs": self.calibrated_speech_dbfs,
        }


# --- Compressor ---

class Compressor:
    """Feed-forward compressor with RMS envelope follower."""

    def __init__(self, sample_rate: int = 48000, enabled: bool = False,
                 threshold_dbfs: float = -15.0, ratio: float = 2.0,
                 attack_ms: float = 5.0, release_ms: float = 100.0,
                 makeup_gain_db: float = 0.0):
        self.enabled = enabled
        self.sample_rate = sample_rate
        self.threshold_dbfs = threshold_dbfs
        self.ratio = ratio
        self.attack_ms = attack_ms
        self.release_ms = release_ms
        self.makeup_gain_db = makeup_gain_db

        self._makeup_lin = 10.0 ** (makeup_gain_db / 20.0)
        self._recompute_coefficients()

        # Pre-allocated index array
        self._indices = np.arange(NoiseGate.MAX_BLOCKSIZE, dtype=np.float32)

        # State
        self._envelope_db = -80.0
        self._current_gr_lin = 1.0  # gain reduction as linear multiplier
        self._gain_reduction_db = 0.0

    def _recompute_coefficients(self):
        tau_attack = self.attack_ms * self.sample_rate / 1000.0
        tau_release = self.release_ms * self.sample_rate / 1000.0
        self._a_attack = math.exp(-1.0 / max(tau_attack, 1.0))
        self._a_release = math.exp(-1.0 / max(tau_release, 1.0))

    def process_inplace(self, buf: np.ndarray):
        """Process audio in-place. Reads and writes to buf."""
        n = len(buf)

        # 1. Block RMS → dBFS
        rms = float(np.sqrt(np.mean(buf ** 2)))
        level_db = 20.0 * math.log10(max(rms, 1e-10))

        # 2. Envelope follower (block-corrected: raise coefficient to block size)
        if level_db > self._envelope_db:
            a = self._a_attack ** n
        else:
            a = self._a_release ** n
        self._envelope_db = level_db + a * (self._envelope_db - level_db)

        # 3. Gain computer
        if self._envelope_db > self.threshold_dbfs:
            over = self._envelope_db - self.threshold_dbfs
            gr_db = over * (1.0 - 1.0 / self.ratio)
        else:
            gr_db = 0.0

        self._gain_reduction_db = gr_db
        target_gr_lin = 10.0 ** (-gr_db / 20.0) * self._makeup_lin

        # 4. Smooth gain ramp (reuse same pattern)
        a_smooth = self._a_attack if target_gr_lin < self._current_gr_lin else self._a_release
        ramp = a_smooth ** self._indices[:n]
        gains = target_gr_lin + (self._current_gr_lin - target_gr_lin) * ramp
        self._current_gr_lin = float(gains[-1])

        # 5. Apply in-place
        buf *= gains

    def reset(self):
        """Reset state. Call on stream restart."""
        self._envelope_db = -80.0
        self._current_gr_lin = 1.0
        self._gain_reduction_db = 0.0

    def configure(self, **kwargs):
        """Update parameters at runtime. Raises ValueError on invalid params."""
        errors = validate_comp_params(kwargs)
        if errors:
            raise ValueError("; ".join(errors))

        for k, v in kwargs.items():
            if hasattr(self, k):
                setattr(self, k, v)

        self._makeup_lin = 10.0 ** (self.makeup_gain_db / 20.0)
        self._recompute_coefficients()

    def get_state(self) -> dict:
        return {
            "enabled": self.enabled,
            "threshold_dbfs": self.threshold_dbfs,
            "ratio": self.ratio,
            "gain_reduction_db": round(self._gain_reduction_db, 1),
            "attack_ms": self.attack_ms,
            "release_ms": self.release_ms,
            "makeup_gain_db": self.makeup_gain_db,
        }


# --- DSPChain ---

class DSPChain:
    """Chains NoiseGate → Compressor. In-place, zero-alloc per block."""

    def __init__(self, gate: NoiseGate, compressor: Compressor,
                 max_blocksize: int = NoiseGate.MAX_BLOCKSIZE):
        self.gate = gate
        self.compressor = compressor
        self._buf = np.empty(max_blocksize, dtype=np.float32)

    def process(self, block: np.ndarray) -> np.ndarray:
        """Process block through gate → compressor. Returns view into pre-allocated buffer."""
        n = len(block)
        out = self._buf[:n]
        np.copyto(out, block)

        if self.gate.enabled:
            self.gate.process_inplace(block, out)
        if self.compressor.enabled:
            self.compressor.process_inplace(out)

        return out

    def reset(self):
        """Reset all DSP state. Call on stream restart / device hot-swap."""
        self.gate.reset()
        self.compressor.reset()

    def get_state(self) -> dict:
        return {
            "gate": self.gate.get_state(),
            "compressor": self.compressor.get_state(),
        }
