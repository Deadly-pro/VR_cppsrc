#pragma once

#include <string>
#include "thread_safe_queue.h"

// Forward declaration of GyroData
struct GyroData {
    float yaw;
    float pitch;
    float roll;
};


/**
 * Reads JSON gyro data from stdin line by line and pushes it into a thread-safe queue.
 *
 * @param queue Reference to a ThreadSafeQueue<GyroData> instance.
 */
void GyroStdinReaderThread(ThreadSafeQueue<GyroData>& queue);
