#include "engine_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

EngineClient::EngineClient(const std::string& host, int port)
    : m_host(host), m_port(port) {}

EngineClient::~EngineClient() {
    StopPolling();
}

void EngineClient::StartPolling() {
    m_running = true;
    m_pollThread = std::thread(&EngineClient::PollLoop, this);
}

void EngineClient::StopPolling() {
    m_running = false;
    if (m_pollThread.joinable())
        m_pollThread.join();
}

EngineStatus EngineClient::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool EngineClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status.connected;
}

void EngineClient::PollLoop() {
    while (m_running) {
        bool health_ok = PollHealth();

        if (health_ok) {
            PollStatus();
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.connected = false;
        }

        // Poll every 50ms for responsive spectrum updates
        for (int i = 0; i < 5 && m_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool EngineClient::PollHealth() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(1);

    auto res = cli.Get("/health");
    if (res && res->status == 200) {
        auto j = json::parse(res->body, nullptr, false);
        if (!j.is_discarded()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.connected = true;
            m_status.version = j.value("version", std::string{});
            m_status.uptime_s = j.value("uptime_s", 0.0f);
            return true;
        }
    }
    return false;
}

bool EngineClient::PollStatus() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(2);

    auto res = cli.Get("/status");
    if (res && res->status == 200) {
        auto j = json::parse(res->body, nullptr, false);
        if (!j.is_discarded()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.phase = j.value("state", std::string{"unknown"});
            m_status.current_mode = j.value("current_mode", std::string{});
            m_status.current_profile = j.value("current_profile", std::string{});
            m_status.last_raw_transcript = j.value("last_raw_transcript", std::string{});
            m_status.last_cleaned_text = j.value("last_cleaned_text", std::string{});
            m_status.audio_rms = j.value("audio_rms", 0.0f);

            auto err = j.value("errors", json(nullptr));
            m_status.last_error = err.is_string() ? err.get<std::string>() : "";

            m_status.approval_mode = j.value("approval_mode", false);
            m_status.push_to_talk = j.value("push_to_talk", false);
            m_status.pending_text = j.value("pending_text", std::string{});
            m_status.recording = j.value("recording", false);
            m_status.model_loading = j.value("model_loading", false);
            m_status.hotkey = j.value("hotkey", std::string{"f1"});

            m_status.mode_names.clear();
            if (j.contains("mode_names") && j["mode_names"].is_array())
                for (auto& m : j["mode_names"]) m_status.mode_names.push_back(m.get<std::string>());
            m_status.profile_names.clear();
            if (j.contains("profile_names") && j["profile_names"].is_array())
                for (auto& p : j["profile_names"]) m_status.profile_names.push_back(p.get<std::string>());

            m_status.fft_bins.clear();
            if (j.contains("fft_bins") && j["fft_bins"].is_array())
                for (auto& b : j["fft_bins"]) m_status.fft_bins.push_back(b.get<float>());
            m_status.is_speech = j.value("is_speech", false);

            m_status.input_devices.clear();
            if (j.contains("input_devices") && j["input_devices"].is_array()) {
                for (auto& d : j["input_devices"]) {
                    EngineStatus::InputDevice dev;
                    dev.index = d.value("index", -1);
                    dev.name = d.value("name", std::string{});
                    dev.is_default = d.value("is_default", false);
                    m_status.input_devices.push_back(dev);
                }
            }
            m_status.mic_device_index = j.value("mic_device_index", -1);
            m_status.mic_device_name = j.value("mic_device_name", std::string{});

            if (j.contains("latency_ms")) {
                auto& lat = j["latency_ms"];
                m_status.latency.record_ms = lat.value("record", 0.0f);
                m_status.latency.transcribe_ms = lat.value("transcribe", 0.0f);
                m_status.latency.cleanup_ms = lat.value("cleanup", 0.0f);
                m_status.latency.type_ms = lat.value("type", 0.0f);
            }

            m_status.cleanup_backend = j.value("cleanup_backend", std::string{"lmstudio"});
            m_status.cleanup_backend_url = j.value("cleanup_backend_url", std::string{});
            if (j.contains("cleanup_backend_urls") && j["cleanup_backend_urls"].is_object()) {
                auto& u = j["cleanup_backend_urls"];
                m_status.cleanup_backend_urls.lmstudio = u.value("lmstudio", std::string{});
                m_status.cleanup_backend_urls.llamacpp = u.value("llamacpp", std::string{});
            }

            m_status.spectrum_pre_dsp = j.value("spectrum_pre_dsp", false);

            // WAV recording status
            if (j.contains("wav_recording") && j["wav_recording"].is_object()) {
                auto& wr = j["wav_recording"];
                m_status.wav_recording.active = wr.value("active", false);
                auto wrp = wr.value("path", nlohmann::json(nullptr));
                m_status.wav_recording.path = wrp.is_string() ? wrp.get<std::string>() : "";
                m_status.wav_recording.seconds = wr.value("seconds", 0.0f);
                m_status.wav_recording.dropped_frames = wr.value("dropped_frames", 0);
                m_status.wav_recording.source = wr.value("source", std::string{"post"});
            }
            m_status.ffmpeg_available = j.value("ffmpeg_available", false);
            m_status.recordings_dir = j.value("recordings_dir", std::string{});
            auto lrp = j.value("last_recording_path", json(nullptr));
            m_status.last_recording_path = lrp.is_string() ? lrp.get<std::string>() : "";

            // File transcription (Audio to Text)
            if (j.contains("file_transcription") && j["file_transcription"].is_object()) {
                auto& ft = j["file_transcription"];
                m_status.file_transcription.active = ft.value("active", false);
                m_status.file_transcription.status = ft.value("status", std::string{"idle"});
                m_status.file_transcription.input_path = ft.value("input_path", std::string{});
                m_status.file_transcription.output_path = ft.value("output_path", std::string{});
                m_status.file_transcription.error = ft.value("error", std::string{});
                m_status.file_transcription.progress = ft.value("progress", 0.0f);
            }
            m_status.transcripts_dir = j.value("transcripts_dir", std::string{});

            if (j.contains("dsp") && j["dsp"].is_object()) {
                m_status.has_dsp = true;
                auto& dsp = j["dsp"];
                if (dsp.contains("gate") && dsp["gate"].is_object()) {
                    auto& g = dsp["gate"];
                    m_status.gate.enabled = g.value("enabled", false);
                    m_status.gate.gate_open = g.value("gate_open", false);
                    m_status.gate.input_dbfs = g.value("input_dbfs", -80.0f);
                    m_status.gate.output_dbfs = g.value("output_dbfs", -80.0f);
                    m_status.gate.current_gain_db = g.value("current_gain_db", 0.0f);
                    m_status.gate.attenuation_db = g.value("attenuation_db", 0.0f);
                    m_status.gate.open_threshold_dbfs = g.value("open_threshold_dbfs", -45.0f);
                    m_status.gate.close_threshold_dbfs = g.value("close_threshold_dbfs", -50.0f);
                    m_status.gate.floor_db = g.value("floor_db", -25.0f);
                    m_status.gate.hold_ms = g.value("hold_ms", 100.0f);
                    m_status.gate.attack_ms = g.value("attack_ms", 5.0f);
                    m_status.gate.release_ms = g.value("release_ms", 150.0f);
                    m_status.gate.calibrating = g.value("calibrating", false);
                    m_status.gate.speech_calibrating = g.value("speech_calibrating", false);
                    m_status.gate.calibrated_noise_floor_dbfs = g.value("calibrated_noise_floor_dbfs", -80.0f);
                    m_status.gate.calibrated_speech_dbfs = g.value("calibrated_speech_dbfs", -80.0f);
                }
                if (dsp.contains("compressor") && dsp["compressor"].is_object()) {
                    auto& c = dsp["compressor"];
                    m_status.compressor.enabled = c.value("enabled", false);
                    m_status.compressor.threshold_dbfs = c.value("threshold_dbfs", -15.0f);
                    m_status.compressor.ratio = c.value("ratio", 2.0f);
                    m_status.compressor.gain_reduction_db = c.value("gain_reduction_db", 0.0f);
                    m_status.compressor.attack_ms = c.value("attack_ms", 5.0f);
                    m_status.compressor.release_ms = c.value("release_ms", 100.0f);
                    m_status.compressor.makeup_gain_db = c.value("makeup_gain_db", 0.0f);
                }
            } else {
                m_status.has_dsp = false;
            }

            return true;
        }
    }
    return false;
}

bool EngineClient::PollHealthOnce() {
    // Quick probe with short timeout — used at startup before render loop
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(0, 300000);  // 300ms
    cli.set_read_timeout(0, 300000);

    auto res = cli.Get("/health");
    if (res && res->status == 200) {
        auto j = json::parse(res->body, nullptr, false);
        if (!j.is_discarded()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.connected = true;
            m_status.version = j.value("version", std::string{});
            m_status.uptime_s = j.value("uptime_s", 0.0f);
            return true;
        }
    }
    return false;
}

// --- Command methods ---

bool EngineClient::Toggle() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/control/toggle");
    return res && res->status == 200;
}

bool EngineClient::Start() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/control/start");
    return res && res->status == 200;
}

bool EngineClient::Stop() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/control/stop");
    return res && res->status == 200;
}

bool EngineClient::SetMode(const std::string& mode) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"mode", mode}};
    auto res = cli.Post("/control/set_mode", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::SetProfile(const std::string& profile) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"profile", profile}};
    auto res = cli.Post("/control/set_profile", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::SendCommand(const std::string& cmd) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"cmd", cmd}};
    auto res = cli.Post("/control/command", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::Shutdown() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/engine/shutdown");
    return res && res->status == 200;
}

bool EngineClient::SetApprovalMode(bool enabled) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"enabled", enabled}};
    auto res = cli.Post("/control/set_approval_mode", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::SetPushToTalk(bool enabled) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"enabled", enabled}};
    auto res = cli.Post("/control/set_push_to_talk", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::ApprovePending() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/control/approve");
    return res && res->status == 200;
}

bool EngineClient::EditPending(const std::string& text) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"text", text}};
    auto res = cli.Post("/control/edit", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::RejectPending() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/control/reject");
    return res && res->status == 200;
}

bool EngineClient::SetHotkey(const std::string& key) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"hotkey", key}};
    auto res = cli.Post("/control/set_hotkey", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::SetMicDevice(int device_index) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    json body = {{"device_index", device_index}};
    auto res = cli.Post("/control/set_mic", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::SetLLMBackend(const std::string& type) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"type", type}};
    auto res = cli.Post("/control/set_llm_backend", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::SetLLMBackendURL(const std::string& url) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"url", url}};
    auto res = cli.Post("/control/set_llm_backend_url", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::PostDSPConfig(const std::string& json_body) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/config", json_body, "application/json");
    return res && res->status == 200;
}

bool EngineClient::StartCalibration() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"action", "start"}};
    auto res = cli.Post("/dsp/calibrate", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::FinishSilenceCalibration() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    json body = {{"action", "finish_silence"}};
    auto res = cli.Post("/dsp/calibrate", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::StartSpeechCalibration() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"action", "start_speech"}};
    auto res = cli.Post("/dsp/calibrate", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::FinishCalibration() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    json body = {{"action", "finish"}};
    auto res = cli.Post("/dsp/calibrate", body.dump(), "application/json");
    return res && res->status == 200;
}

std::string EngineClient::FetchCalibrationPrompt() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(8);
    auto res = cli.Get("/calibrate/prompt");
    if (res && res->status == 200) {
        auto j = json::parse(res->body, nullptr, false);
        if (!j.is_discarded() && j.contains("prompt"))
            return j["prompt"].get<std::string>();
    }
    return "The quick brown fox jumps over the lazy dog near the river bank.";
}

bool EngineClient::SetSpectrumSource(bool pre_dsp) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"spectrum_source", pre_dsp ? "pre" : "post"}};
    auto res = cli.Post("/config", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::StartWavRecording(const std::string& source) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    json body = {{"source", source}};
    auto res = cli.Post("/record/start", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::StopWavRecording() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/record/stop");
    return res && res->status == 200;
}

bool EngineClient::ExportMP3(const std::string& wav_path) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(60);
    json body = {{"wav_path", wav_path}};
    auto res = cli.Post("/record/export_mp3", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::TranscribeFile(const std::string& path, std::string& out_error) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    json body = {{"path", path}};
    auto res = cli.Post("/transcribe/file", body.dump(), "application/json");
    if (!res) {
        out_error = "Connection error (httplib error " + std::to_string((int)res.error()) + ")";
        return false;
    }
    if (res->status != 200) {
        out_error = "HTTP " + std::to_string(res->status) + ": " + res->body;
        return false;
    }
    return true;
}

bool EngineClient::SaveTranscription(const std::string& format, const std::string& style, const std::string& filename) {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);
    json body = {{"format", format}, {"style", style}};
    if (!filename.empty())
        body["filename"] = filename;
    auto res = cli.Post("/transcribe/save", body.dump(), "application/json");
    return res && res->status == 200;
}

bool EngineClient::ResetSave() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/transcribe/reset-save");
    return res && res->status == 200;
}

bool EngineClient::ClearLogs() {
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Post("/logs/clear");
    return res && res->status == 200;
}

std::vector<std::string> EngineClient::FetchLogTail(int n) {
    std::vector<std::string> lines;
    httplib::Client cli(m_host, m_port);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(2);
    std::string path = "/logs/tail?n=" + std::to_string(n);
    auto res = cli.Get(path);
    if (res && res->status == 200) {
        auto j = json::parse(res->body, nullptr, false);
        if (!j.is_discarded() && j.contains("lines") && j["lines"].is_array()) {
            for (auto& line : j["lines"])
                if (line.is_string())
                    lines.push_back(line.get<std::string>());
        }
    }
    return lines;
}
