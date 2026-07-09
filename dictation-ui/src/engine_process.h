#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

class EngineProcess {
public:
    EngineProcess(const std::wstring& engineDir, int port);
    ~EngineProcess();

    bool Launch();
    void Terminate(int gracefulTimeoutMs = 3000);
    bool IsRunning() const;

    enum class State { NOT_STARTED, LAUNCHING, RUNNING, FAILED, STOPPED };
    State GetState();  // also detects a child that died after launch
    std::string GetError() const { return m_error; }

    // True if this process manager actually started the engine child.
    // Used to avoid shutting down an engine the user started themselves.
    bool Launched() const { return m_launched; }

    // Derive engine dir from the running exe's path.
    // Wide string end-to-end: narrowing through the ANSI codepage mangles
    // non-Latin install paths (e.g. Cyrillic/CJK usernames).
    static std::wstring DiscoverEngineDirW();

private:
    std::wstring m_engineDir;
    int m_port;
    bool m_bundled = false;  // true when engine/murmur-engine.exe exists

    HANDLE m_processHandle = nullptr;
    HANDLE m_threadHandle = nullptr;
    HANDLE m_jobHandle = nullptr;
    DWORD m_processId = 0;
    bool m_launched = false;

    State m_state = State::NOT_STARTED;
    std::string m_error;
};
