#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

class EngineProcess {
public:
    EngineProcess(const std::string& engineDir, int port);
    ~EngineProcess();

    bool Launch();
    void Terminate(int gracefulTimeoutMs = 3000);
    bool IsRunning() const;

    enum class State { NOT_STARTED, LAUNCHING, RUNNING, FAILED, STOPPED };
    State GetState() const { return m_state; }
    std::string GetError() const { return m_error; }

    // Derive engine dir from the running exe's path
    static std::string DiscoverEngineDir();

private:
    std::string m_engineDir;
    int m_port;
    bool m_bundled = false;  // true when engine/murmur-engine.exe exists

    HANDLE m_processHandle = nullptr;
    HANDLE m_threadHandle = nullptr;
    DWORD m_processId = 0;

    State m_state = State::NOT_STARTED;
    std::string m_error;
};
