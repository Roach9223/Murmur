Murmur — Local Voice Dictation
===============================

Press a hotkey (or click the recording banner), speak naturally, and clean
text gets typed into any app. Fully local — runs on your GPU with Whisper
and optional LLM cleanup via LM Studio.


REQUIREMENTS
------------
- NVIDIA GPU with CUDA support
- LM Studio running at localhost:1234 with a model loaded (for LLM cleanup modes)
  Raw mode works without LM Studio.
- A microphone


GETTING STARTED
---------------
1. Launch Murmur.exe — it starts the engine automatically.
2. Press F1 (default hotkey) or click the recording banner to start.
3. Speak naturally. The app transcribes when you pause.
4. Press F1 again (or click the banner) to stop recording.
5. Text is typed into whatever app has focus.

Change the hotkey via Edit > Change Hotkey in the menu bar.
Change the microphone via Edit > Microphone in the menu bar.


MODES
-----
Switch modes via Edit > Mode in the menu bar.

  Raw     No LLM processing. Whisper output typed as-is. Fastest.
  Clean   Removes filler words (um, uh, like), fixes grammar. Default.
  Prompt  Restructures speech into clear prompts for AI input.
  Dev     Converts speech into bullet points and task lists.

Clean/Prompt/Dev require LM Studio to be running with a model loaded.


PROFILES
--------
Switch profiles via Edit > Profile in the menu bar.

Profiles bundle a mode with optional overrides (voice commands, hotkey).
For example, the "LM Studio" profile uses Prompt mode and maps "send"
to Ctrl+Enter instead of Enter.

Available profiles:
  Default    Clean mode
  Terminal   Raw mode (no LLM)
  LM Studio  Prompt mode, Ctrl+Enter for send
  VS Code    Dev mode (task lists)
  Meeting    Clean mode

Auto-detect (when enabled) automatically switches profiles based on
the active window title. Configure rules in config.json.


VOICE COMMANDS
--------------
Voice commands require the prefix word "command" before the phrase.
This prevents accidental triggering during normal speech.

  "command new line"        Press Enter
  "command send"            Press Enter (Ctrl+Enter in some profiles)
  "command clear"           Select all and delete
  "command stop dictation"  Stop recording

Without the prefix, phrases like "new line" or "send" are typed as
regular text. The prefix is configurable via "command_prefix" in
config.json (set to empty string "" to disable).


FEATURES
--------

Approval Mode
  Toggle via the "Approval" button in the main window. When enabled,
  transcribed text is shown for review before being typed. You can
  approve, edit the text, or reject each chunk.

Push-to-Talk
  Toggle via the "Push-to-Talk" button in the main window. Hold the
  hotkey to record, release to stop. Useful for short commands or
  noisy environments.


DSP AUDIO PROCESSING
--------------------
Murmur processes your microphone audio in real time before it reaches
Whisper. This happens automatically — you can hear the effect in the
spectrum visualizer.

Noise Gate
  Reduces background noise between speech. When you're not speaking,
  the gate attenuates the signal to a configurable floor level (not
  full mute — this preserves natural room tone).

  Controls in the Processing section:
    Gate ON/OFF     Enable/disable the noise gate
    Open Threshold  Signal level (dBFS) that opens the gate
    Close Threshold Signal level (dBFS) that closes the gate
    Floor           How much attenuation when gate is closed (dB)
    Auto Calibrate  Measures your room noise for 1.5s and sets
                    thresholds automatically — stay silent during
                    calibration

  The open threshold must be at least 3 dB above the close threshold.
  This hysteresis gap prevents the gate from chattering at the boundary.

Compressor (optional)
  Tames loud peaks for more consistent levels. Useful if you vary your
  speaking distance or volume. Disabled by default.

  Controls:
    Comp ON/OFF     Enable/disable the compressor
    Threshold       Level above which compression starts (dBFS)
    Ratio           Compression ratio (2:1, 4:1, etc.)
    Makeup Gain     Compensate for gain reduction (dB)

Spectrum Visualizer
  Shows a 64-bin real-time frequency spectrum (50 Hz to 12 kHz).
  Colors change based on engine state. Toggle between "Post DSP"
  (after noise gate/compressor) and "Pre DSP" (raw mic input) to
  see what the DSP chain is doing.


CONFIGURATION
-------------
Edit config.json (in this folder) to change:
  - Hotkey, mic device, Whisper model size
  - Silence timeout, energy threshold, max speech duration
  - LLM model, modes, profiles, auto-detect rules
  - Voice command phrases
  - Noise gate and compressor parameters

DSP slider changes are saved automatically. Other config changes
take effect on next engine restart (File > Restart Engine).


TROUBLESHOOTING
---------------
Engine disconnected?
  Click File > Restart Engine or the Restart button on the main screen.

LLM cleanup not working?
  Make sure LM Studio is running at localhost:1234 with a model loaded.
  If LM Studio is down, the app falls back to raw Whisper output.

Audio not detected?
  Try a different microphone via Edit > Microphone. The level bar
  in the Processing section shows your current input level in dBFS.

Gate not opening?
  Your open threshold may be too high. Click "Auto Calibrate" — stay
  silent for 1.5 seconds and the gate will set thresholds based on
  your room's noise floor. Or manually lower the open threshold slider.

Text not appearing in target app?
  Make sure the target app has focus before the text is typed.
  Some apps may block programmatic input.


LICENSE
-------
MIT License. See LICENSE file for details.
