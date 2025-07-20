#include <nlohmann/json.hpp>
#include <string>
#include <iostream>
#include <fstream>
#include "thread_safe_queue.h"
#include "handat_thread.h"
#include <windows.h>
#include <cstdio>

static bool CheckStdinAvailable() {
    DWORD bytesAvailable = 0;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE) return false;

    return PeekNamedPipe(hIn, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0;
}
using json = nlohmann::json;
void HandStdinReaderThread(ThreadSafeQueue<HandTrackingData>& queue) {
    std::ofstream log("hand_debug.log", std::ios::app);
    log << "[INFO] hand thread started\n";
    std::string line;

    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            auto type = j.value("type", "");
            if (type != "hand") continue;
            auto payload = j.at("payload");  // this is an array of the payload 

            for (auto& h : payload) {
                HandTrackingData data;
                data.handedness = h.value("handedness", "");
                data.confidence = h.value("confidence", 0.0f);

                auto lm = h.at("landmarks");
                for (auto& pt : lm) {
                    data.landmarks.push_back({
                        pt.value("x", 0.0f),
                        pt.value("y", 0.0f),
                        pt.value("z", 0.0f)
                        });
                }
                queue.push(std::move(data));
            }
        }
        catch (const std::exception& e) {
            log << "[ERROR] parse hand data: " << e.what() << "\n";
        }
    }
}
