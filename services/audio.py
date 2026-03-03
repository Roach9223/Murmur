import logging
import queue
import threading
import time

import numpy as np
import sounddevice as sd
from scipy.signal import resample_poly
from scipy.signal.windows import blackmanharris

from services.config import RECORD_RATE, WHISPER_RATE, CHANNELS

logger = logging.getLogger(__name__)


class AudioCaptureService:
    _MAX_CONSECUTIVE_ERRORS = 50

    # FFT / spectrum constants (professional-grade, inspired by FabFilter Pro-Q)
    FFT_WINDOW = 8192       # ~170ms at 48kHz, 5.86Hz frequency resolution
    FFT_HOP = 4096          # 50% overlap for smoother temporal tracking
    N_BINS = 128            # Double bin count for smoother curves
    DB_FLOOR = -96.0        # Matches SSL 2 MKII ~98dB SNR
    DB_CEIL = 0.0

    def __init__(self, device_index: int, dsp_chain=None):
        self.device_index = device_index
        self.recording = False
        self.audio_q: queue.Queue[tuple[np.ndarray, float]] = queue.Queue()
        self._stream = None
        self._error_count = 0
        self._needs_restart = False
        self._dsp_chain = dsp_chain
        self._live_rms = 0.0

        # --- Spectrum / FFT state ---
        # Blackman-Harris 4-term window: >92dB sidelobe rejection (vs Hann's 32dB)
        self._window = blackmanharris(self.FFT_WINDOW).astype(np.float32)
        self._window_compensation = 1.0 / float(np.mean(self._window ** 2))
        # Reference power: full-scale sine after windowing
        self._ref_power = 0.5 * self._window_compensation

        # Ring buffer: post-DSP (default spectrum view)
        self._ring = np.zeros(self.FFT_WINDOW, dtype=np.float32)
        self._ring_pos = 0
        self._ring_lock = threading.Lock()

        # Ring buffer: pre-DSP (debug tap)
        self._ring_pre = np.zeros(self.FFT_WINDOW, dtype=np.float32)
        self._ring_pre_pos = 0
        self._spectrum_pre_dsp = False  # False=post-DSP (default), True=pre-DSP

        # 50% overlap tracking
        self._samples_since_fft = 0

        # FFT cache + preallocated work buffer
        self._cached_bins: list[float] = []
        self._fft_buf = np.empty(self.FFT_WINDOW, dtype=np.float32)
        self._fft_lock = threading.Lock()
        self._fft_stop = threading.Event()
        self._fft_thread: threading.Thread | None = None

        # Pre-compute log-spaced bin edges (20Hz → 20kHz) and masks
        self._log_edges = np.logspace(np.log10(20), np.log10(20000), self.N_BINS + 1)
        self._freqs = np.fft.rfftfreq(self.FFT_WINDOW, d=1.0 / RECORD_RATE)
        self._bin_masks = []
        for i in range(self.N_BINS):
            self._bin_masks.append(
                (self._freqs >= self._log_edges[i]) & (self._freqs < self._log_edges[i + 1])
            )

        # Geometric mean center frequency of each log band (for interpolation fallback)
        self._bin_centers = np.sqrt(self._log_edges[:-1] * self._log_edges[1:])
        # Precompute count of FFT bins per display band (for sparse-band detection)
        self._bin_counts = np.array([int(np.count_nonzero(m)) for m in self._bin_masks], dtype=np.int32)
        # DC/subsonic cutoff bin index (zero out FFT bins below 20 Hz)
        self._dc_cutoff_bin = int(np.ceil(20.0 / (RECORD_RATE / self.FFT_WINDOW)))

    def _callback(self, indata, frames, time_info, status):
        if status:
            self._error_count += 1
            if self._error_count >= self._MAX_CONSECUTIVE_ERRORS and not self._needs_restart:
                logger.warning("Audio: %d consecutive errors, flagging for restart", self._error_count)
                self._needs_restart = True
            return
        self._error_count = 0

        # Extract mono (audio is float32 [-1, 1])
        mono = indata[:, 0] if indata.ndim > 1 else indata.squeeze()

        # DSP processing (gate detector reads raw mono internally)
        if self._dsp_chain is not None:
            processed = self._dsp_chain.process(mono)
        else:
            processed = mono

        # Always-on RMS for UI metering
        self._live_rms = float(np.sqrt(np.mean(processed ** 2)))

        n = len(mono)
        with self._ring_lock:
            # Write raw mono to pre-DSP ring buffer
            end_pre = self._ring_pre_pos + n
            if end_pre <= self.FFT_WINDOW:
                self._ring_pre[self._ring_pre_pos:end_pre] = mono
            else:
                first = self.FFT_WINDOW - self._ring_pre_pos
                self._ring_pre[self._ring_pre_pos:] = mono[:first]
                self._ring_pre[:n - first] = mono[first:]
            self._ring_pre_pos = end_pre % self.FFT_WINDOW

            # Write processed to post-DSP ring buffer (main)
            end = self._ring_pos + n
            if end <= self.FFT_WINDOW:
                self._ring[self._ring_pos:end] = processed
            else:
                first = self.FFT_WINDOW - self._ring_pos
                self._ring[self._ring_pos:] = processed[:first]
                self._ring[:n - first] = processed[first:]
            self._ring_pos = end % self.FFT_WINDOW
            self._samples_since_fft += n

        if self.recording:
            rms = float(np.sqrt(np.mean(processed ** 2)))
            out = processed.reshape(-1, 1) if indata.ndim > 1 else processed
            self.audio_q.put((out.copy() if out.base is not None else out, rms))

    def start_stream(self):
        """Open the mic stream (always on). Call start_recording()/stop_recording() to control capture."""
        self._stream = sd.InputStream(
            samplerate=RECORD_RATE,
            channels=CHANNELS,
            device=self.device_index,
            dtype="float32",
            callback=self._callback,
            blocksize=0,
        )
        self._stream.start()
        # Start FFT computation thread (50% overlap, ~23Hz effective rate)
        self._fft_stop.clear()
        self._fft_thread = threading.Thread(target=self._fft_loop, daemon=True, name="fft-compute")
        self._fft_thread.start()

    def stop_stream(self):
        self._fft_stop.set()
        if self._fft_thread:
            self._fft_thread.join(timeout=1)
            self._fft_thread = None
        if self._stream:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception as e:
                logger.warning("Audio: error stopping stream: %s", e)
            self._stream = None

    def _reset_dsp_and_rings(self):
        """Reset DSP state and ring buffers. Call on stream restart / device hot-swap."""
        if self._dsp_chain is not None:
            self._dsp_chain.reset()
        with self._ring_lock:
            self._ring[:] = 0
            self._ring_pos = 0
            self._ring_pre[:] = 0
            self._ring_pre_pos = 0
            self._samples_since_fft = 0

    def restart_stream(self) -> bool:
        """Stop and re-open the audio stream. Returns True on success."""
        logger.warning("Audio: restarting stream...")
        try:
            self.stop_stream()
            self._reset_dsp_and_rings()
            time.sleep(0.5)
            self.start_stream()
            self._needs_restart = False
            self._error_count = 0
            logger.info("Audio: stream restarted successfully")
            return True
        except Exception as e:
            logger.error("Audio: restart failed: %s", e)
            return False

    @property
    def needs_restart(self) -> bool:
        return self._needs_restart

    def start_recording(self):
        self.recording = True

    def stop_recording(self):
        self.recording = False

    # --- FFT / Spectrum ---

    def _fft_loop(self):
        """Compute FFT bins with 50% overlap for professional-grade smoothness."""
        while not self._fft_stop.is_set() and self._stream is not None:
            try:
                if self._samples_since_fft >= self.FFT_HOP:
                    with self._ring_lock:
                        self._samples_since_fft -= self.FFT_HOP
                    bins = self._compute_fft_bins()
                    with self._fft_lock:
                        self._cached_bins = bins
            except Exception:
                pass  # keep last good bins on error
            self._fft_stop.wait(0.015)  # 15ms poll — tight enough to catch hops

    def get_cached_fft_bins(self) -> list[float]:
        with self._fft_lock:
            return list(self._cached_bins)

    def _compute_fft_bins(self) -> list[float]:
        with self._ring_lock:
            # Snapshot toggle + read consistent (ring, pos) pair under lock
            if self._spectrum_pre_dsp:
                ring, pos = self._ring_pre, self._ring_pre_pos
            else:
                ring, pos = self._ring, self._ring_pos
            # Two-slice copy into preallocated buffer (no heap alloc)
            buf = self._fft_buf
            tail = self.FFT_WINDOW - pos
            buf[:tail] = ring[pos:]
            buf[tail:] = ring[:pos]

        # Windowed FFT (Blackman-Harris 4-term)
        windowed = buf * self._window
        spectrum = np.fft.rfft(windowed)

        # Power spectrum with window energy compensation
        power = (np.abs(spectrum) ** 2) * self._window_compensation

        # DC/subsonic rejection: zero out bins below 20 Hz
        power[:self._dc_cutoff_bin] = 0.0

        # Interpolated power at band centers (for sparse low-freq bands)
        interp_power = np.interp(self._bin_centers, self._freqs, power)

        # Bin into 128 log-spaced bands (20Hz-20kHz), convert to dBFS
        bins = []
        for i, mask in enumerate(self._bin_masks):
            if self._bin_counts[i] >= 2:
                avg_power = float(np.mean(power[mask]))
            else:
                # Sparse band (0-1 FFT bins): interpolate power at band center
                avg_power = float(interp_power[i])
            # dBFS: 0 dB = full-scale sine after window compensation
            db = 10.0 * np.log10(max(avg_power, 1e-12) / self._ref_power)
            db = max(self.DB_FLOOR, min(self.DB_CEIL, db))
            bins.append(float((db - self.DB_FLOOR) / (self.DB_CEIL - self.DB_FLOOR)))

        return bins

    @staticmethod
    def enumerate_input_devices() -> list[dict]:
        """Return all available input devices as [{index, name, is_default, sample_rate, channels}]."""
        devices = sd.query_devices()
        default_input = sd.default.device[0]
        result = []
        for i, dev in enumerate(devices):
            if dev['max_input_channels'] > 0:
                result.append({
                    "index": i,
                    "name": dev['name'],
                    "is_default": (i == default_input),
                    "sample_rate": int(dev['default_samplerate']),
                    "channels": dev['max_input_channels'],
                })
        return result

    def get_device_name(self) -> str:
        """Return the name of the currently active device."""
        try:
            info = sd.query_devices(self.device_index)
            return info['name']
        except Exception:
            return f"Device {self.device_index}"

    def switch_device(self, new_index: int):
        """Stop current stream, switch device, restart."""
        old_index = self.device_index
        self.stop_stream()
        self._reset_dsp_and_rings()
        self.device_index = new_index
        time.sleep(0.3)  # WASAPI needs time to release the device
        try:
            self.start_stream()
            logger.info("Audio: switched to device %d", new_index)
        except Exception as e:
            logger.error("Audio: failed to open device %d: %s — reverting to %d", new_index, e, old_index)
            self.device_index = old_index
            time.sleep(0.3)
            self.start_stream()

    @property
    def live_rms(self) -> float:
        return self._live_rms

    def get_dsp_state(self) -> dict | None:
        """Return current DSP state for status reporting."""
        if self._dsp_chain is not None:
            return self._dsp_chain.get_state()
        return None

    def set_spectrum_source(self, pre_dsp: bool):
        """Toggle spectrum between pre-DSP (raw) and post-DSP (processed)."""
        with self._ring_lock:
            self._spectrum_pre_dsp = pre_dsp

    @staticmethod
    def resample(chunks: list[np.ndarray]) -> np.ndarray:
        """Concatenate audio chunks and resample from RECORD_RATE to WHISPER_RATE."""
        audio = np.concatenate(chunks, axis=0).astype(np.float32).squeeze()
        return resample_poly(audio, up=1, down=RECORD_RATE // WHISPER_RATE)
