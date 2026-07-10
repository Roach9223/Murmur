// Console harness for the self-updater. Runs the real check → download →
// stage pipeline against the live GitHub release without applying anything.
// Usage: updater_test.exe <work-dir> [pretend-version]
#include "updater.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static const char* StateName(Updater::State s)
{
    switch (s) {
    case Updater::State::IDLE: return "IDLE";
    case Updater::State::CHECKING: return "CHECKING";
    case Updater::State::UP_TO_DATE: return "UP_TO_DATE";
    case Updater::State::UPDATE_AVAILABLE: return "UPDATE_AVAILABLE";
    case Updater::State::DOWNLOADING: return "DOWNLOADING";
    case Updater::State::EXTRACTING: return "EXTRACTING";
    case Updater::State::READY: return "READY";
    case Updater::State::FAILED: return "FAILED";
    }
    return "?";
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        printf("usage: updater_test.exe <work-dir> [pretend-version]\n");
        return 2;
    }
    std::wstring dir = argv[1];
    std::string pretend = "0.0.1";
    if (argc >= 3) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            pretend.assign(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, pretend.data(), len, nullptr, nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);

    Updater u(pretend, dir);
    printf("checking (as v%s)...\n", pretend.c_str());
    u.CheckAsync();
    while (u.GetState() == Updater::State::CHECKING)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    printf("state: %s  latest: %s  asset: %lld bytes\n",
           StateName(u.GetState()), u.LatestVersion().c_str(), u.AssetSizeBytes());
    if (u.GetState() == Updater::State::FAILED) {
        printf("error: %s\n", u.Error().c_str());
        return 1;
    }
    if (u.GetState() != Updater::State::UPDATE_AVAILABLE) {
        printf("nothing to download — done.\n");
        return 0;
    }

    printf("downloading...\n");
    u.DownloadAndStageAsync();
    float lastShown = -1.0f;
    while (u.GetState() == Updater::State::DOWNLOADING ||
           u.GetState() == Updater::State::EXTRACTING) {
        float p = u.Progress();
        if (p - lastShown >= 0.10f) {
            printf("  %.0f%% (%s)\n", p * 100.0f, StateName(u.GetState()));
            lastShown = p;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    printf("final state: %s\n", StateName(u.GetState()));
    if (u.GetState() == Updater::State::FAILED) {
        printf("error: %s\n", u.Error().c_str());
        return 1;
    }

    bool apply = (argc >= 4 && std::wstring(argv[3]) == L"--apply");
    if (!apply) {
        printf("staged OK — NOT applying (pass --apply as 4th arg to test the swap).\n");
        return 0;
    }
    printf("launching apply script against %ls ...\n", dir.c_str());
    if (!u.LaunchApplyAndQuit()) {
        printf("failed to launch apply script\n");
        return 1;
    }
    printf("apply script launched — check the work dir contents in a few seconds.\n");
    return 0;
}
