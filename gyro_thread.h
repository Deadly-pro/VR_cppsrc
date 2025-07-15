#include <string>
#include "thread_safe_queue.h"
struct GyroData {
    float yaw;
    float pitch;
    float roll;
};

void GyroStdinReaderThread(ThreadSafeQueue<GyroData>& queue);
