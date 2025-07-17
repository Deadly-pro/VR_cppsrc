#pragma once

#include <string>
#include "thread_safe_queue.h"
struct Vector3 {
    float x; float y; float z;
};

struct HandTrackingData {
    std::string handedness;
    std::vector<Vector3> landmarks;
    float confidence;
    float depth_scale;
    float distance_factor;
};

void HandStdinReaderThread(ThreadSafeQueue<HandTrackingData>& queue);
