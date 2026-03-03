#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

struct DSPGateState {
    bool enabled = false;
    bool gate_open = false;
    float input_dbfs = -80.0f;
    float output_dbfs = -80.0f;
    float current_gain_db = 0.0f;
    float attenuation_db = 0.0f;
    float open_threshold_dbfs = -45.0f;
    float close_threshold_dbfs = -50.0f;
    float floor_db = -25.0f;
    float hold_ms = 100.0f;
    float attack_ms = 5.0f;
    float release_ms = 150.0f;
    bool calibrating = false;
    float calibrated_noise_floor_dbfs = -80.0f;
};

struct DSPCompressorState {
    bool enabled = false;
    float threshold_dbfs = -15.0f;
    float ratio = 2.0f;
    float gain_reduction_db = 0.0f;
    float attack_ms = 5.0f;
    float release_ms = 100.0f;
    float makeup_gain_db = 0.0f;
};

struct EngineStatus {
    bool connected = false;
    std::string phase = "unknown";
    std::string current_mode;
    std::string current_profile;
    std::string last_raw_transcript;
    std::string last_cleaned_text;
    float audio_rms = 0.0f;
    float uptime_s = 0.0f;
    std::string version;
    std::string last_error;

    bool approval_mode = false;
    bool push_to_talk = false;
    std::string pending_text;
    bool recording = false;
    std::string hotkey;

    std::vector<std::string> mode_names;
    std::vector<std::string> profile_names;

    std::vector<float> fft_bins;
    bool is_speech = false;

    struct InputDevice {
        int index = -1;
        std::string name;
        bool is_default = false;
    };
    std::vector<InputDevice> input_devices;
    int mic_device_index = -1;
    std::string mic_device_name;

    struct Latency {
        float record_ms = 0.0f;
        float transcribe_ms = 0.0f;
        float cleanup_ms = 0.0f;
        float type_ms = 0.0f;
    } latency;

    bool has_dsp = false;
    DSPGateState gate;
    DSPCompressorState compressor;
    bool spectrum_pre_dsp = false;
};

class EngineClient {
public:
    EngineClient(const std::string& host, int port);
    ~EngineClient();

    void StartPolling();
    void StopPolling();

    EngineStatus GetStatus() const;
    bool IsConnected() const;

    bool PollHealthOnce();  // synchronous single-shot health check

    bool Toggle();
    bool Start();
    bool Stop();
    bool SetMode(const std::string& mode);
    bool SetProfile(const std::string& profile);
    bool SendCommand(const std::string& cmd);
    bool Shutdown();

    bool SetApprovalMode(bool enabled);
    bool SetPushToTalk(bool enabled);
    bool ApprovePending();
    bool EditPending(const std::string& text);
    bool RejectPending();
    bool SetHotkey(const std::string& key);
    bool SetMicDevice(int device_index);

    bool PostDSPConfig(const std::string& json_body);
    bool StartCalibration();
    bool FinishCalibration();
    bool SetSpectrumSource(bool pre_dsp);

private:
    void PollLoop();
    bool PollHealth();
    bool PollStatus();

    std::string m_host;
    int m_port;

    mutable std::mutex m_mutex;
    EngineStatus m_status;

    std::atomic<bool> m_running{false};
    std::thread m_pollThread;
};
