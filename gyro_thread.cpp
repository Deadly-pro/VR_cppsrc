#include <nlohmann/json.hpp>
#include <string>
#include <iostream>
#include <fstream>
#include "thread_safe_queue.h"
#include "gyro_thread.h"
#include <windows.h>
#include <cstdio>

bool CheckStdinAvailable() {
    DWORD bytesAvailable = 0;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE) return false;

    return PeekNamedPipe(hIn, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0;
}

#define DEGRAD 0.01745329251994329576923690768489 // PI / 180
void GyroStdinReaderThread(ThreadSafeQueue<GyroData>& queue) {
    std::ofstream log("gyro_debug.log", std::ios::app);
    log << "[INFO] Gyro thread started\n";

    while (true) {
        if (CheckStdinAvailable()) {
            std::string line;
            if (std::getline(std::cin, line)) {
                try {
                    if (line.empty()) continue;

                    auto j = nlohmann::json::parse(line);

                    float alpha = j.value("alpha", 0.0f);
                    float beta = j.value("beta", 0.0f);
                    float gamma = j.value("gamma", 0.0f);

                    GyroData data;
                    data.yaw = DEGRAD * alpha;
                    data.pitch = DEGRAD * gamma;
                    data.roll = DEGRAD * beta;

                    log << "[INFO] Parsed Gyro: alpha=" << alpha
                        << ", beta=" << beta
                        << ", gamma=" << gamma << std::endl;

                    queue.push(std::move(data));
                }
                catch (const std::exception& e) {
                    log << "Gyro parse error: " << e.what() << std::endl;
                }
            }
            else {
                log << "[INFO] EOF reached in Gyro thread. Exiting.\n";
                break;
            }
        }
        else {
            // Sleep briefly to avoid busy spin
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}
