#include "screen_capture.h"
#include <iostream>
#include <chrono>

// Link necessary DirectX libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Define static members
std::vector<std::unique_ptr<std::thread>> ScreenCapture::captureThreads;
std::atomic<bool> ScreenCapture::shouldStop{ false };
std::atomic<bool> ScreenCapture::isRunning{ false };
ThreadSafeQueue<CapturedFrame*> ScreenCapture::frameQueue;
ThreadSafeQueue<CapturedFrame*> ScreenCapture::framePool;
ID3D11Device* ScreenCapture::d3d11Device = nullptr;
ID3D11DeviceContext* ScreenCapture::d3d11Context = nullptr;
std::vector<IDXGIOutputDuplication*> ScreenCapture::duplications;

const int FRAME_POOL_SIZE = 10; // Number of frames to pre-allocate

void ScreenCapture::captureThreadFunction(size_t monitorIndex) {
    IDXGIOutputDuplication* duplication = duplications[monitorIndex];
    DXGI_OUTDUPL_DESC outputDesc;
    duplication->GetDesc(&outputDesc);

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = outputDesc.ModeDesc.Width;
    stagingDesc.Height = outputDesc.ModeDesc.Height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = outputDesc.ModeDesc.Format;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stagingTexture = nullptr;
    HRESULT hr = d3d11Device->CreateTexture2D(&stagingDesc, NULL, &stagingTexture);
    if (FAILED(hr)) {
        std::cerr << "Failed to create staging texture for monitor " << monitorIndex << std::endl;
        return;
    }

    while (!shouldStop) {
        IDXGIResource* capturedResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        hr = duplication->AcquireNextFrame(500, &frameInfo, &capturedResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr) || !capturedResource) {
            std::cerr << "Failed to acquire next frame, re-initializing..." << std::endl;
            // Handle device reset or other errors by breaking and letting it re-initialize if needed
            break;
        }

        ID3D11Texture2D* capturedTexture = nullptr;
        capturedResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&capturedTexture));
        capturedResource->Release();

        d3d11Context->CopyResource(stagingTexture, capturedTexture);
        capturedTexture->Release();

        // Get a frame from the pool
        auto pooledFrameOpt = framePool.tryPop();
        if (!pooledFrameOpt.has_value()) {
            // Pool is empty, means consumer is too slow. Drop frame.
            duplication->ReleaseFrame();
            continue;
        }

        CapturedFrame* frame = *pooledFrameOpt;

        D3D11_MAPPED_SUBRESOURCE mappedResource;
        hr = d3d11Context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (SUCCEEDED(hr)) {
            // Copy data from staging texture to our frame buffer
            // For simplicity, we assume BGRA format and convert to RGBA for Raylib.
            uint8_t* source = reinterpret_cast<uint8_t*>(mappedResource.pData);
            uint8_t* dest = frame->pixels.data();
            int width = outputDesc.ModeDesc.Width;
            int height = outputDesc.ModeDesc.Height;

            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    int src_idx = y * mappedResource.RowPitch + x * 4;
                    int dst_idx = (y * width + x) * 4;
                    dest[dst_idx + 0] = source[src_idx + 2]; // B -> R
                    dest[dst_idx + 1] = source[src_idx + 1]; // G -> G
                    dest[dst_idx + 2] = source[src_idx + 0]; // R -> B
                    dest[dst_idx + 3] = source[src_idx + 3]; // A -> A
                }
            }
            d3d11Context->Unmap(stagingTexture, 0);
        }

        frame->width = outputDesc.ModeDesc.Width;
        frame->height = outputDesc.ModeDesc.Height;
        frame->channels = 4;
        frame->screenIndex = static_cast<int>(monitorIndex);
        frame->timestamp = std::chrono::steady_clock::now();
        frame->isValid = true;

        frameQueue.push(frame);

        duplication->ReleaseFrame();
    }

    stagingTexture->Release();
}

bool ScreenCapture::initialize() {
    if (isRunning) return true;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d3d11Device, nullptr, &d3d11Context);
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device." << std::endl;
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    d3d11Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));

    IDXGIAdapter* dxgiAdapter = nullptr;
    dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdapter));
    dxgiDevice->Release();

    IDXGIFactory* dxgiFactory = nullptr;
    dxgiAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory));

    UINT i = 0;
    IDXGIOutput* dxgiOutput = nullptr;
    while (dxgiAdapter->EnumOutputs(i, &dxgiOutput) != DXGI_ERROR_NOT_FOUND) {
        IDXGIOutput1* dxgiOutput1 = nullptr;
        dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&dxgiOutput1));
        dxgiOutput->Release();

        IDXGIOutputDuplication* duplication = nullptr;
        if (SUCCEEDED(dxgiOutput1->DuplicateOutput(d3d11Device, &duplication))) {
            duplications.push_back(duplication);
        }
        dxgiOutput1->Release();
        i++;
    }
    dxgiFactory->Release();
    dxgiAdapter->Release();

    if (duplications.empty()) {
        std::cout << "No monitors found for duplication." << std::endl;
        return false;
    }

    // --- MODIFICATION: Initialize and populate the frame pool ---
    for (int j = 0; j < FRAME_POOL_SIZE; ++j) {
        CapturedFrame* frame = new CapturedFrame();
        DXGI_OUTDUPL_DESC desc;
        duplications[0]->GetDesc(&desc); // Assuming all monitors have similar max resolution
        frame->pixels.resize(desc.ModeDesc.Width * desc.ModeDesc.Height * 4);
        framePool.push(frame);
    }

    shouldStop = false;
    for (size_t k = 0; k < duplications.size(); ++k) {
        captureThreads.emplace_back(std::make_unique<std::thread>(captureThreadFunction, k));
    }

    isRunning = true;
    return true;
}

void ScreenCapture::cleanup() {
    shouldStop = true;
    for (auto& t : captureThreads) {
        if (t && t->joinable()) {
            t->join();
        }
    }
    captureThreads.clear();

    for (auto& dup : duplications) {
        if (dup) dup->Release();
    }
    duplications.clear();

    if (d3d11Context) d3d11Context->Release();
    if (d3d11Device) d3d11Device->Release();

    while (!framePool.empty()) {
        auto frameOpt = framePool.tryPop();
        if (frameOpt.has_value()) {
            delete* frameOpt;
        }
    }
}

std::optional<CapturedFrame*> ScreenCapture::getLatestFrame() {
    return frameQueue.tryPop();
}

void ScreenCapture::releaseFrame(CapturedFrame* frame) {
    if (frame) {
        framePool.push(frame);
    }
}

int ScreenCapture::getScreenCount() {
    return static_cast<int>(duplications.size());
}

// This function now has little effect as Desktop Duplication is event-driven
void ScreenCapture::setCaptureRate(float fps) {
    // No-op
}

bool ScreenCapture::isInitialized() {
    return isRunning;
}

size_t ScreenCapture::getQueueSize() {
    return frameQueue.size();
}