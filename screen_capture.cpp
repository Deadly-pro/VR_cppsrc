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

std::vector<MonitorInfo> g_monitors;

BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
    g_monitors.push_back({ hMonitor, *lprcMonitor });
    return TRUE;
}

std::vector<std::unique_ptr<std::thread>> ScreenCapture::captureThreads;
std::atomic<bool> ScreenCapture::shouldStop{ false };
std::atomic<bool> ScreenCapture::isRunning{ false };
ThreadSafeQueue<CapturedFrame> ScreenCapture::frameQueue;
std::atomic<float> ScreenCapture::captureRate{ 1.0f / 120.0f };

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
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    while (!shouldStop) {
        BitBlt(memoryDC, 0, 0, width, height, screenDC, rc.left, rc.top, SRCCOPY);

        CapturedFrame frame;
        frame.width = width;
        frame.height = height;
        frame.channels = 4;
        frame.pixels.resize(width * height * 4);

        GetDIBits(screenDC, bitmap, 0, height, frame.pixels.data(), &bmi, DIB_RGB_COLORS);

        for (size_t i = 0; i < frame.pixels.size(); i += 4) {
            std::swap(frame.pixels[i], frame.pixels[i + 2]);
        }

        frame.timestamp = std::chrono::steady_clock::now();
        frame.screenIndex = static_cast<int>(monitorIndex);
        frame.isValid = true;

        frameQueue.push(std::move(frame));

        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(captureRate.load() * 1000)));
    }

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
        std::cout << "No monitors found." << std::endl;
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

    while (!frameQueue.empty()) {
        frameQueue.tryPop();
    }
}

std::optional<CapturedFrame> ScreenCapture::getLatestFrame() {
    return frameQueue.tryPop();
}

void ScreenCapture::setCaptureRate(float fps) {
    captureRate = 1.0f / fps;
}

bool ScreenCapture::isInitialized() {
    return isRunning;
}

size_t ScreenCapture::getQueueSize() {
    return frameQueue.size();
}
