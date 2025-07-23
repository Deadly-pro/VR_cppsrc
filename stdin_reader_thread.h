#pragma once

#include <string>
#include <vector>

// Forward declarations for the data structures and the template class.
// This avoids including the full definitions in the header, reducing compile times.
struct GyroData;
struct HandTrackingData;
template<typename T> class ThreadSafeQueue;

/**
 * @brief Reads JSON data from stdin, parses it, and dispatches it to the appropriate queue.
 *
 * This function is designed to run in a dedicated thread. It polls stdin for new data,
 * determines the data type ("Gyro" or "hand") from the JSON content, and pushes the
 * parsed data to the correct thread-safe queue.
 *
 * @param gyroQueue A reference to a thread-safe queue for GyroData.
 * @param handQueue A reference to a thread-safe queue for HandTrackingData.
 */
void StdinReaderThread(ThreadSafeQueue<GyroData>& gyroQueue, ThreadSafeQueue<HandTrackingData>& handQueue);
