#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Self-update via GitHub Releases.
//
// Flow: CheckAsync() polls the latest release; if newer, the UI shows an
// update button. DownloadAndStageAsync() downloads the release asset
// (preferring the slim "Murmur-update.zip" when the release ships one,
// falling back to the full "Murmur-release.zip") and extracts it to
// <install>\update\staging using the OS-bundled tar. LaunchApplyAndQuit()
// writes a tiny apply script that waits for Murmur.exe + murmur-engine.exe
// to exit, copies the staged files over the install dir (never touching
// config.json), relaunches Murmur, and cleans up.
class Updater {
public:
    enum class State {
        IDLE, CHECKING, UP_TO_DATE, UPDATE_AVAILABLE,
        DOWNLOADING, EXTRACTING, READY, FAILED
    };

    Updater(std::string currentVersion, std::wstring installDir);
    ~Updater();

    void CheckAsync();
    void DownloadAndStageAsync();
    bool LaunchApplyAndQuit();   // true if the apply script was launched

    State GetState() const { return m_state.load(); }
    float Progress() const { return m_progress.load(); }        // 0..1
    long long AssetSizeBytes() const { return m_assetSize.load(); }
    std::string LatestVersion() const;
    std::string ReleaseNotes() const;
    std::string Error() const;

private:
    void CheckWorker();
    void DownloadWorker();
    void JoinWorker();
    void SetError(const std::string& msg);

    std::string m_current;
    std::wstring m_installDir;

    std::atomic<State> m_state{State::IDLE};
    std::atomic<float> m_progress{0.0f};
    std::atomic<long long> m_assetSize{0};

    mutable std::mutex m_mutex;   // guards the strings below
    std::string m_latest;
    std::string m_notes;
    std::string m_error;
    std::string m_assetUrl;

    std::thread m_worker;
};
