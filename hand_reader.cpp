#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "thread_safe_queue.h"
using json = nlohmann::json;
struct Vector3 { float x; float y; float z; };
struct HandTrackingData {
    std::string handedness;
    std::vector<Vector3> landmarks;
};

void HandTrackingReaderThread(ThreadSafeQueue<std::vector<HandTrackingData>>& handQueue)
{
    std::string pythonExe = R"(.venv\Scripts\python.exe)";
    std::string scriptPath = R"(execs\Mediapipe.py)";
    std::string cmd = "\"" + pythonExe + "\" \"" + scriptPath + "\"";

    // Create pipes
	// can be reconfigured to use CreateNamedPipe if needed for go server to send directly here 
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE readPipe, writePipe;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        std::cerr << "Pipe creation failed.\n";
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessA(
        NULL,
        (LPSTR)cmd.c_str(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!ok) {
        std::cerr << "CreateProcess failed.\n";
        return;
    }

    CloseHandle(writePipe);

    char buffer[8192] = {};
    std::string leftover;

    while (true) {
        DWORD bytesRead = 0;
        BOOL success = ReadFile(readPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
        if (!success || bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = '\0';
        leftover += buffer;

        size_t pos;
        while ((pos = leftover.find('\n')) != std::string::npos) {
            std::string line = leftover.substr(0, pos);
            leftover.erase(0, pos + 1);

            if (line.empty()) continue;

            try {
                json j = json::parse(line);
                std::vector<HandTrackingData> hands;

                for (const auto& item : j) {
                    HandTrackingData h;
                    h.handedness = item["handedness"].get<std::string>();
                    for (const auto& lm : item["landmarks"]) {
                        h.landmarks.push_back(Vector3{
                            lm["x"].get<float>(),
                            lm["y"].get<float>(),
                            lm["z"].get<float>()
                            });
                    }
                    hands.push_back(h);
                }

                handQueue.push(hands);

            }
            catch (std::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

