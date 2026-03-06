#pragma once

#include "engine_client.h"
#include "engine_process.h"

class DictationApp {
public:
    DictationApp(EngineClient& engine, EngineProcess& process);
    void Render();
    bool ShouldQuit() const { return m_requestQuit; }

private:
    EngineClient& m_engine;
    EngineProcess& m_process;

    char m_editBuffer[4096] = {};
    bool m_editActive = false;
    bool m_capturingHotkey = false;
    bool m_showAbout = false;
    bool m_showInstructions = false;
    bool m_requestQuit = false;

    // LLM Backend UI state
    char m_backendUrlBuf[256] = {};
    bool m_editingBackendUrl = false;
    std::string m_lastSeenBackend;

    // Spectrum visualizer state (matches Python N_BINS=128)
    static constexpr int kSpectrumBins = 128;
    float m_smoothBins[kSpectrumBins] = {};
    float m_peakBins[kSpectrumBins] = {};
    float m_noiseFloor[kSpectrumBins] = {};
    double m_lastFrameTime = 0.0;

    // DSP meter smoothing
    float m_smoothInputDbfs = -80.0f;
    float m_smoothOutputDbfs = -80.0f;
    float m_smoothGR = 0.0f;

    // Calibration UI state
    double m_calStartTime = 0.0;
    bool m_calWaiting = false;
    bool m_dspLocked = true;  // Prevent accidental slider changes

    // Calibration popup state
    bool m_showCalPopup = false;
    int m_calPhase = 0;          // 0=silence, 1=speech, 2=done, -1=error
    std::string m_calPrompt;     // LLM sentence for user to read
    float m_calNoiseFloor = 0;
    float m_calSpeechLevel = 0;
    float m_calOpenThresh = 0;
    float m_calCloseThresh = 0;
    std::string m_calError;

    // VU meter peak hold
    float m_vuPeakDbfs = -80.0f;

    // Animation state
    float m_stateColor[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    std::string m_lastOutputText;
    double m_outputFlashTime = 0.0;

    // WAV recording UI state
    double m_wavRecordStartTime = 0.0;
    std::string m_lastRecordingPath;
    int m_recordSourceIdx = 0;  // 0=Post-DSP, 1=Pre-DSP

    // Audio to Text UI state
    std::string m_lastTranscriptStatus;  // "Done! ..." or error
    double m_transcriptFlashTime = 0.0;
    bool m_showTranscriptionPopup = false;
    int m_saveStyleIdx = 2;  // 0=Raw, 1=Clean, 2=Detailed, 3=Summarize
    char m_saveFilename[256] = "";
    std::vector<std::string> m_transcriptLogLines;
    double m_lastLogFetchTime = 0.0;
};
