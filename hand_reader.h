#pragma once
#include <vector>
#include "thread_safe_queue.h"
void HandTrackingReaderThread(ThreadSafeQueue<std::vector<HandTrackingData>>& handQueue);

