#include "engine_process.h"

#include <httplib.h>
#include <filesystem>

namespace fs = std::filesystem;

// Lossless UTF-16 -> UTF-8 for display/error strings
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

EngineProcess::EngineProcess(const std::wstring& engineDir, int port)
    : m_engineDir(engineDir), m_port(port)
{
    // Detect bundled mode: engine/murmur-engine.exe next to Murmur.exe
    if (!engineDir.empty()) {
        m_bundled = fs::exists(fs::path(engineDir) / "engine" / "murmur-engine.exe");
    }
}

EngineProcess::~EngineProcess() {
    if (IsRunning())
        Terminate();

    if (m_processHandle) CloseHandle(m_processHandle);
    if (m_threadHandle)  CloseHandle(m_threadHandle);
    if (m_jobHandle)     CloseHandle(m_jobHandle);
}

std::wstring EngineProcess::DiscoverEngineDirW() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();

    // 1) Bundled mode: engine/murmur-engine.exe next to our exe
    if (fs::exists(exeDir / "engine" / "murmur-engine.exe")) {
        return exeDir.wstring();
    }

    // 2) Dev mode: walk up to find app.py + venv
    fs::path p(exePath);
    for (int i = 0; i < 6; ++i) {
        p = p.parent_path();
        if (fs::exists(p / "app.py") && fs::exists(p / "venv")) {
            return p.wstring();
        }
    }
    return {};
}

bool EngineProcess::Launch() {
    m_state = State::LAUNCHING;

    std::wstring cmdLine;
    std::wstring workDir = fs::path(m_engineDir).wstring();

    if (m_bundled) {
        // Bundled mode: engine/murmur-engine.exe --server --port N --base-dir <dir>
        fs::path engineExe = fs::path(m_engineDir) / "engine" / "murmur-engine.exe";
        if (!fs::exists(engineExe)) {
            m_error = "murmur-engine.exe not found: " + WideToUtf8(engineExe.wstring());
            m_state = State::FAILED;
            return false;
        }
        // Strip trailing separators: a dir like "F:\" would put a backslash
        // before the closing quote and escape it, mangling the argument.
        std::wstring baseDir = fs::path(m_engineDir).wstring();
        while (!baseDir.empty() && (baseDir.back() == L'\\' || baseDir.back() == L'/'))
            baseDir.pop_back();
        cmdLine =
            L"\"" + engineExe.wstring() + L"\" " +
            L"--server --port " + std::to_wstring(m_port) +
            L" --base-dir \"" + baseDir + L"\"";
    } else {
        // Dev mode: pythonw.exe app.py --server --port N
        fs::path pythonExe = fs::path(m_engineDir) / "venv" / "Scripts" / "pythonw.exe";
        fs::path appPy = fs::path(m_engineDir) / "app.py";

        if (!fs::exists(pythonExe)) {
            m_error = "pythonw.exe not found: " + WideToUtf8(pythonExe.wstring());
            m_state = State::FAILED;
            return false;
        }
        if (!fs::exists(appPy)) {
            m_error = "app.py not found: " + WideToUtf8(appPy.wstring());
            m_state = State::FAILED;
            return false;
        }
        cmdLine =
            L"\"" + pythonExe.wstring() + L"\" " +
            L"\"" + appPy.wstring() + L"\" " +
            L"--server --port " + std::to_wstring(m_port);
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),          // mutable command line
        nullptr, nullptr,        // security attrs
        FALSE,                   // inherit handles
        CREATE_NO_WINDOW,        // no console
        nullptr,                 // environment (inherit)
        workDir.c_str(),         // working directory
        &si, &pi
    );

    if (!ok) {
        DWORD err = GetLastError();
        m_error = "CreateProcess failed (error " + std::to_string(err) + ")";
        m_state = State::FAILED;
        return false;
    }

    m_processHandle = pi.hProcess;
    m_threadHandle = pi.hThread;
    m_processId = pi.dwProcessId;

    // Tie the engine's lifetime to ours: if Murmur.exe dies abnormally
    // (crash, Task Manager kill), the job object kills the engine too.
    if (!m_jobHandle) {
        m_jobHandle = CreateJobObjectW(nullptr, nullptr);
        if (m_jobHandle) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(m_jobHandle, JobObjectExtendedLimitInformation,
                                    &jeli, sizeof(jeli));
        }
    }
    if (m_jobHandle)
        AssignProcessToJobObject(m_jobHandle, m_processHandle);

    m_launched = true;
    m_state = State::RUNNING;
    return true;
}

EngineProcess::State EngineProcess::GetState() {
    // Detect a child that died after a successful launch (port conflict,
    // missing runtime, corrupted download) so the UI can show an error
    // instead of "waiting for connection..." forever.
    if (m_state == State::RUNNING && m_processHandle && !IsRunning()) {
        DWORD exitCode = 0;
        GetExitCodeProcess(m_processHandle, &exitCode);
        m_error = "Engine process exited unexpectedly (exit code " +
                  std::to_string(exitCode) + "). Check logs\\dictation.log, "
                  "or another app may be using port " + std::to_string(m_port) + ".";
        m_state = State::FAILED;
    }
    return m_state;
}

void EngineProcess::Terminate(int gracefulTimeoutMs) {
    if (!m_processHandle)
        return;

    // 1) Graceful: POST /engine/shutdown
    {
        httplib::Client cli("127.0.0.1", m_port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(2);
        cli.Post("/engine/shutdown");
    }

    // 2) Wait for process to exit
    DWORD result = WaitForSingleObject(m_processHandle, gracefulTimeoutMs);

    // 3) Force kill if still running
    if (result == WAIT_TIMEOUT && IsRunning()) {
        TerminateProcess(m_processHandle, 1);
        WaitForSingleObject(m_processHandle, 1000);
    }

    CloseHandle(m_processHandle);
    CloseHandle(m_threadHandle);
    m_processHandle = nullptr;
    m_threadHandle = nullptr;
    m_processId = 0;
    m_state = State::STOPPED;
}

bool EngineProcess::IsRunning() const {
    if (!m_processHandle)
        return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_processHandle, &exitCode))
        return false;
    return exitCode == STILL_ACTIVE;
}
