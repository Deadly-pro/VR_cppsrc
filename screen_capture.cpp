#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wingdi.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <memory>
#include "screen_capture.h"
struct MonitorInfo {
    HMONITOR hMonitor;
    RECT rcMonitor;
};

// Global monitor list
std::vector<MonitorInfo> g_monitors;

// Callback function for EnumDisplayMonitors
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    g_monitors.push_back({ hMonitor, *lprcMonitor });
    return TRUE;
}

// Define static members
std::vector<std::unique_ptr<std::thread>> ScreenCapture::captureThreads;
std::atomic<bool> ScreenCapture::shouldStop{ false };
std::atomic<bool> ScreenCapture::isRunning{ false };
ThreadSafeQueue<CapturedFrame> ScreenCapture::frameQueue;
std::atomic<float> ScreenCapture::captureRate{ 1.0f / 120.0f }; // Default to 120 FPS capture attempt

void ScreenCapture::captureThreadFunction(size_t monitorIndex) {
    isRunning = true;

    const RECT rc = g_monitors[monitorIndex].rcMonitor;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    HDC screenDC = GetDC(NULL);
    HDC memoryDC = CreateCompatibleDC(screenDC);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height);
    SelectObject(memoryDC, bitmap);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Negative height for top-down bitmap
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    while (!shouldStop) {
        // Capture the screen
        BitBlt(memoryDC, 0, 0, width, height, screenDC, rc.left, rc.top, SRCCOPY);

        CapturedFrame frame;
        frame.width = width;
        frame.height = height;
        frame.channels = 4;
        frame.pixels.resize(width * height * 4);

        // Get the pixel data from the bitmap
        GetDIBits(screenDC, bitmap, 0, height, frame.pixels.data(), &bmi, DIB_RGB_COLORS);

        // The bitmap data is BGRA, but Raylib expects RGBA. Swap R and B channels.
        for (size_t i = 0; i < frame.pixels.size(); i += 4) {
            std::swap(frame.pixels[i], frame.pixels[i + 2]); // Swap B and R
        }

        frame.timestamp = std::chrono::steady_clock::now();
        frame.screenIndex = static_cast<int>(monitorIndex);
        frame.isValid = true;

        frameQueue.push(std::move(frame));

        // Wait to maintain the desired capture rate
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(captureRate.load() * 1000)));
    }

    // Cleanup GDI objects
    DeleteObject(bitmap);
    DeleteDC(memoryDC);
    ReleaseDC(NULL, screenDC);

    isRunning = false;
}

bool ScreenCapture::initialize() {
    if (isRunning) return true;

    g_monitors.clear();
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    if (g_monitors.empty()) {
        // *** FIX: Write error messages to std::cerr, NOT std::cout ***
        // This prevents text from corrupting the stdout video stream.
        //std::cerr << "Error: No monitors found for screen capture." << std::endl;
        return false;
    }

    shouldStop = false;

    for (size_t i = 0; i < g_monitors.size(); ++i) {
        captureThreads.emplace_back(
            std::make_unique<std::thread>([i]() {
                ScreenCapture::captureThreadFunction(i);
                })
        );
    }

    return true;
}

int ScreenCapture::getScreenCount() {
    return static_cast<int>(g_monitors.size());
}

void ScreenCapture::cleanup() {
    shouldStop = true;

    for (auto& t : captureThreads) {
        if (t && t->joinable()) {
            t->join();
        }
    }
    captureThreads.clear();

    // Clear any remaining frames in the queue
    while (!frameQueue.empty()) {
        frameQueue.tryPop();
    }
}

std::optional<CapturedFrame> ScreenCapture::getLatestFrame() {
    return frameQueue.tryPop();
}

void ScreenCapture::setCaptureRate(float fps) {
    if (fps > 0) {
        captureRate = 1.0f / fps;
    }
}

bool ScreenCapture::isInitialized() {
    return isRunning;
}

size_t ScreenCapture::getQueueSize() {
    return frameQueue.size();
}
