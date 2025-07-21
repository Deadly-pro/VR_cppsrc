#pragma once

#include <string>
#include "thread_safe_queue.h"
struct Vector3;
struct HandTrackingData {
    std::string handedness;
    std::vector<Vector3> landmarks;
    float depth_scale;
    float distance_factor;
    //HandTrackingData();
    //~HandTrackingData();

    //// Optional: Add copy/move constructors if needed, but default is fine here
    //HandTrackingData(const HandTrackingData&) = default;
    //HandTrackingData& operator=(const HandTrackingData&) = default;
    //HandTrackingData(HandTrackingData&&) = default;
    //HandTrackingData& operator=(HandTrackingData&&) = default;
};

void HandStdinReaderThread(ThreadSafeQueue<HandTrackingData>& queue);
