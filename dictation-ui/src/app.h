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

    // VU meter peak hold
    float m_vuPeakDbfs = -80.0f;

    // Animation state
    float m_stateColor[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    std::string m_lastOutputText;
    double m_outputFlashTime = 0.0;
};
