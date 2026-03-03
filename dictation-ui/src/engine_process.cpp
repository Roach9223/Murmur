#include "engine_process.h"

#include <httplib.h>
#include <filesystem>

namespace fs = std::filesystem;

EngineProcess::EngineProcess(const std::string& engineDir, int port)
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
}

std::string EngineProcess::DiscoverEngineDir() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();

    // 1) Bundled mode: engine/murmur-engine.exe next to our exe
    if (fs::exists(exeDir / "engine" / "murmur-engine.exe")) {
        return exeDir.string();
    }

    // 2) Dev mode: walk up to find app.py + venv
    fs::path p(exePath);
    for (int i = 0; i < 6; ++i) {
        p = p.parent_path();
        if (fs::exists(p / "app.py") && fs::exists(p / "venv")) {
            return p.string();
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
            m_error = "murmur-engine.exe not found: " + engineExe.string();
            m_state = State::FAILED;
            return false;
        }
        cmdLine =
            L"\"" + engineExe.wstring() + L"\" " +
            L"--server --port " + std::to_wstring(m_port) +
            L" --base-dir \"" + fs::path(m_engineDir).wstring() + L"\"";
    } else {
        // Dev mode: pythonw.exe app.py --server --port N
        fs::path pythonExe = fs::path(m_engineDir) / "venv" / "Scripts" / "pythonw.exe";
        fs::path appPy = fs::path(m_engineDir) / "app.py";

        if (!fs::exists(pythonExe)) {
            m_error = "pythonw.exe not found: " + pythonExe.string();
            m_state = State::FAILED;
            return false;
        }
        if (!fs::exists(appPy)) {
            m_error = "app.py not found: " + appPy.string();
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
    m_state = State::RUNNING;
    return true;
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
