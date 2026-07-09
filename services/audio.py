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

# Largest audio block we mix per callback. Matches the DSP chain's block cap
# (services.dsp.NoiseGate.MAX_BLOCKSIZE); WASAPI blocks (blocksize=0) stay well under.
_MAX_BLOCKSIZE = 4096

# Substrings that flag a likely loopback/system-audio capture device by name.
_LOOPBACK_NAME_HINTS = ("loopback", "cable output", "stereo mix", "what u hear", "vb-audio")


class _LoopbackRing:
    """Single-producer / single-consumer "newest-N" ring for the loopback stream.

    The loopback callback (producer) writes mono frames; the mic callback (consumer)
    reads the most-recent N samples each block. Lock-free under CPython's GIL: the
    write index (`_wpos`) is published with a single atomic store, and the consumer
    only ever reads a window ending at the current head — a torn read at the wrap
    boundary costs at worst a few stale samples mixed under speech, inaudible to ASR.
    """

    def __init__(self, capacity: int):
        self._cap = int(capacity)
        self._buf = np.zeros(self._cap, dtype=np.float32)
        self._wpos = 0  # next write index; published last

    def zero(self):
        self._buf[:] = 0
        self._wpos = 0

    def write(self, samples: np.ndarray):
        """Producer thread only."""
        n = len(samples)
        if n >= self._cap:  # block larger than ring: keep the tail
            self._buf[:] = samples[-self._cap:]
            self._wpos = 0
            return
        w = self._wpos
        end = w + n
        if end <= self._cap:
            self._buf[w:end] = samples
        else:
            first = self._cap - w
            self._buf[w:] = samples[:first]
            self._buf[:n - first] = samples[first:]
        self._wpos = end % self._cap  # single atomic publish

    def read_last(self, n: int, out: np.ndarray):
        """Consumer thread only. Fill out[:n] with the most recent n samples."""
        cap = self._cap
        if n > cap:
            out[:n] = 0
            n = cap
        w = self._wpos  # single atomic snapshot
        start = (w - n) % cap
        end = start + n
        if end <= cap:
            out[:n] = self._buf[start:end]
        else:
            first = cap - start
            out[:first] = self._buf[start:]
            out[first:n] = self._buf[:n - first]


class AudioCaptureService:
    _MAX_CONSECUTIVE_ERRORS = 50

    # FFT / spectrum constants (professional-grade, inspired by FabFilter Pro-Q)
    FFT_WINDOW = 8192       # ~170ms at 48kHz, 5.86Hz frequency resolution
    FFT_HOP = 4096          # 50% overlap for smoother temporal tracking
    N_BINS = 128            # Double bin count for smoother curves
    DB_FLOOR = -96.0        # Matches SSL 2 MKII ~98dB SNR
    DB_CEIL = 0.0

    def __init__(self, device_index: int, dsp_chain=None, queue_maxsize: int = 500):
        self.device_index = device_index
        self.recording = False
        self.audio_q: queue.Queue[tuple[np.ndarray, float]] = queue.Queue(
            maxsize=queue_maxsize if queue_maxsize > 0 else 0
        )
        self._stream = None
        self._error_count = 0
        self._needs_restart = False
        self._dsp_chain = dsp_chain
        self._live_rms = 0.0
        self._queue_drops = 0
        self._queue_drop_t0 = time.monotonic()

        # --- WAV recording tap (independent of dictation) ---
        self._wav_recorder = None
        self._record_pre_dsp = False

        # --- System-audio loopback (second input stream, mixed into the mic) ---
        self._loopback_enabled = False
        self._loopback_gain = 1.0
        self._loopback_device_index: int | None = None
        self._loopback_stream = None
        self._loopback_error_count = 0
        self._loopback_ring = _LoopbackRing(int(0.5 * RECORD_RATE))  # 0.5s drift tolerance
        # Consumer-owned scratch (mic callback): newest-N loopback read + mixed output
        self._lb_read = np.empty(_MAX_BLOCKSIZE, dtype=np.float32)
        self._mix_buf = np.empty(_MAX_BLOCKSIZE, dtype=np.float32)

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

        # Mix in system-audio loopback (if enabled). DSP stays mic-only so the
        # friend's audio passes through ungated/uncompressed. Mix into a dedicated
        # buffer — `processed` is a view into the DSP chain's shared scratch.
        if self._loopback_enabled:
            m = len(processed)
            mixed = self._mix_buf[:m]
            np.copyto(mixed, processed)
            self._loopback_ring.read_last(m, self._lb_read)
            lb = self._lb_read[:m]
            if self._loopback_gain != 1.0:
                np.multiply(lb, self._loopback_gain, out=lb)
            mixed += lb
            np.clip(mixed, -1.0, 1.0, out=mixed)
        else:
            mixed = processed

        # Always-on RMS for UI metering (reflects the mixed signal that is transcribed)
        self._live_rms = float(np.sqrt(np.mean(mixed ** 2)))

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
            rms = float(np.sqrt(np.mean(mixed ** 2)))
            out = mixed.reshape(-1, 1) if indata.ndim > 1 else mixed
            item = (out.copy(), rms)  # mixed reuses _mix_buf each block — always copy
            try:
                self.audio_q.put_nowait(item)
            except queue.Full:
                try:
                    self.audio_q.get_nowait()  # drop oldest
                except queue.Empty:
                    pass
                try:
                    self.audio_q.put_nowait(item)
                except queue.Full:
                    pass
                self._queue_drops += 1
                if self._queue_drops % 100 == 1:
                    elapsed = time.monotonic() - self._queue_drop_t0
                    rate = self._queue_drops / max(elapsed, 0.001)
                    logger.warning("Audio queue full — dropped %d chunks (%.1f drops/sec)",
                                   self._queue_drops, rate)

        # WAV recording tap (independent of dictation recording)
        if self._wav_recorder is not None and self._wav_recorder.is_recording:
            source = mono if self._record_pre_dsp else processed
            self._wav_recorder.push(source)

    def _loopback_callback(self, indata, frames, time_info, status):
        """Producer for the loopback ring. Downmix stereo->mono, then publish.

        Errors are isolated from the mic stream — a loopback fault never flags the
        mic for restart; persistent faults just auto-disable loopback.
        """
        if status:
            self._loopback_error_count += 1
            if self._loopback_error_count >= self._MAX_CONSECUTIVE_ERRORS and self._loopback_enabled:
                logger.warning("Loopback: %d consecutive errors, auto-disabling", self._loopback_error_count)
                self._loopback_enabled = False
            return
        self._loopback_error_count = 0
        if indata.ndim > 1 and indata.shape[1] > 1:
            mono = indata.mean(axis=1)  # downmix L/R
        else:
            mono = indata[:, 0] if indata.ndim > 1 else indata.squeeze()
        self._loopback_ring.write(mono)

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
        # Drain stale audio from previous sessions
        while not self.audio_q.empty():
            try:
                self.audio_q.get_nowait()
            except queue.Empty:
                break
        self._queue_drops = 0
        self._queue_drop_t0 = time.monotonic()
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

    # --- System-audio loopback ---

    @property
    def loopback_enabled(self) -> bool:
        return self._loopback_enabled

    @property
    def loopback_device_index(self) -> int | None:
        return self._loopback_device_index

    def set_loopback_gain(self, gain: float):
        self._loopback_gain = max(0.0, float(gain))

    def get_loopback_device_name(self) -> str:
        if self._loopback_device_index is None:
            return ""
        try:
            return sd.query_devices(self._loopback_device_index)['name']
        except Exception:
            return f"Device {self._loopback_device_index}"

    def enable_loopback(self, index: int) -> bool:
        """Open a second input stream on a loopback device and mix it into the mic.

        Independent of the mic stream — a failure here never touches the mic.
        Returns True on success.
        """
        if index is None:
            return False
        if index == self.device_index:
            logger.error("Loopback: device %d is the active mic — refusing to loop it back", index)
            return False
        # Already running on this device? no-op.
        if self._loopback_stream is not None and self._loopback_device_index == index:
            return True
        # Switching devices: tear down the old stream first.
        if self._loopback_stream is not None:
            self.disable_loopback()
        try:
            info = sd.query_devices(index)
            channels = min(2, int(info['max_input_channels'])) or 1
            sd.check_input_settings(device=index, samplerate=RECORD_RATE,
                                    channels=channels, dtype="float32")
            stream = sd.InputStream(
                samplerate=RECORD_RATE,
                channels=channels,
                device=index,
                dtype="float32",
                callback=self._loopback_callback,
                blocksize=0,
            )
            stream.start()
        except Exception as e:
            logger.error("Loopback: failed to open device %d: %s", index, e)
            return False
        time.sleep(0.3)  # WASAPI settle
        self._loopback_stream = stream
        self._loopback_device_index = index
        self._loopback_error_count = 0
        self._loopback_ring.zero()
        self._loopback_enabled = True  # publish flag last
        logger.info("Loopback: capturing system audio from device %d (%s)", index, info['name'])
        return True

    def disable_loopback(self):
        """Stop and close the loopback stream. Safe to call when already off."""
        self._loopback_enabled = False  # stop the mic callback mixing first
        if self._loopback_stream is not None:
            try:
                self._loopback_stream.stop()
                self._loopback_stream.close()
            except Exception as e:
                logger.warning("Loopback: error stopping stream: %s", e)
            self._loopback_stream = None
        self._loopback_device_index = None

    def switch_loopback_device(self, index: int) -> bool:
        """Switch the loopback source at runtime. No-op if unchanged."""
        if index == self._loopback_device_index:
            return True
        return self.enable_loopback(index)

    _loopback_cache: list[dict] | None = None
    _loopback_cache_t: float = 0.0
    _LOOPBACK_CACHE_TTL = 5.0  # seconds — /status polls at 20Hz; don't probe every time

    @staticmethod
    def enumerate_loopback_devices() -> list[dict]:
        """Input devices usable as loopback sources, flagged + 48kHz-checked.

        Returns [{index, name, is_default, is_loopback, supported}] where
        is_loopback flags names that look like system-audio captures and
        supported indicates the device accepts 48kHz float32 input.

        Cached for a few seconds: check_input_settings probes PortAudio per
        device, which is too expensive to run on every /status poll.
        """
        now = time.monotonic()
        if (AudioCaptureService._loopback_cache is not None
                and now - AudioCaptureService._loopback_cache_t < AudioCaptureService._LOOPBACK_CACHE_TTL):
            return AudioCaptureService._loopback_cache
        result = []
        for dev in AudioCaptureService.enumerate_input_devices():
            name_l = dev["name"].lower()
            channels = min(2, int(dev["channels"])) or 1
            try:
                sd.check_input_settings(device=dev["index"], samplerate=RECORD_RATE,
                                        channels=channels, dtype="float32")
                supported = True
            except Exception:
                supported = False
            result.append({
                "index": dev["index"],
                "name": dev["name"],
                "is_default": dev["is_default"],
                "is_loopback": any(h in name_l for h in _LOOPBACK_NAME_HINTS),
                "supported": supported,
            })
        AudioCaptureService._loopback_cache = result
        AudioCaptureService._loopback_cache_t = now
        return result

    @property
    def live_rms(self) -> float:
        return self._live_rms

    @property
    def queue_drops(self) -> int:
        return self._queue_drops

    def get_dsp_state(self) -> dict | None:
        """Return current DSP state for status reporting."""
        if self._dsp_chain is not None:
            return self._dsp_chain.get_state()
        return None

    def set_wav_recorder(self, recorder):
        """Set or clear the WAV recorder reference (called from app.py)."""
        self._wav_recorder = recorder

    def set_record_source(self, pre_dsp: bool):
        """Toggle WAV recording source between pre-DSP (raw) and post-DSP (processed)."""
        self._record_pre_dsp = pre_dsp

    def set_spectrum_source(self, pre_dsp: bool):
        """Toggle spectrum between pre-DSP (raw) and post-DSP (processed)."""
        with self._ring_lock:
            self._spectrum_pre_dsp = pre_dsp

    @staticmethod
    def resample(chunks: list[np.ndarray]) -> np.ndarray:
        """Concatenate audio chunks and resample from RECORD_RATE to WHISPER_RATE."""
        audio = np.concatenate(chunks, axis=0).astype(np.float32).squeeze()
        return resample_poly(audio, up=1, down=RECORD_RATE // WHISPER_RATE)
