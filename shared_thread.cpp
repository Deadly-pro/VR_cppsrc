#include <nlohmann/json.hpp>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <windows.h>
#include <cstdio>

// Custom headers for data structures and the thread-safe queue
#include "thread_safe_queue.h"
#include "gyro_thread.h" // Assumed to contain the definition for GyroData
#include "handat_thread.h" // Assumed to contain the definition for HandTrackingData

// --- FIX ---
// The full definition for Vector3 is needed here so the compiler knows how to construct it.
// It's best to move this to a shared header file (e.g., "datatypes.h") in a real project.
struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};


// Define the constant for converting degrees to radians
#define DEGRAD 0.01745329251994329576923690768489 // PI / 180

// Use the nlohmann::json library
using json = nlohmann::json;

/**
 * @brief Checks if there is data available on the standard input stream without blocking.
 *
 * This function is specific to Windows and uses PeekNamedPipe to check for available bytes.
 *
 * @return true if data is available to be read from stdin, false otherwise.
 */
static bool CheckStdinAvailable() {
    DWORD bytesAvailable = 0;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE) {
        return false;
    }
    // Check the pipe for available data without removing it
    return PeekNamedPipe(hIn, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0;
}

/**
 * @brief Reads JSON data from stdin, parses it, and dispatches it to the appropriate queue.
 *
 * This function runs in a dedicated thread and continuously polls stdin for new data.
 * It expects each line to be a JSON object with a "type" field, which determines
 * whether the data is for the gyroscope or hand tracking.
 *
 * @param gyroQueue A thread-safe queue to push GyroData into.
 * @param handQueue A thread-safe queue to push HandTrackingData into.
 */
void StdinReaderThread(ThreadSafeQueue<GyroData>& gyroQueue, ThreadSafeQueue<HandTrackingData>& handQueue) {
    // Open a log file for debugging purposes. Appending to the file.
    std::ofstream log("stdin_reader_debug.log", std::ios::app);
    log << "[INFO] Combined STDIN reader thread started." << std::endl;

    while (true) {
        // Non-blockingly check if there is data on stdin
        if (CheckStdinAvailable()) {
            std::string line;
            // Read a line from stdin. If the stream closes, getline returns false.
            if (std::getline(std::cin, line)) {
                if (line.empty()) {
                    continue; // Skip empty lines
                }

                try {
                    // Parse the line as a JSON object
                    auto j = json::parse(line);

                    // Determine the data type from the "type" field
                    std::string type = j.value("type", "");

                    if (type == "Gyro") {
                        log << "[INFO] Gyro data received: " << line << std::endl;
                        auto payload = j.at("payload");

                        float alpha = payload.value("alpha", 0.0f);
                        float beta = payload.value("beta", 0.0f);
                        float gamma = payload.value("gamma", 0.0f);

                        // Apply custom logic to fix gyro orientation
                        if (beta > 0) {
                            beta = -180.0f + beta;
                        }

                        GyroData data;
                        data.yaw = DEGRAD * alpha;
                        data.pitch = DEGRAD * beta;
                        data.roll = DEGRAD * gamma;

                        gyroQueue.push(std::move(data));

                    }
                    else if (type == "hand") {
                        // log << "[INFO] Hand data received: " << line << std::endl;
                        auto payload = j.at("payload");

                        // The payload for hand tracking is an array of hands
                        for (const auto& h : payload) {
                            HandTrackingData data;
                            data.handedness = h.value("handedness", "");

                            auto landmarksJson = h.at("landmarks");
                            for (const auto& pt : landmarksJson) {
                                // --- FIX ---
                                // Explicitly construct the Vector3 object. This works on all compilers.
                                data.landmarks.push_back(Vector3(
                                    pt.value("x", 0.0f),
                                    pt.value("y", 0.0f),
                                    pt.value("z", 0.0f)
                                ));
                            }
                            log << "[INFO] Hand data for " << data.handedness << " received with "
								<< data.landmarks.size() << " landmarks." << std::endl;
                            handQueue.push(std::move(data));
                        }
                    }
                    else {
                        log << "[WARN] Unknown data type received: '" << type << "'" << std::endl;
                    }

                }
                catch (const json::exception& e) {
                    log << "[ERROR] JSON parse/access error: " << e.what() << std::endl;
                    log << "[ERROR] Offending line: " << line << std::endl;
                }
                catch (const std::exception& e) {
                    log << "[ERROR] A standard exception occurred: " << e.what() << std::endl;
                }

            }
            else {
                // std::getline failed, probably because stdin was closed.
                log << "[INFO] STDIN stream closed. Exiting thread." << std::endl;
                break; // Exit the loop
            }
        }
        else {
            // No data available, sleep briefly to prevent a busy-wait loop.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}