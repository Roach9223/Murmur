#include "updater.h"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

static const wchar_t* kApiHost   = L"api.github.com";
static const wchar_t* kApiPath   = L"/repos/Roach9223/Murmur/releases/latest";
static const wchar_t* kUserAgent = L"Murmur-Updater";

// --- WinHTTP helpers -------------------------------------------------------

struct HInternet {
    HINTERNET h = nullptr;
    HInternet(HINTERNET v) : h(v) {}
    ~HInternet() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

// GET https://<host><path> and return the response body (empty on failure).
static bool HttpGetString(const std::wstring& host, const std::wstring& path,
                          std::string& out)
{
    HInternet session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    HInternet conn(WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn) return false;
    HInternet req(WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE));
    if (!req) return false;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return false;
    if (!WinHttpReceiveResponse(req, nullptr)) return false;

    DWORD statusCode = 0, len = sizeof(statusCode);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) return false;

    out.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &read) || read == 0) break;
        out.append(buf.data(), read);
    }
    return !out.empty();
}

// Download a full https URL (follows GitHub's S3 redirects) to a file,
// reporting progress via the callback.
static bool HttpDownloadToFile(const std::wstring& url, const fs::path& dest,
                               long long expectedSize,
                               const std::function<void(float)>& onProgress)
{
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;  uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HInternet session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    HInternet conn(WinHttpConnect(session, host, uc.nPort, 0));
    if (!conn) return false;
    HInternet req(WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE));
    if (!req) return false;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return false;
    if (!WinHttpReceiveResponse(req, nullptr)) return false;

    DWORD statusCode = 0, len = sizeof(statusCode);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 200) return false;

    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    long long total = 0;
    std::vector<char> buf(256 * 1024);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) return false;
        if (avail == 0) break;
        DWORD toRead = (avail > buf.size()) ? (DWORD)buf.size() : avail;
        DWORD read = 0;
        if (!WinHttpReadData(req, buf.data(), toRead, &read) || read == 0) break;
        f.write(buf.data(), read);
        if (!f) return false;
        total += read;
        if (expectedSize > 0 && onProgress)
            onProgress((float)((double)total / (double)expectedSize));
    }
    f.close();
    return expectedSize <= 0 || total == expectedSize;
}

// --- Version compare -------------------------------------------------------

static void ParseVersion(std::string v, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    (void)sscanf_s(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
}

static bool IsNewer(const std::string& candidate, const std::string& current)
{
    int a[3], b[3];
    ParseVersion(candidate, a);
    ParseVersion(current, b);
    if (a[0] != b[0]) return a[0] > b[0];
    if (a[1] != b[1]) return a[1] > b[1];
    return a[2] > b[2];
}

// --- Updater ----------------------------------------------------------------

Updater::Updater(std::string currentVersion, std::wstring installDir)
    : m_current(std::move(currentVersion)), m_installDir(std::move(installDir)) {}

Updater::~Updater() { JoinWorker(); }

void Updater::JoinWorker()
{
    if (m_worker.joinable())
        m_worker.join();
}

void Updater::SetError(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = msg;
    }
    m_state = State::FAILED;
}

std::string Updater::LatestVersion() const { std::lock_guard<std::mutex> l(m_mutex); return m_latest; }
std::string Updater::ReleaseNotes() const  { std::lock_guard<std::mutex> l(m_mutex); return m_notes; }
std::string Updater::Error() const         { std::lock_guard<std::mutex> l(m_mutex); return m_error; }

void Updater::CheckAsync()
{
    State s = m_state.load();
    if (s == State::CHECKING || s == State::DOWNLOADING || s == State::EXTRACTING)
        return;
    JoinWorker();
    m_state = State::CHECKING;
    m_worker = std::thread(&Updater::CheckWorker, this);
}

void Updater::CheckWorker()
{
    std::string body;
    if (!HttpGetString(kApiHost, kApiPath, body)) {
        SetError("Could not reach GitHub to check for updates.");
        return;
    }
    try {
        json j = json::parse(body);
        std::string tag = j.value("tag_name", "");
        if (tag.empty()) {
            SetError("Unexpected response from GitHub.");
            return;
        }
        std::string notes = j.value("body", "");
        std::string updateUrl, fullUrl;
        long long updateSize = 0, fullSize = 0;
        if (j.contains("assets") && j["assets"].is_array()) {
            for (auto& a : j["assets"]) {
                std::string name = a.value("name", "");
                if (name == "Murmur-update.zip") {
                    updateUrl = a.value("browser_download_url", "");
                    updateSize = a.value("size", 0LL);
                } else if (name == "Murmur-release.zip") {
                    fullUrl = a.value("browser_download_url", "");
                    fullSize = a.value("size", 0LL);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latest = tag;
            m_notes = notes.substr(0, 1200);
            m_assetUrl = !updateUrl.empty() ? updateUrl : fullUrl;
        }
        m_assetSize = !updateUrl.empty() ? updateSize : fullSize;

        if (!IsNewer(tag, m_current)) {
            m_state = State::UP_TO_DATE;
        } else if (m_assetUrl.empty()) {
            SetError("A newer version exists but has no downloadable package yet.");
        } else {
            m_state = State::UPDATE_AVAILABLE;
        }
    } catch (const std::exception& e) {
        SetError(std::string("Update check failed: ") + e.what());
    }
}

void Updater::DownloadAndStageAsync()
{
    if (m_state.load() != State::UPDATE_AVAILABLE)
        return;
    JoinWorker();
    m_progress = 0.0f;
    m_state = State::DOWNLOADING;
    m_worker = std::thread(&Updater::DownloadWorker, this);
}

void Updater::DownloadWorker()
{
    try {
        fs::path updateDir = fs::path(m_installDir) / "update";
        fs::path zipPath = updateDir / "pkg.zip";
        fs::path staging = updateDir / "staging";
        std::error_code ec;
        fs::remove_all(staging, ec);
        fs::create_directories(staging);

        std::string urlUtf8;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            urlUtf8 = m_assetUrl;
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, urlUtf8.c_str(), -1, nullptr, 0);
        std::wstring url(wlen > 0 ? wlen - 1 : 0, L'\0');
        if (wlen > 0)
            MultiByteToWideChar(CP_UTF8, 0, urlUtf8.c_str(), -1, url.data(), wlen);

        if (!HttpDownloadToFile(url, zipPath, m_assetSize.load(),
                                [this](float p) { m_progress = p; })) {
            SetError("Download failed — check your connection and try again.");
            return;
        }

        m_state = State::EXTRACTING;

        // Extract with the OS-bundled bsdtar (ships with Windows 10 1803+)
        std::wstring cmd = L"tar -x -f \"" + zipPath.wstring() + L"\" -C \"" +
                           staging.wstring() + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::wstring cmdMut = cmd;
        if (!CreateProcessW(nullptr, cmdMut.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            SetError("Could not extract the update (tar not found).");
            return;
        }
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (exitCode != 0 || !fs::exists(staging / "Murmur" / "Murmur.exe")) {
            SetError("The downloaded update package is invalid.");
            return;
        }
        m_state = State::READY;
    } catch (const std::exception& e) {
        SetError(std::string("Update failed: ") + e.what());
    }
}

bool Updater::LaunchApplyAndQuit()
{
    if (m_state.load() != State::READY)
        return false;

    fs::path updateDir = fs::path(m_installDir) / "update";

    // config.json must survive updates — it holds the user's settings.
    // (Everything is %~dp0-relative so non-ASCII install paths work.)
    {
        std::ofstream ex(updateDir / "exclude.txt", std::ios::trunc);
        ex << "config.json\n";
    }
    {
        std::ofstream bat(updateDir / "apply.cmd", std::ios::trunc);
        bat <<
            "@echo off\r\n"
            "cd /d \"%~dp0\"\r\n"
            "set TRIES=0\r\n"
            ":waitui\r\n"
            "tasklist /FI \"IMAGENAME eq Murmur.exe\" 2>NUL | find /I \"Murmur.exe\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  set /a TRIES+=1\r\n"
            "  if %TRIES% GEQ 60 goto restart\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waitui\r\n"
            ")\r\n"
            "set TRIES=0\r\n"
            ":waiteng\r\n"
            "tasklist /FI \"IMAGENAME eq murmur-engine.exe\" 2>NUL | find /I \"murmur-engine.exe\" >NUL\r\n"
            "if not errorlevel 1 (\r\n"
            "  set /a TRIES+=1\r\n"
            "  if %TRIES% GEQ 60 goto restart\r\n"
            "  ping -n 2 127.0.0.1 >NUL\r\n"
            "  goto waiteng\r\n"
            ")\r\n"
            "xcopy \"staging\\Murmur\" \"..\" /E /Y /Q /EXCLUDE:exclude.txt\r\n"
            ":restart\r\n"
            "rmdir /s /q \"staging\" 2>NUL\r\n"
            "del /q \"pkg.zip\" 2>NUL\r\n"
            "start \"\" \"%~dp0..\\Murmur.exe\"\r\n";
        if (!bat) return false;
    }

    fs::path batPath = updateDir / "apply.cmd";
    HINSTANCE r = ShellExecuteW(nullptr, L"open", batPath.c_str(), nullptr,
                                updateDir.c_str(), SW_HIDE);
    return (INT_PTR)r > 32;
}
