#include "app.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <cctype>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

// Semibold font for the voice-to-type banner (defined in main.cpp)
extern ImFont* g_bannerFont;

DictationApp::DictationApp(EngineClient& engine, EngineProcess& process)
    : m_engine(engine), m_process(process) {}

void DictationApp::Render()
{
    auto status = m_engine.GetStatus();

    // Frame delta time (used for animation smoothing)
    double now = ImGui::GetTime();
    float dt = (m_lastFrameTime > 0.0) ? (float)(now - m_lastFrameTime) : 1.0f / 60.0f;
    if (dt > 0.1f) dt = 0.1f;  // clamp to avoid jumps after stalls
    m_lastFrameTime = now;

    // Full-viewport window with menu bar
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_MenuBar);

    // ===================== Menu Bar =====================
    if (ImGui::BeginMenuBar()) {
        // --- File menu ---
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("View Log")) {
                std::string logPath = EngineProcess::DiscoverEngineDir() + "\\logs\\dictation.log";
                ShellExecuteA(NULL, "open", logPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem("Clear Log")) {
                m_engine.ClearLogs();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Restart Engine")) {
                m_process.Terminate(2000);
                m_process.Launch();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                m_requestQuit = true;
            }
            ImGui::EndMenu();
        }

        // --- Edit menu ---
        if (ImGui::BeginMenu("Edit")) {
            // Mode submenu
            if (ImGui::BeginMenu("Mode")) {
                for (const auto& mode : status.mode_names) {
                    bool selected = (mode == status.current_mode);
                    if (ImGui::MenuItem(mode.c_str(), nullptr, selected)) {
                        m_engine.SetMode(mode);
                    }
                }
                ImGui::EndMenu();
            }

            // Profile submenu
            if (ImGui::BeginMenu("Profile")) {
                for (const auto& profile : status.profile_names) {
                    bool selected = (profile == status.current_profile);
                    if (ImGui::MenuItem(profile.c_str(), nullptr, selected)) {
                        m_engine.SetProfile(profile);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // Change Hotkey
            if (m_capturingHotkey) {
                ImGui::MenuItem("Press any key...", nullptr, false, false);
            } else {
                std::string hkUpper = status.hotkey;
                for (auto& c : hkUpper) c = (char)toupper((unsigned char)c);
                char label[64];
                snprintf(label, sizeof(label), "Change Hotkey [%s]", hkUpper.c_str());
                if (ImGui::MenuItem(label)) {
                    m_capturingHotkey = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click, then press any keyboard key to set as the new hotkey");
            }

            ImGui::Separator();

            // Microphone submenu
            if (ImGui::BeginMenu("Microphone")) {
                for (const auto& dev : status.input_devices) {
                    ImGui::PushID(dev.index);
                    char devLabel[256];
                    if (dev.is_default)
                        snprintf(devLabel, sizeof(devLabel), "%s (Default)", dev.name.c_str());
                    else
                        snprintf(devLabel, sizeof(devLabel), "%s", dev.name.c_str());

                    bool selected = (dev.index == status.mic_device_index);
                    if (ImGui::MenuItem(devLabel, nullptr, selected)) {
                        if (dev.index != status.mic_device_index) {
                            m_engine.SetMicDevice(dev.index);
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // LLM Backend submenu
            if (ImGui::BeginMenu("LLM Backend")) {
                // Backend type radio selection
                bool isLmStudio = (status.cleanup_backend == "lmstudio" || status.cleanup_backend.empty());
                bool isLlamaCpp = (status.cleanup_backend == "llamacpp");

                if (ImGui::MenuItem("LM Studio", nullptr, isLmStudio)) {
                    if (!isLmStudio) {
                        m_engine.SetLLMBackend("lmstudio");
                        strncpy(m_backendUrlBuf, status.cleanup_backend_urls.lmstudio.c_str(), sizeof(m_backendUrlBuf) - 1);
                        m_backendUrlBuf[sizeof(m_backendUrlBuf) - 1] = '\0';
                    }
                }
                if (ImGui::MenuItem("llama.cpp", nullptr, isLlamaCpp)) {
                    if (!isLlamaCpp) {
                        m_engine.SetLLMBackend("llamacpp");
                        strncpy(m_backendUrlBuf, status.cleanup_backend_urls.llamacpp.c_str(), sizeof(m_backendUrlBuf) - 1);
                        m_backendUrlBuf[sizeof(m_backendUrlBuf) - 1] = '\0';
                    }
                }

                ImGui::Separator();

                // Sync URL buffer when backend changes externally or on first frame
                if (status.cleanup_backend != m_lastSeenBackend || !m_editingBackendUrl) {
                    strncpy(m_backendUrlBuf, status.cleanup_backend_url.c_str(), sizeof(m_backendUrlBuf) - 1);
                    m_backendUrlBuf[sizeof(m_backendUrlBuf) - 1] = '\0';
                    m_lastSeenBackend = status.cleanup_backend;
                }

                // Endpoint URL input
                ImGui::Text("Endpoint:");
                ImGui::SetNextItemWidth(350);
                if (ImGui::InputText("##BackendURL", m_backendUrlBuf, sizeof(m_backendUrlBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    m_engine.SetLLMBackendURL(m_backendUrlBuf);
                    m_editingBackendUrl = false;
                }
                m_editingBackendUrl = ImGui::IsItemActive();

                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        // --- Help menu ---
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Instructions")) {
                m_showInstructions = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About")) {
                m_showAbout = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    // Handle hotkey capture (runs outside menu so it catches key presses)
    if (m_capturingHotkey) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.1f, 1.0f),
                           ">> Press any key or click a mouse button <<");
        // Keyboard keys (up to MouseLeft boundary)
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_MouseLeft; key++) {
            if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                const char* name = ImGui::GetKeyName((ImGuiKey)key);
                if (name && name[0] != '\0') {
                    std::string keyLower = name;
                    for (auto& c : keyLower) c = (char)tolower((unsigned char)c);
                    m_engine.SetHotkey(keyLower);
                    m_capturingHotkey = false;
                }
                break;
            }
        }
        // Mouse side buttons (middle, X1, X2 — left/right excluded)
        if (m_capturingHotkey) {
            struct { ImGuiKey key; const char* name; } mouseButtons[] = {
                { ImGuiKey_MouseMiddle, "mouse_middle" },
                { ImGuiKey_MouseX1, "mouse_x1" },
                { ImGuiKey_MouseX2, "mouse_x2" },
            };
            for (auto& mb : mouseButtons) {
                if (ImGui::IsKeyPressed(mb.key)) {
                    m_engine.SetHotkey(mb.name);
                    m_capturingHotkey = false;
                    break;
                }
            }
        }
    }

    // --- Connection status ---
    if (status.connected) {
        if (status.model_loading) {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "ENGINE: LOADING MODEL...");
            ImGui::SameLine();
            ImGui::TextDisabled("v%s  |  uptime %.0fs", status.version.c_str(), status.uptime_s);
        } else {
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "ENGINE: CONNECTED");
            ImGui::SameLine();
            ImGui::TextDisabled("v%s  |  uptime %.0fs", status.version.c_str(), status.uptime_s);
        }
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "ENGINE: DISCONNECTED");
        ImGui::SameLine();
        ImGui::TextDisabled("Waiting for engine on 127.0.0.1:8899...");
    }

    ImGui::Separator();

    if (!status.connected) {
        auto procState = m_process.GetState();
        switch (procState) {
        case EngineProcess::State::LAUNCHING:
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f), "LAUNCHING ENGINE...");
            break;
        case EngineProcess::State::RUNNING:
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.1f, 1.0f),
                               "Engine process started, waiting for connection...");
            break;
        case EngineProcess::State::FAILED:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Engine launch failed:");
            ImGui::TextWrapped("%s", m_process.GetError().c_str());
            break;
        default:
            ImGui::TextWrapped("Engine not running.");
            break;
        }

        ImGui::Spacing();
        if (ImGui::Button("Restart Engine")) {
            m_process.Terminate(2000);
            m_process.Launch();
        }

        ImGui::End();
        return;
    }

    // --- Voice-to-Type banner (hero button) ---
    {
        std::string hkUpper = status.hotkey;
        for (auto& c : hkUpper) c = (char)toupper((unsigned char)c);

        ImGui::PushFont(g_bannerFont);
        if (status.model_loading) {
            float pulse = 0.4f + 0.15f * (float)sin(ImGui::GetTime() * 3.0);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(pulse, pulse * 0.7f, 0.05f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(pulse, pulse * 0.7f, 0.05f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(pulse, pulse * 0.7f, 0.05f, 1.0f));
            ImGui::Button("Loading Whisper Model...", ImVec2(-1, 42));
            ImGui::PopStyleColor(3);
        } else if (status.recording) {
            float pulse = 0.5f + 0.3f * (float)sin(ImGui::GetTime() * 4.0);
            ImVec4 recCol(pulse, 0.08f, 0.08f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, recCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.05f, 0.05f, 1.0f));
            char label[128];
            snprintf(label, sizeof(label), "Voice to Type  \xc2\xb7  Active  (%s to stop)", hkUpper.c_str());
            if (ImGui::Button(label, ImVec2(-1, 42)))
                m_engine.Stop();
            ImGui::PopStyleColor(3);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            char label[128];
            snprintf(label, sizeof(label), "Voice to Type  \xc2\xb7  Press %s to start", hkUpper.c_str());
            if (ImGui::Button(label, ImVec2(-1, 42)))
                m_engine.Start();
            ImGui::PopStyleColor(3);
        }
        ImGui::PopFont();
    }

    // --- State + Profile ---
    ImGui::Text("State:");
    ImGui::SameLine();

    ImVec4 targetColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // idle: muted gray
    if (status.phase == "listening")              targetColor = ImVec4(0.3f, 0.5f, 1.0f, 1.0f);   // blue
    else if (status.phase == "recording")         targetColor = ImVec4(1.0f, 0.6f, 0.1f, 1.0f);   // orange
    else if (status.phase == "transcribing")      targetColor = ImVec4(0.7f, 0.4f, 1.0f, 1.0f);   // purple
    else if (status.phase == "cleaning")          targetColor = ImVec4(0.2f, 0.9f, 0.9f, 1.0f);   // cyan
    else if (status.phase == "typing")            targetColor = ImVec4(0.2f, 0.9f, 0.3f, 1.0f);   // green
    else if (status.phase == "pending_approval")  targetColor = ImVec4(1.0f, 0.8f, 0.1f, 1.0f);   // yellow
    else if (status.phase == "error")             targetColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);   // red

    // Smooth color transition (150ms half-life)
    float fadeAlpha = 1.0f - powf(0.5f, dt / 0.15f);
    m_stateColor[0] += fadeAlpha * (targetColor.x - m_stateColor[0]);
    m_stateColor[1] += fadeAlpha * (targetColor.y - m_stateColor[1]);
    m_stateColor[2] += fadeAlpha * (targetColor.z - m_stateColor[2]);

    ImGui::TextColored(ImVec4(m_stateColor[0], m_stateColor[1], m_stateColor[2], 1.0f),
                       "%s", status.phase.c_str());

    ImGui::Text("Profile: %s  |  Mode: %s",
                status.current_profile.c_str(),
                status.current_mode.c_str());

    // --- Feature toggles ---
    ImGui::Separator();
    {
        // Approval Mode toggle button
        if (status.approval_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.45f, 0.10f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        }
        if (ImGui::Button(status.approval_mode ? "Approval: ON" : "Approval: OFF", ImVec2(140, 25))) {
            m_engine.SetApprovalMode(!status.approval_mode);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Review text before typing into active window");

        ImGui::SameLine(0, 12);

        // Push-to-Talk toggle button
        if (status.push_to_talk) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.25f, 0.55f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        }
        if (ImGui::Button(status.push_to_talk ? "Push-to-Talk: ON" : "Push-to-Talk: OFF", ImVec2(160, 25))) {
            m_engine.SetPushToTalk(!status.push_to_talk);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hold hotkey to record, release to stop");

        // --- Record / Audio to Text / Folder (right side of same row) ---
        ImGui::SameLine(ImGui::GetWindowWidth() - 310);

        // Record button
        bool wavActive = status.wav_recording.active;
        if (wavActive) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.08f, 0.08f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.4f, 0.05f, 0.05f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        }
        if (ImGui::Button(wavActive ? "Stop Rec" : "Record", ImVec2(80, 25))) {
            if (wavActive) {
                m_engine.StopWavRecording();
            } else {
                const char* sources[] = { "post", "pre" };
                m_engine.StartWavRecording(sources[m_recordSourceIdx]);
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(wavActive ? "Stop WAV recording" : "Record mic audio to WAV file");

        ImGui::SameLine(0, 6);

        // Audio to Text button
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.55f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.45f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.25f, 0.45f, 1.0f));
        bool a2tDisabled = status.file_transcription.active || status.model_loading;
        if (a2tDisabled) ImGui::BeginDisabled();
        if (ImGui::Button("Audio to Text", ImVec2(120, 25))) {
            wchar_t szFile[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = L"Audio Files\0*.wav;*.mp3;*.flac;*.m4a;*.ogg\0All Files\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            std::wstring initDir;
            if (!status.recordings_dir.empty()) {
                initDir.assign(status.recordings_dir.begin(), status.recordings_dir.end());
                ofn.lpstrInitialDir = initDir.c_str();
            }
            if (GetOpenFileNameW(&ofn)) {
                int len = WideCharToMultiByte(CP_UTF8, 0, szFile, -1, nullptr, 0, nullptr, nullptr);
                std::string path(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, szFile, -1, path.data(), len, nullptr, nullptr);
                std::string a2tError;
                bool ok = m_engine.TranscribeFile(path, a2tError);
                m_showTranscriptionPopup = true;
                m_lastTranscriptStatus.clear();
                if (!ok) {
                    m_lastTranscriptStatus = "Error: " + a2tError;
                }
            }
        }
        if (a2tDisabled) ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (status.model_loading)
                ImGui::SetTooltip("Wait for Whisper model to load");
            else if (status.file_transcription.active)
                ImGui::SetTooltip("Transcription in progress...");
            else
                ImGui::SetTooltip("Transcribe an audio file to text (WAV, MP3, FLAC)");
        }

        ImGui::SameLine(0, 6);

        // Folder button
        if (ImGui::Button("Folder", ImVec2(55, 25))) {
            std::string dir = status.recordings_dir;
            if (!dir.empty()) {
                CreateDirectoryA(dir.c_str(), NULL);
                ShellExecuteA(NULL, "open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open recordings folder");

        // Timer inline when WAV recording is active
        if (wavActive) {
            ImGui::SameLine(0, 8);
            int secs = (int)status.wav_recording.seconds;
            char timer[16];
            snprintf(timer, sizeof(timer), "%02d:%02d", secs / 60, secs % 60);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", timer);
        }
    }

    // --- Transcription Progress Popup ---
    // Auto-open when transcription starts (e.g. via API)
    if (status.file_transcription.active && !m_showTranscriptionPopup) {
        m_showTranscriptionPopup = true;
    }

    if (m_showTranscriptionPopup) {
        ImGui::OpenPopup("Transcription Progress");
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(700, 420), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Transcription Progress", nullptr,
                               ImGuiWindowFlags_NoMove)) {
        auto& ft = status.file_transcription;

        // Throttled log fetch (~1s interval while popup is open)
        if (now - m_lastLogFetchTime > 1.0) {
            m_transcriptLogLines = m_engine.FetchLogTail(30);
            m_lastLogFetchTime = now;
        }

        // Extract filename from path
        std::string filename = ft.input_path;
        auto lastSlash = filename.find_last_of("\\/");
        if (lastSlash != std::string::npos)
            filename = filename.substr(lastSlash + 1);

        // Pre-populate save filename when transcription first completes
        if ((ft.status == "done" || ft.status == "saving") && m_saveFilename[0] == '\0' && !filename.empty()) {
            std::string baseName = filename;
            auto dotPos = baseName.find_last_of('.');
            if (dotPos != std::string::npos)
                baseName = baseName.substr(0, dotPos);
            snprintf(m_saveFilename, sizeof(m_saveFilename), "%s", baseName.c_str());
        }

        if (ft.active) {
            // In-progress
            ImGui::Text("File:");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", filename.c_str());
            ImGui::Spacing();

            if (ft.status == "transcribing") {
                ImGui::Text("Transcribing...");
                ImGui::ProgressBar(ft.progress / 100.0f, ImVec2(-1, 0));
                ImGui::Text("%.0f%%", ft.progress);
            } else if (ft.status == "cleaning") {
                ImGui::Text("Cleaning with LLM...");
                float t = (float)fmod(ImGui::GetTime() * 0.5, 1.0);
                ImGui::ProgressBar(t, ImVec2(-1, 0));
            }
        } else if (ft.status == "done" || ft.status == "saving") {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Transcription Complete");
            ImGui::Spacing();
            ImGui::Text("File:");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", filename.c_str());
            ImGui::Spacing();

            if (ft.output_path.empty()) {
                // Not saved yet — show filename + style dropdown + format buttons
                static const char* styleLabels[] = {"Raw", "Clean", "Detailed", "Summarize"};
                static const char* styleKeys[]   = {"raw", "clean", "detailed", "summarize"};

                ImGui::Text("Name:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##filename", m_saveFilename, sizeof(m_saveFilename));

                ImGui::Text("Style:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::Combo("##style", &m_saveStyleIdx, styleLabels, IM_ARRAYSIZE(styleLabels));

                ImGui::Spacing();

                if (ft.status == "saving") {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Saving...");
                    float t = (float)fmod(ImGui::GetTime() * 0.5, 1.0);
                    ImGui::ProgressBar(t, ImVec2(-1, 0));
                } else {
                    if (ImGui::Button("Save .txt", ImVec2(120, 0))) {
                        m_engine.SaveTranscription("txt", styleKeys[m_saveStyleIdx], m_saveFilename);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Save .md", ImVec2(120, 0))) {
                        m_engine.SaveTranscription("md", styleKeys[m_saveStyleIdx], m_saveFilename);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Close##done_unsaved", ImVec2(80, 0))) {
                        m_showTranscriptionPopup = false;
                        m_transcriptLogLines.clear();
                        m_saveFilename[0] = '\0';
                        ImGui::CloseCurrentPopup();
                    }
                }
            } else {
                // Already saved — show path + Open Folder
                std::string outName = ft.output_path;
                auto outSlash = outName.find_last_of("\\/");
                if (outSlash != std::string::npos)
                    outName = outName.substr(outSlash + 1);
                ImGui::Text("Saved:");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", outName.c_str());
                ImGui::Spacing();

                if (ImGui::Button("Open Folder", ImVec2(120, 0))) {
                    std::string cmd = "/select,\"" + ft.output_path + "\"";
                    ShellExecuteA(NULL, "open", "explorer.exe", cmd.c_str(), NULL, SW_SHOWNORMAL);
                }
                ImGui::SameLine();
                if (ImGui::Button("Save Another", ImVec2(120, 0))) {
                    m_engine.ResetSave();
                    m_saveFilename[0] = '\0';
                }
                ImGui::SameLine();
                if (ImGui::Button("Close##done_saved", ImVec2(80, 0))) {
                    m_showTranscriptionPopup = false;
                    m_transcriptLogLines.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        } else if (ft.status == "error") {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Transcription Failed");
            ImGui::Spacing();
            ImGui::TextWrapped("Error: %s", ft.error.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(80, 0))) {
                m_showTranscriptionPopup = false;
                m_transcriptLogLines.clear();
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (!m_lastTranscriptStatus.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", m_lastTranscriptStatus.c_str());
                ImGui::Spacing();
            }
            if (ImGui::Button("Close", ImVec2(80, 0))) {
                m_showTranscriptionPopup = false;
                m_transcriptLogLines.clear();
                ImGui::CloseCurrentPopup();
            }
        }

        // --- Engine Log panel ---
        ImGui::Spacing();
        ImGui::SeparatorText("Engine Log");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##logs")) {
            m_engine.ClearLogs();
            m_transcriptLogLines.clear();
        }
        float logHeight = ImGui::GetContentRegionAvail().y - 4.0f;
        if (logHeight < 40.0f) logHeight = 40.0f;
        ImGui::BeginChild("##TranscriptLog", ImVec2(-1, logHeight), ImGuiChildFlags_Border,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (const auto& line : m_transcriptLogLines) {
            ImGui::TextUnformatted(line.c_str());
        }
        // Auto-scroll to bottom
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();

        ImGui::EndPopup();
    }

    // --- Calibration Popup ---
    if (m_showCalPopup) {
        ImGui::OpenPopup("Microphone Calibration");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 280), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Microphone Calibration", nullptr, ImGuiWindowFlags_NoMove)) {
        float elapsed = (float)(now - m_calStartTime);

        if (m_calPhase == 0) {
            // Phase 0: Silence measurement
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Step 1 of 2: Measuring Room Noise");
            ImGui::Spacing();
            ImGui::TextWrapped("Stay quiet — measuring ambient noise level...");
            ImGui::Spacing();
            float silenceDuration = 2.0f;
            float progress = fminf(elapsed / silenceDuration, 1.0f);
            ImGui::ProgressBar(progress, ImVec2(-1, 0));
            char timerBuf[32];
            snprintf(timerBuf, sizeof(timerBuf), "%.1fs remaining", fmaxf(0.0f, silenceDuration - elapsed));
            ImGui::TextDisabled("%s", timerBuf);

            // Auto-transition to speech phase
            if (elapsed >= silenceDuration + 0.3f) {
                if (m_engine.FinishSilenceCalibration()) {
                    m_calNoiseFloor = status.gate.calibrated_noise_floor_dbfs;
                    m_engine.StartSpeechCalibration();
                    m_calStartTime = now;
                    m_calPhase = 1;
                } else {
                    m_calPhase = -1;
                    m_calError = "Silence calibration failed — speech was detected. Please try again and stay quiet.";
                }
            }
        } else if (m_calPhase == 1) {
            // Phase 1: Speech measurement
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Step 2 of 2: Measuring Speech Level");
            ImGui::Spacing();
            ImGui::TextWrapped("Read this sentence aloud at your normal speaking volume:");
            ImGui::Spacing();

            // Display the LLM-generated prompt prominently
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
            ImGui::BeginChild("##calPrompt", ImVec2(-1, 50), ImGuiChildFlags_Border);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.7f, 1.0f), "\"%s\"", m_calPrompt.c_str());
            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Spacing();
            float speechDuration = 3.0f;
            float progress = fminf(elapsed / speechDuration, 1.0f);
            ImGui::ProgressBar(progress, ImVec2(-1, 0));
            char timerBuf[32];
            snprintf(timerBuf, sizeof(timerBuf), "%.1fs remaining", fmaxf(0.0f, speechDuration - elapsed));
            ImGui::TextDisabled("%s", timerBuf);

            // Auto-finish
            if (elapsed >= speechDuration + 0.3f) {
                if (m_engine.FinishCalibration()) {
                    m_calNoiseFloor = status.gate.calibrated_noise_floor_dbfs;
                    m_calSpeechLevel = status.gate.calibrated_speech_dbfs;
                    m_calOpenThresh = status.gate.open_threshold_dbfs;
                    m_calCloseThresh = status.gate.close_threshold_dbfs;
                    m_calPhase = 2;
                } else {
                    m_calPhase = -1;
                    m_calError = "Calibration failed — could not compute thresholds.";
                }
            }
        } else if (m_calPhase == 2) {
            // Phase 2: Results
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Calibration Complete!");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Noise Floor:");
            ImGui::SameLine(180);
            ImGui::Text("%.1f dBFS", m_calNoiseFloor);
            ImGui::Text("Speech Level:");
            ImGui::SameLine(180);
            ImGui::Text("%.1f dBFS", m_calSpeechLevel);
            ImGui::Spacing();
            ImGui::Text("Open Threshold:");
            ImGui::SameLine(180);
            ImGui::Text("%.1f dBFS", m_calOpenThresh);
            ImGui::Text("Close Threshold:");
            ImGui::SameLine(180);
            ImGui::Text("%.1f dBFS", m_calCloseThresh);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Thresholds have been applied and saved.");
            ImGui::Spacing();
            if (ImGui::Button("Done", ImVec2(120, 0))) {
                m_showCalPopup = false;
                ImGui::CloseCurrentPopup();
            }
        } else {
            // Error state
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Calibration Failed");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", m_calError.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Retry", ImVec2(100, 0))) {
                m_calPhase = 0;
                m_calError.clear();
                m_calPrompt = m_engine.FetchCalibrationPrompt();
                m_engine.StartCalibration();
                m_calStartTime = now;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100, 0))) {
                m_showCalPopup = false;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    // --- Processing (DSP) ---
    if (status.has_dsp) {
        ImGui::Separator();
        float meterAlpha = 1.0f - powf(0.5f, dt / 0.05f);

        // Smooth meters
        m_smoothInputDbfs += meterAlpha * (status.gate.input_dbfs - m_smoothInputDbfs);
        m_smoothOutputDbfs += meterAlpha * (status.gate.output_dbfs - m_smoothOutputDbfs);
        m_smoothGR += meterAlpha * (status.compressor.gain_reduction_db - m_smoothGR);

        // VU peak hold: fast attack, 1.5s decay
        if (m_smoothInputDbfs > m_vuPeakDbfs)
            m_vuPeakDbfs = m_smoothInputDbfs;
        else
            m_vuPeakDbfs += (1.0f - powf(0.5f, dt / 1.5f)) * (m_smoothInputDbfs - m_vuPeakDbfs);

        // === Noise Gate ===
        {
            // Header line: toggle + status
            bool gateEnabled = status.gate.enabled;
            if (gateEnabled) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.45f, 0.10f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
            }
            if (ImGui::Button(gateEnabled ? "Gate: ON" : "Gate: OFF", ImVec2(100, 22))) {
                nlohmann::json body = {{"dsp", {{"noise_gate", {{"enabled", !gateEnabled}}}}}};
                m_engine.PostDSPConfig(body.dump());
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            ImGui::Text("Input: %.1f dBFS", m_smoothInputDbfs);
            ImGui::SameLine();
            if (status.gate.gate_open) {
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "OPEN");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "CLOSED (%.0f dB)", -status.gate.attenuation_db);
            }

            if (gateEnabled) {
                // Input level bar with threshold markers
                {
                    ImVec2 barPos = ImGui::GetCursorScreenPos();
                    float barWidth = ImGui::GetContentRegionAvail().x;
                    float barHeight = 14.0f;
                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    dl->AddRectFilled(barPos, ImVec2(barPos.x + barWidth, barPos.y + barHeight),
                                      IM_COL32(30, 30, 30, 255));

                    // Level fill (-80..0 dBFS mapped to 0..1)
                    float levelT = (m_smoothInputDbfs - (-80.0f)) / (0.0f - (-80.0f));
                    levelT = fmaxf(0.0f, fminf(1.0f, levelT));
                    ImU32 levelCol = status.gate.gate_open ? IM_COL32(40, 200, 60, 200) : IM_COL32(200, 60, 40, 200);
                    dl->AddRectFilled(barPos, ImVec2(barPos.x + levelT * barWidth, barPos.y + barHeight), levelCol);

                    // Open threshold marker (bright yellow)
                    float openT = (status.gate.open_threshold_dbfs - (-80.0f)) / 80.0f;
                    openT = fmaxf(0.0f, fminf(1.0f, openT));
                    float openX = barPos.x + openT * barWidth;
                    dl->AddLine(ImVec2(openX, barPos.y), ImVec2(openX, barPos.y + barHeight),
                                IM_COL32(255, 220, 50, 220), 2.0f);

                    // Close threshold marker (dim yellow)
                    float closeT = (status.gate.close_threshold_dbfs - (-80.0f)) / 80.0f;
                    closeT = fmaxf(0.0f, fminf(1.0f, closeT));
                    float closeX = barPos.x + closeT * barWidth;
                    dl->AddLine(ImVec2(closeX, barPos.y), ImVec2(closeX, barPos.y + barHeight),
                                IM_COL32(255, 220, 50, 100), 2.0f);

                    ImGui::Dummy(ImVec2(barWidth, barHeight));
                }

                // Sliders (locked by default to prevent accidental changes)
                if (ImGui::SmallButton(m_dspLocked ? "Unlock Sliders" : "Lock Sliders")) {
                    m_dspLocked = !m_dspLocked;
                }

                if (m_dspLocked) ImGui::BeginDisabled();
                ImGui::PushItemWidth(-140);

                float openTh = status.gate.open_threshold_dbfs;
                float closeTh = status.gate.close_threshold_dbfs;
                float floorDb = status.gate.floor_db;

                if (ImGui::SliderFloat("Open Threshold##gate", &openTh, -80.0f, 0.0f, "%.1f dBFS")) {
                    // Enforce gap: push close down if needed
                    if (openTh < closeTh + 3.0f) closeTh = openTh - 3.0f;
                    nlohmann::json body = {{"dsp", {{"noise_gate", {
                        {"open_threshold_dbfs", openTh},
                        {"close_threshold_dbfs", closeTh}
                    }}}}};
                    m_engine.PostDSPConfig(body.dump());
                }

                if (ImGui::SliderFloat("Close Threshold##gate", &closeTh, -80.0f, 0.0f, "%.1f dBFS")) {
                    // Enforce gap: push open up if needed
                    if (closeTh > openTh - 3.0f) openTh = closeTh + 3.0f;
                    nlohmann::json body = {{"dsp", {{"noise_gate", {
                        {"open_threshold_dbfs", openTh},
                        {"close_threshold_dbfs", closeTh}
                    }}}}};
                    m_engine.PostDSPConfig(body.dump());
                }

                if (ImGui::SliderFloat("Floor##gate", &floorDb, -80.0f, 0.0f, "%.1f dB")) {
                    nlohmann::json body = {{"dsp", {{"noise_gate", {{"floor_db", floorDb}}}}}};
                    m_engine.PostDSPConfig(body.dump());
                }

                ImGui::PopItemWidth();
                if (m_dspLocked) ImGui::EndDisabled();

                // Auto Calibrate button — opens guided popup
                if (ImGui::Button("Auto Calibrate", ImVec2(140, 22))) {
                    m_showCalPopup = true;
                    m_calPhase = 0;
                    m_calError.clear();
                    m_calPrompt = m_engine.FetchCalibrationPrompt();
                    m_engine.StartCalibration();
                    m_calStartTime = ImGui::GetTime();
                }
                if (status.gate.calibrated_noise_floor_dbfs > -79.0f) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Noise floor: %.1f dBFS", status.gate.calibrated_noise_floor_dbfs);
                }
            }
        }

        ImGui::Spacing();

        // === Compressor ===
        {
            bool compEnabled = status.compressor.enabled;
            if (compEnabled) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.25f, 0.55f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
            }
            if (ImGui::Button(compEnabled ? "Comp: ON" : "Comp: OFF", ImVec2(100, 22))) {
                nlohmann::json body = {{"dsp", {{"compressor", {{"enabled", !compEnabled}}}}}};
                m_engine.PostDSPConfig(body.dump());
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();

            // GR meter (always visible, even when OFF)
            {
                ImGui::Text("GR:");
                ImGui::SameLine();
                ImVec2 grPos = ImGui::GetCursorScreenPos();
                float grWidth = 120.0f;
                float grHeight = 14.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(grPos, ImVec2(grPos.x + grWidth, grPos.y + grHeight),
                                  IM_COL32(30, 30, 30, 255));
                // GR fills right-to-left in orange
                float grT = fminf(m_smoothGR / 20.0f, 1.0f);
                if (grT > 0.001f) {
                    dl->AddRectFilled(ImVec2(grPos.x + grWidth * (1.0f - grT), grPos.y),
                                      ImVec2(grPos.x + grWidth, grPos.y + grHeight),
                                      IM_COL32(220, 140, 30, 200));
                }
                ImGui::Dummy(ImVec2(grWidth, grHeight));
                ImGui::SameLine();
                ImGui::Text("%.1f dB", m_smoothGR);
            }

            if (compEnabled) {
                if (m_dspLocked) ImGui::BeginDisabled();
                ImGui::PushItemWidth(-140);

                float compTh = status.compressor.threshold_dbfs;
                float compRatio = status.compressor.ratio;
                float compMakeup = status.compressor.makeup_gain_db;

                if (ImGui::SliderFloat("Threshold##comp", &compTh, -60.0f, 0.0f, "%.1f dBFS")) {
                    nlohmann::json body = {{"dsp", {{"compressor", {{"threshold_dbfs", compTh}}}}}};
                    m_engine.PostDSPConfig(body.dump());
                }
                if (ImGui::SliderFloat("Ratio##comp", &compRatio, 1.0f, 20.0f, "%.1f:1")) {
                    nlohmann::json body = {{"dsp", {{"compressor", {{"ratio", compRatio}}}}}};
                    m_engine.PostDSPConfig(body.dump());
                }
                if (ImGui::SliderFloat("Makeup##comp", &compMakeup, 0.0f, 24.0f, "%.1f dB")) {
                    nlohmann::json body = {{"dsp", {{"compressor", {{"makeup_gain_db", compMakeup}}}}}};
                    m_engine.PostDSPConfig(body.dump());
                }

                ImGui::PopItemWidth();
                if (m_dspLocked) ImGui::EndDisabled();
            }
        }

        // Spectrum source toggle
        ImGui::Spacing();
        {
            const char* items[] = { "Post DSP", "Pre DSP" };
            int current = status.spectrum_pre_dsp ? 1 : 0;
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("Spectrum##source", &current, items, 2)) {
                m_engine.SetSpectrumSource(current == 1);
            }
        }
    }

    ImGui::Separator();

    // --- Spectrum Visualizer ---
    {
        // 5a. Delta-time EMA smoothing + peak hold + noise floor
        float riseAlpha  = 1.0f - powf(0.5f, dt / 0.08f);
        float decayAlpha = 1.0f - powf(0.5f, dt / 0.30f);
        float peakDecay  = 1.0f - powf(0.5f, dt / 0.80f);
        float floorRise  = 1.0f - powf(0.5f, dt / 3.0f);
        float floorFall  = 1.0f - powf(0.5f, dt / 0.5f);

        for (int i = 0; i < kSpectrumBins && i < (int)status.fft_bins.size(); ++i) {
            float target = status.fft_bins[i];

            if (target > m_smoothBins[i])
                m_smoothBins[i] += riseAlpha * (target - m_smoothBins[i]);
            else
                m_smoothBins[i] += decayAlpha * (target - m_smoothBins[i]);

            if (target > m_peakBins[i])
                m_peakBins[i] = target;
            else
                m_peakBins[i] += peakDecay * (m_smoothBins[i] - m_peakBins[i]);

            if (!status.is_speech) {
                if (target < m_noiseFloor[i])
                    m_noiseFloor[i] += floorFall * (target - m_noiseFloor[i]);
                else
                    m_noiseFloor[i] += floorRise * (target - m_noiseFloor[i]);
            } else {
                float speechFloorDecay = 1.0f - powf(0.5f, dt / 2.0f);
                m_noiseFloor[i] += speechFloorDecay * (0.0f - m_noiseFloor[i]);
            }
        }

        // 5b. Phase-based colors
        ImU32 lineColor, fillColor, peakColor, glowColor;
        if (status.phase == "recording") {
            lineColor = IM_COL32(255, 153, 51, 230);
            fillColor = IM_COL32(255, 153, 51, 40);
            peakColor = IM_COL32(255, 200, 120, 140);
            glowColor = IM_COL32(255, 153, 51, 64);
        } else if (status.phase == "listening") {
            lineColor = IM_COL32(77, 128, 255, 217);
            fillColor = IM_COL32(77, 128, 255, 35);
            peakColor = IM_COL32(140, 170, 255, 120);
            glowColor = IM_COL32(77, 128, 255, 64);
        } else if (status.phase == "transcribing") {
            lineColor = IM_COL32(178, 102, 255, 180);
            fillColor = IM_COL32(178, 102, 255, 30);
            peakColor = IM_COL32(200, 160, 255, 120);
            glowColor = IM_COL32(178, 102, 255, 64);
        } else if (status.phase == "cleaning") {
            lineColor = IM_COL32(51, 230, 230, 180);
            fillColor = IM_COL32(51, 230, 230, 30);
            peakColor = IM_COL32(120, 240, 240, 120);
            glowColor = IM_COL32(51, 230, 230, 64);
        } else if (status.phase == "typing") {
            lineColor = IM_COL32(51, 230, 77, 180);
            fillColor = IM_COL32(51, 230, 77, 30);
            peakColor = IM_COL32(120, 240, 140, 120);
            glowColor = IM_COL32(51, 230, 77, 64);
        } else if (status.phase == "error") {
            lineColor = IM_COL32(255, 77, 77, 180);
            fillColor = IM_COL32(255, 77, 77, 30);
            peakColor = IM_COL32(255, 140, 140, 120);
            glowColor = IM_COL32(255, 77, 77, 64);
        } else {
            lineColor = IM_COL32(90, 90, 100, 150);
            fillColor = IM_COL32(90, 90, 100, 25);
            peakColor = IM_COL32(120, 120, 130, 80);
            glowColor = IM_COL32(90, 90, 100, 40);
        }

        // 5c. Drawing
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        float height = 120.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (status.is_speech) {
            dl->AddRect(ImVec2(pos.x - 1, pos.y - 1),
                        ImVec2(pos.x + width + 1, pos.y + height + 1),
                        glowColor, 0.0f, 0, 2.0f);
        }

        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(18, 18, 18, 255));

        // Grid lines + labels (20Hz-20kHz, matching Python FFT range)
        float logMin = log10f(20.0f), logMax = log10f(20000.0f);
        struct GridLine { float freq; const char* label; };
        GridLine gridLines[] = {
            {20, "20"}, {50, "50"}, {100, "100"}, {200, "200"}, {500, "500"},
            {1000, "1k"}, {2000, "2k"}, {4000, "4k"}, {8000, "8k"},
            {16000, "16k"}, {20000, "20k"}
        };
        for (const auto& gl : gridLines) {
            float t = (log10f(gl.freq) - logMin) / (logMax - logMin);
            float x = pos.x + t * width;
            dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + height), IM_COL32(255, 255, 255, 15));
            if (gl.label) {
                dl->AddText(ImVec2(x + 2, pos.y + height - 12), IM_COL32(255, 255, 255, 40), gl.label);
            }
        }

        // Articulation band highlight (2k-4k)
        {
            float t0 = (log10f(2000.0f) - logMin) / (logMax - logMin);
            float t1 = (log10f(4000.0f) - logMin) / (logMax - logMin);
            int binStart = (int)roundf(t0 * kSpectrumBins);
            int binEnd   = (int)roundf(t1 * kSpectrumBins);
            binStart = (binStart < 0) ? 0 : (binStart >= kSpectrumBins ? kSpectrumBins - 1 : binStart);
            binEnd   = (binEnd < 0)   ? 0 : (binEnd   >= kSpectrumBins ? kSpectrumBins - 1 : binEnd);
            float articulationEnergy = 0.0f;
            for (int i = binStart; i <= binEnd && i < kSpectrumBins; ++i)
                articulationEnergy = fmaxf(articulationEnergy, m_smoothBins[i]);
            if (articulationEnergy > 0.15f) {
                float alpha = fminf(articulationEnergy * 30.0f, 20.0f);
                dl->AddRectFilled(ImVec2(pos.x + t0 * width, pos.y),
                                  ImVec2(pos.x + t1 * width, pos.y + height),
                                  IM_COL32(255, 255, 255, (int)alpha));
            }
        }

        // Build curve points
        ImVec2 points[kSpectrumBins];
        for (int i = 0; i < kSpectrumBins; ++i) {
            float x = pos.x + (float)i / (kSpectrumBins - 1) * width;
            float y = pos.y + height - m_smoothBins[i] * height * 0.9f;
            points[i] = ImVec2(x, y);
        }

        // Noise floor baseline
        for (int i = 0; i < kSpectrumBins - 1; ++i) {
            float x0 = pos.x + (float)i / (kSpectrumBins - 1) * width;
            float x1 = pos.x + (float)(i + 1) / (kSpectrumBins - 1) * width;
            float y0 = pos.y + height - m_noiseFloor[i] * height * 0.9f;
            float y1 = pos.y + height - m_noiseFloor[i + 1] * height * 0.9f;
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 20), 1.0f);
        }

        // Filled area
        for (int i = 0; i < kSpectrumBins - 1; ++i) {
            ImVec2 bl(points[i].x, pos.y + height);
            ImVec2 br(points[i + 1].x, pos.y + height);
            dl->AddTriangleFilled(points[i], points[i + 1], br, fillColor);
            dl->AddTriangleFilled(points[i], br, bl, fillColor);
        }

        // Shimmer overlay during recording — subtle traveling highlight
        if (status.phase == "recording") {
            float shimmerPos = fmodf((float)ImGui::GetTime() * 0.4f, 1.0f);
            for (int i = 0; i < kSpectrumBins - 1; ++i) {
                float binT = (float)i / (kSpectrumBins - 1);
                float dist = fabsf(binT - shimmerPos);
                if (dist > 0.5f) dist = 1.0f - dist;  // wrap around
                float shimmer = fmaxf(0.0f, 1.0f - dist * 8.0f);
                if (shimmer > 0.0f) {
                    ImU32 shimmerColor = IM_COL32(255, 200, 120, (int)(shimmer * 25.0f));
                    float y_top = fminf(points[i].y, points[i + 1].y);
                    dl->AddRectFilled(ImVec2(points[i].x, y_top),
                                      ImVec2(points[i + 1].x, pos.y + height), shimmerColor);
                }
            }
        }

        // Curve line
        dl->AddPolyline(points, kSpectrumBins, lineColor, 0, 2.0f);

        // Peak hold dots
        for (int i = 0; i < kSpectrumBins; ++i) {
            float x = pos.x + (float)i / (kSpectrumBins - 1) * width;
            float y = pos.y + height - m_peakBins[i] * height * 0.9f;
            if (m_peakBins[i] > m_smoothBins[i] + 0.02f) {
                dl->AddCircleFilled(ImVec2(x, y), 1.5f, peakColor);
            }
        }

        // Clip indicator
        {
            bool clipping = false;
            for (int i = 0; i < kSpectrumBins; ++i)
                if (m_smoothBins[i] > 0.95f) { clipping = true; break; }
            if (clipping) {
                const char* clipText = "CLIP";
                ImVec2 textSize = ImGui::CalcTextSize(clipText);
                dl->AddText(ImVec2(pos.x + width - textSize.x - 4, pos.y + 3),
                            IM_COL32(255, 40, 40, 220), clipText);
            }
        }

        ImGui::Dummy(ImVec2(width, height));
    }

    // --- VU Level Meter ---
    {
        ImGui::Spacing();
        ImVec2 vuPos = ImGui::GetCursorScreenPos();
        float vuWidth = ImGui::GetContentRegionAvail().x;
        float vuHeight = 22.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(vuPos, ImVec2(vuPos.x + vuWidth, vuPos.y + vuHeight),
                          IM_COL32(18, 18, 20, 255));

        // dBFS scale: map input_dbfs from [-80, 0] to [0, 1]
        float dbfs = m_smoothInputDbfs;
        float levelT = (dbfs + 80.0f) / 80.0f;
        levelT = fmaxf(0.0f, fminf(1.0f, levelT));

        // Color gradient: green -> yellow -> red
        float fillW = levelT * vuWidth;
        float greenEnd = ((-12.0f + 80.0f) / 80.0f) * vuWidth;
        float yellowEnd = ((-3.0f + 80.0f) / 80.0f) * vuWidth;

        // Green segment
        if (fillW > 0) {
            float gW = fminf(fillW, greenEnd);
            dl->AddRectFilled(vuPos, ImVec2(vuPos.x + gW, vuPos.y + vuHeight),
                              IM_COL32(40, 200, 60, 220));
        }
        // Yellow segment
        if (fillW > greenEnd) {
            float yW = fminf(fillW, yellowEnd);
            dl->AddRectFilled(ImVec2(vuPos.x + greenEnd, vuPos.y),
                              ImVec2(vuPos.x + yW, vuPos.y + vuHeight),
                              IM_COL32(230, 200, 40, 220));
        }
        // Red segment
        if (fillW > yellowEnd) {
            dl->AddRectFilled(ImVec2(vuPos.x + yellowEnd, vuPos.y),
                              ImVec2(vuPos.x + fillW, vuPos.y + vuHeight),
                              IM_COL32(220, 50, 40, 220));
        }

        // Peak hold indicator (thin bright line)
        float peakT = (m_vuPeakDbfs + 80.0f) / 80.0f;
        peakT = fmaxf(0.0f, fminf(1.0f, peakT));
        float peakX = vuPos.x + peakT * vuWidth;
        dl->AddLine(ImVec2(peakX, vuPos.y), ImVec2(peakX, vuPos.y + vuHeight),
                    IM_COL32(255, 255, 255, 180), 2.0f);

        // Gate threshold marker (yellow line)
        if (status.gate.enabled) {
            float openT = (status.gate.open_threshold_dbfs + 80.0f) / 80.0f;
            float openX = vuPos.x + fmaxf(0.0f, fminf(1.0f, openT)) * vuWidth;
            dl->AddLine(ImVec2(openX, vuPos.y), ImVec2(openX, vuPos.y + vuHeight),
                        IM_COL32(255, 220, 50, 200), 2.0f);
        }

        // dBFS tick marks
        struct VUTick { float db; const char* label; };
        VUTick ticks[] = { {-60, "-60"}, {-40, "-40"}, {-20, "-20"}, {-6, "-6"}, {0, "0"} };
        for (const auto& t : ticks) {
            float tx = vuPos.x + ((t.db + 80.0f) / 80.0f) * vuWidth;
            dl->AddLine(ImVec2(tx, vuPos.y + vuHeight - 4), ImVec2(tx, vuPos.y + vuHeight),
                        IM_COL32(255, 255, 255, 50));
            dl->AddText(ImVec2(tx + 2, vuPos.y + 1), IM_COL32(255, 255, 255, 60), t.label);
        }

        // dBFS value text (right-aligned)
        char dbText[32];
        snprintf(dbText, sizeof(dbText), "%.1f dBFS", dbfs);
        ImVec2 textSize = ImGui::CalcTextSize(dbText);
        dl->AddText(ImVec2(vuPos.x + vuWidth - textSize.x - 4, vuPos.y + (vuHeight - textSize.y) / 2),
                    status.gate.gate_open ? IM_COL32(40, 220, 60, 255) : IM_COL32(200, 200, 200, 200),
                    dbText);

        ImGui::Dummy(ImVec2(vuWidth, vuHeight));
    }

    ImGui::Separator();

    // --- Pending approval panel ---
    if (status.phase == "pending_approval" && !status.pending_text.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.1f, 1.0f), "PENDING APPROVAL:");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::TextWrapped("%s", status.pending_text.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Approve (green)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
        if (ImGui::Button("Approve", ImVec2(100, 30))) {
            m_engine.ApprovePending();
            m_editActive = false;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // Edit (blue)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.3f, 0.7f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.4f, 0.8f, 1.0f));
        if (ImGui::Button("Edit", ImVec2(100, 30))) {
            strncpy(m_editBuffer, status.pending_text.c_str(), sizeof(m_editBuffer) - 1);
            m_editBuffer[sizeof(m_editBuffer) - 1] = '\0';
            m_editActive = true;
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();

        // Reject (red)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Reject", ImVec2(100, 30))) {
            m_engine.RejectPending();
            m_editActive = false;
        }
        ImGui::PopStyleColor(2);

        // Edit text box
        if (m_editActive) {
            ImGui::Spacing();
            ImGui::Text("Edit text:");
            ImGui::InputTextMultiline("##EditPending", m_editBuffer, sizeof(m_editBuffer),
                                       ImVec2(-1, 80));
            if (ImGui::Button("Send Edited", ImVec2(120, 25))) {
                m_engine.EditPending(std::string(m_editBuffer));
                m_editActive = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 25))) {
                m_editActive = false;
            }
        }

        ImGui::Separator();
    } else {
        m_editActive = false;
    }

    // --- Last transcript ---
    ImGui::TextDisabled("Raw:");
    ImGui::Indent(8);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::TextWrapped("%s", status.last_raw_transcript.empty()
                       ? "(waiting for speech...)" : status.last_raw_transcript.c_str());
    ImGui::PopStyleColor();
    ImGui::Unindent(8);

    ImGui::Separator();

    ImGui::Text("Output:");
    ImGui::Indent(8);
    // Detect new output and trigger flash
    if (status.last_cleaned_text != m_lastOutputText && !status.last_cleaned_text.empty()) {
        m_lastOutputText = status.last_cleaned_text;
        m_outputFlashTime = ImGui::GetTime();
    }
    // Flash: bright white-green → normal green over 300ms
    float flashAge = (float)(ImGui::GetTime() - m_outputFlashTime);
    float flashAlpha = fmaxf(0.0f, 1.0f - flashAge / 0.3f);
    ImVec4 outputColor = ImVec4(0.4f + 0.6f * flashAlpha, 1.0f, 0.4f + 0.6f * flashAlpha, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, outputColor);
    ImGui::TextWrapped("%s", status.last_cleaned_text.empty()
                       ? "(waiting for speech...)" : status.last_cleaned_text.c_str());
    ImGui::PopStyleColor();
    ImGui::Unindent(8);

    ImGui::Separator();

    // --- Latency ---
    float gen_ms = status.latency.transcribe_ms + status.latency.cleanup_ms
                 + status.latency.type_ms;
    float total_ms = status.latency.record_ms + gen_ms;

    ImGui::Text("Latency");
    ImGui::Indent(12);
    ImGui::TextDisabled("Record:     %4.0f ms", status.latency.record_ms);
    ImGui::TextDisabled("Transcribe: %4.0f ms", status.latency.transcribe_ms);
    ImGui::TextDisabled("Cleanup:    %4.0f ms", status.latency.cleanup_ms);
    ImGui::TextDisabled("Type:       %4.0f ms", status.latency.type_ms);
    ImGui::Unindent(12);

    auto colorForMs = [](float ms) -> ImVec4 {
        return (ms < 2000.0f) ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f)
             : (ms < 5000.0f) ? ImVec4(0.9f, 0.8f, 0.2f, 1.0f)
                               : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    };
    ImGui::TextColored(colorForMs(gen_ms), "Generation: %.0f ms", gen_ms);
    ImGui::TextColored(colorForMs(total_ms), "Total:      %.0f ms", total_ms);

    // --- Error ---
    if (!status.last_error.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Error: %s", status.last_error.c_str());
    }

    ImGui::End();

    // --- Instructions popup ---
    if (m_showInstructions) {
        ImGui::OpenPopup("Instructions##modal");
        m_showInstructions = false;
    }
    if (ImGui::BeginPopupModal("Instructions##modal", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::SetWindowSize(ImVec2(620, 560), ImGuiCond_Always);

        // Accent color used for section headers and highlights
        const ImVec4 accent(0.24f, 0.78f, 0.78f, 1.0f);     // teal/cyan
        const ImVec4 accentDim(0.24f, 0.60f, 0.60f, 1.0f);
        const ImVec4 cmdColor(0.95f, 0.75f, 0.30f, 1.0f);   // warm gold for commands
        const ImVec4 muted(0.50f, 0.52f, 0.55f, 1.0f);

        // Title
        ImGui::TextColored(accent, "Murmur");
        ImGui::SameLine();
        ImGui::TextDisabled("Local Voice Dictation");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginChild("##InstructionsScroll", ImVec2(-1, 440), ImGuiChildFlags_Border,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);

        // --- Getting Started ---
        ImGui::TextColored(accent, "GETTING STARTED");
        ImGui::Spacing();
        ImGui::Indent(8);
        ImGui::TextWrapped(
            "Press your hotkey to start recording. Speak naturally "
            "and the app will transcribe when you pause. Press the "
            "hotkey again to stop.");
        ImGui::Spacing();
        ImGui::TextColored(muted, "Current hotkey:");
        ImGui::SameLine();
        {
            std::string hkUpper = status.hotkey;
            for (auto& ch : hkUpper) ch = (char)toupper((unsigned char)ch);
            ImGui::TextColored(cmdColor, "%s", hkUpper.c_str());
        }
        ImGui::TextColored(muted, "Change via:");
        ImGui::SameLine();
        ImGui::Text("Edit > Change Hotkey");
        ImGui::TextDisabled("The hotkey is suppressed from other apps while Murmur is running.");
        ImGui::Unindent(8);

        ImGui::Spacing(); ImGui::Spacing();

        // --- Modes ---
        ImGui::TextColored(accent, "MODES");
        ImGui::Spacing();
        ImGui::Indent(8);
        ImGui::TextDisabled("Change via Edit > Mode. Each mode controls how speech is processed.");
        ImGui::Spacing();

        // Mode table
        if (ImGui::BeginTable("##modes", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("LLM", ImGuiTableColumnFlags_WidthFixed, 36);
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "Raw");
            ImGui::TableNextColumn(); ImGui::TextDisabled("OFF");
            ImGui::TableNextColumn(); ImGui::Text("Whisper output typed as-is, no processing");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "Clean");
            ImGui::TableNextColumn(); ImGui::Text("ON");
            ImGui::TableNextColumn(); ImGui::Text("Removes filler words, fixes grammar");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "Prompt");
            ImGui::TableNextColumn(); ImGui::Text("ON");
            ImGui::TableNextColumn(); ImGui::Text("Restructures speech into LLM-ready prompts");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "Dev");
            ImGui::TableNextColumn(); ImGui::Text("ON");
            ImGui::TableNextColumn(); ImGui::Text("Converts speech into bullet points / tasks");

            ImGui::EndTable();
        }
        ImGui::Unindent(8);

        ImGui::Spacing(); ImGui::Spacing();

        // --- Profiles ---
        ImGui::TextColored(accent, "PROFILES");
        ImGui::Spacing();
        ImGui::Indent(8);
        ImGui::TextWrapped(
            "Profiles bundle a mode with optional overrides for voice commands and hotkey. "
            "Switch via Edit > Profile.");
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Auto-detect can switch profiles automatically based on the active window title.");
        ImGui::Unindent(8);

        ImGui::Spacing(); ImGui::Spacing();

        // --- Voice Commands ---
        ImGui::TextColored(accent, "VOICE COMMANDS");
        ImGui::Spacing();
        ImGui::Indent(8);
        ImGui::TextWrapped("Say \"command\" followed by a phrase:");
        ImGui::Spacing();

        if (ImGui::BeginTable("##cmds", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Say", ImGuiTableColumnFlags_WidthFixed, 210);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "\"command new line\"");
            ImGui::TableNextColumn(); ImGui::Text("Press Enter");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "\"command send\"");
            ImGui::TableNextColumn(); ImGui::Text("Press Enter (Ctrl+Enter in some profiles)");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "\"command clear\"");
            ImGui::TableNextColumn(); ImGui::Text("Select all and delete");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "\"command copy\"");
            ImGui::TableNextColumn(); ImGui::Text("Ctrl+C");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "\"command paste\"");
            ImGui::TableNextColumn(); ImGui::Text("Ctrl+V");

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(cmdColor, "\"command stop dictation\"");
            ImGui::TableNextColumn(); ImGui::Text("Stop recording");

            ImGui::EndTable();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Without the \"command\" prefix, phrases are typed as regular text.");
        ImGui::Unindent(8);

        ImGui::Spacing(); ImGui::Spacing();

        // --- Features ---
        ImGui::TextColored(accent, "FEATURES");
        ImGui::Spacing();
        ImGui::Indent(8);

        ImGui::TextColored(accentDim, "Approval Mode");
        ImGui::SameLine();
        ImGui::TextDisabled("—");
        ImGui::SameLine();
        ImGui::TextWrapped("Review transcribed text before it's typed. Approve, edit, or reject each chunk.");

        ImGui::Spacing();
        ImGui::TextColored(accentDim, "Push-to-Talk");
        ImGui::SameLine();
        ImGui::TextDisabled("—");
        ImGui::SameLine();
        ImGui::TextWrapped("Hold the hotkey to record, release to stop. Useful for short commands.");

        ImGui::Spacing();
        ImGui::TextColored(accentDim, "Mouse Hotkey");
        ImGui::SameLine();
        ImGui::TextDisabled("—");
        ImGui::SameLine();
        ImGui::TextWrapped("Use Edit > Change Hotkey and click a mouse side button to bind it.");

        ImGui::Spacing();
        ImGui::TextColored(accentDim, "DSP Controls");
        ImGui::SameLine();
        ImGui::TextDisabled("—");
        ImGui::SameLine();
        ImGui::TextWrapped("Noise gate and compressor. Click \"Unlock Sliders\" to adjust. Use \"Auto Calibrate\" to set thresholds from ambient noise.");

        ImGui::Unindent(8);

        ImGui::Spacing(); ImGui::Spacing();

        // --- Troubleshooting ---
        ImGui::TextColored(accent, "TROUBLESHOOTING");
        ImGui::Spacing();
        ImGui::Indent(8);

        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "Engine disconnected?");
        ImGui::SameLine();
        ImGui::TextWrapped("Click File > Restart Engine.");

        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "LLM not working?");
        ImGui::SameLine();
        ImGui::TextWrapped("Make sure LM Studio is running with a model loaded.");

        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "No audio?");
        ImGui::SameLine();
        ImGui::TextWrapped("Check Edit > Microphone and select the correct input device.");

        ImGui::Unindent(8);

        ImGui::EndChild();

        // Footer
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 28))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 140);
        ImGui::TextDisabled("Murmur v%s", status.connected ? status.version.c_str() : "?.?.?");
        ImGui::EndPopup();
    }

    // --- About popup ---
    if (m_showAbout) {
        ImGui::OpenPopup("About Murmur##modal");
        m_showAbout = false;
    }
    if (ImGui::BeginPopupModal("About Murmur##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const ImVec4 accent(0.24f, 0.78f, 0.78f, 1.0f);

        ImGui::TextColored(accent, "Murmur");
        ImGui::SameLine();
        ImGui::TextDisabled("Local Voice Dictation");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("Engine");
        ImGui::SameLine(90);
        if (status.connected) {
            ImGui::Text("v%s", status.version.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "not connected");
        }

        ImGui::TextDisabled("Hotkey");
        ImGui::SameLine(90);
        {
            std::string hk = status.hotkey;
            for (auto& ch : hk) ch = (char)toupper((unsigned char)ch);
            ImGui::Text("%s", hk.c_str());
        }

        ImGui::TextDisabled("Mode");
        ImGui::SameLine(90);
        ImGui::Text("%s", status.current_mode.c_str());

        ImGui::TextDisabled("Profile");
        ImGui::SameLine(90);
        ImGui::Text("%s", status.current_profile.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("Fully local. No cloud. GPU-accelerated.");

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 28))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
