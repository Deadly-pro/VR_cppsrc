#pragma once
#include "platform.h"
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>
#include <d3d11.h>
#include <dxgi1_2.h>

#include "thread_safe_queue.h"

// The frame structure remains the same, but will be managed by a memory pool.
struct CapturedFrame {
    std::vector<uint8_t> pixels;
    int width;
    int height;
    int channels;
    bool isValid;
    int screenIndex;
    std::chrono::steady_clock::time_point timestamp;
    CapturedFrame() : width(0), height(0), channels(0), isValid(false), screenIndex(0) {}
};

class ScreenCapture {
private:
    static std::vector<std::unique_ptr<std::thread>> captureThreads;
    static std::atomic<bool> shouldStop;
    static std::atomic<bool> isRunning;

    // --- MODIFICATION: Queues now store pointers to frames from the pool ---
    static ThreadSafeQueue<CapturedFrame*> frameQueue;
    static ThreadSafeQueue<CapturedFrame*> framePool;

    // --- MODIFICATION: DirectX members for Desktop Duplication API ---
    static ID3D11Device* d3d11Device;
    static ID3D11DeviceContext* d3d11Context;
    static std::vector<IDXGIOutputDuplication*> duplications;

    static void captureThreadFunction(size_t monitorIndex);

public:
    static bool initialize();
    static void cleanup();

    // --- MODIFICATION: Functions now work with pointers ---
    static std::optional<CapturedFrame*> getLatestFrame();
    static void releaseFrame(CapturedFrame* frame); // New function to return frame to pool

    static int getScreenCount();
    static void setCaptureRate(float fps); // Note: This will be less precise with Desktop Duplication
    static bool isInitialized();
    static size_t getQueueSize();
};