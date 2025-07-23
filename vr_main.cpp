#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "player.h"
#include "vr_desktop_render.h"
#include "gyro_thread.h"
#include "handat_thread.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <chrono>
#include <thread>
#include <memory>
#include <stdexcept>
#include <atomic>
#include <mutex> // Required for thread-safe buffer swapping

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// Global thread-safe queues for input
ThreadSafeQueue<GyroData> gyroQueue;
ThreadSafeQueue<HandTrackingData> handQueue;

// --- Shared State for Thread-Safe Frame Buffering ---
struct FrameBuffer {
    RenderTexture2D texture;
    bool readyForEncoder = false;
};

std::mutex frameMutex;
FrameBuffer renderBuffers[2];
int currentRenderBuffer = 0; // The buffer the main thread is currently drawing to

// --- H264 Encoder Class (No changes needed) ---
class H264Encoder {
public:
    H264Encoder(int width, int height, int fps)
        : width(width), height(height), fps(fps) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) throw std::runtime_error("H.264 codec not found");

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) throw std::runtime_error("Failed to allocate codec context");

        ctx->bit_rate = 4000000;
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{ 1, fps };
        ctx->framerate = AVRational{ fps, 1 };
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->gop_size = 15;
        ctx->max_b_frames = 0;

        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "profile", "baseline", 0);

        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to open codec");
        }

        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!frame || !packet) { cleanup(); throw std::runtime_error("Failed to allocate frame or packet"); }

        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) { cleanup(); throw std::runtime_error("Failed to allocate frame buffer"); }

        swsCtx = sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) { cleanup(); throw std::runtime_error("Failed to create SWS context"); }
    }

    ~H264Encoder() { cleanup(); }

    std::vector<uint8_t> encodeFrame(const uint8_t* rgba) {
        const uint8_t* inData[1] = { rgba };
        int inStride[1] = { 4 * width };
        sws_scale(swsCtx, inData, inStride, 0, height, frame->data, frame->linesize);
        frame->pts = frameIndex++;
        int ret = avcodec_send_frame(ctx, frame);
        if (ret < 0) throw std::runtime_error("Error sending frame for encoding");

        std::vector<uint8_t> outData;
        while (ret >= 0) {
            ret = avcodec_receive_packet(ctx, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) throw std::runtime_error("Error receiving packet from encoder");
            outData.insert(outData.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }
        return outData;
    }

private:
    void cleanup() {
        if (swsCtx) sws_freeContext(swsCtx);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (ctx) avcodec_free_context(&ctx);
    }
    int width, height, fps;
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* swsCtx = nullptr;
    int64_t frameIndex = 0;
};

// --- Frame Header and Helper Functions (No changes needed) ---
struct FrameHeader {
    uint32_t magic = 0xDEADBEEF;
    uint32_t timestamp_ms;
    uint32_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format = 2;
};
bool isStdoutPiped() { return !_isatty(_fileno(stdout)); }
uint32_t GetCurrentTimeMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count());
}
bool SendH264Frame(const std::vector<uint8_t>& frameData, int width, int height) {
    if (frameData.empty()) return true;
    try {
        FrameHeader header;
        header.timestamp_ms = GetCurrentTimeMs();
        header.frame_size = static_cast<uint32_t>(frameData.size());
        header.width = static_cast<uint32_t>(width);
        header.height = static_cast<uint32_t>(height);

        std::cout.write(reinterpret_cast<const char*>(&header), sizeof(header));
        std::cout.write(reinterpret_cast<const char*>(frameData.data()), frameData.size());
        std::cout.flush();
        return std::cout.good();
    }
    catch (const std::exception&) { return false; }
}

// --- Encoder Thread Function ---
// This thread now does all the heavy lifting: capturing, encoding, and sending.
void EncoderWorker(std::atomic<bool>& running, std::ofstream& log) {
    std::unique_ptr<H264Encoder> encoder = nullptr;
    const auto targetFrameTime = std::chrono::microseconds(1000000 / 60); // 60 FPS video stream

    while (running) {
        auto startTime = std::chrono::high_resolution_clock::now();
        int bufferToRead = -1;

        // Safely check which buffer is ready for encoding
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            int otherBuffer = (currentRenderBuffer + 1) % 2;
            if (renderBuffers[otherBuffer].readyForEncoder) {
                bufferToRead = otherBuffer;
                renderBuffers[otherBuffer].readyForEncoder = false; // Mark it as being processed
            }
        }

        if (bufferToRead != -1) {
            // This is the only place LoadImageFromTexture is called, offloading the main thread.
            log << "reached load from texture image \n";
            Image frame = LoadImageFromTexture(renderBuffers[bufferToRead].texture.texture);
            log << "[ENCODER] frame dimensions: " << frame.width << "x" << frame.height << "\n";
            bool encodertest = true;
            if (encodertest) {
                ExportImage(frame,"encoder.png");
                log << "[ENCODER] frame saved \n";
                encodertest = false;
            }
            ImageFlipVertical(&frame);
            log << "started incoding\n";
            if (!encoder) {
                try {
                    encoder = std::make_unique<H264Encoder>(frame.width, frame.height, 60);
                    log << "[INFO] H.264 encoder initialized in thread\n";
                }
                catch (const std::exception& e) {
                    log << "[ERROR] Encoder init failed: " << e.what() << "\n";
                    UnloadImage(frame);
                    continue;
                }
            }
            try {
                auto encoded = encoder->encodeFrame(static_cast<uint8_t*>(frame.data));
                SendH264Frame(encoded, frame.width, frame.height);
            }
            catch (const std::exception& e) {
                log << "[ERROR] Encoding error: " << e.what() << "\n";
            }
            UnloadImage(frame);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto elapsedTime = endTime - startTime;
        if (elapsedTime < targetFrameTime) {
            std::this_thread::sleep_for(targetFrameTime - elapsedTime);
        }
    }
}


// --- Main Application Entry Point ---
int main(void) {
    std::ofstream debugLog("debug.log", std::ios::app);
    debugLog << "[START] VR process launched.\n";

    // --- 1. Initialization and Verification ---
    const int screenWidth = 1920;
    const int screenHeight = 1080;
    SetTraceLogLevel(LOG_NONE);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_WINDOW_HIDDEN);
    InitWindow(screenWidth, screenHeight, "VR");

    if (!IsWindowReady()) {
        debugLog << "[FATAL] Failed to create Raylib window. Exiting.\n";
        return 1;
    }

    // --- 2. Resource Loading ---
    Player player;
    VRDesktopRenderer desktopRenderer;
    desktopRenderer.initialize(player);
    desktopRenderer.setMaxUpdateRate(60.0f);

    // Initialize the Double Buffers for thread-safe rendering
    renderBuffers[0].texture = LoadRenderTexture(screenWidth, screenHeight);
    renderBuffers[1].texture = LoadRenderTexture(screenWidth, screenHeight);

    // --- 3. I/O Redirection ---
    if (isStdoutPiped()) {
        FILE* nullout = nullptr;
        freopen_s(&nullout, "NUL", "w", stderr);
        _setmode(_fileno(stdout), _O_BINARY);
        setvbuf(stdout, nullptr, _IONBF, 0);
        debugLog << "[INFO] Stdout/Stderr redirected.\n";
    }

    // --- 4. Start Worker Threads ---
    std::atomic<bool> running = true;
    std::thread gyroThread(GyroStdinReaderThread, std::ref(gyroQueue));
    std::thread handThread(HandStdinReaderThread, std::ref(handQueue));
    std::thread encoderThread(EncoderWorker, std::ref(running), std::ref(debugLog));
    debugLog << "[INFO] Worker threads started.\n";

    // --- 5. Main Render Loop ---
    const float eyeSeparation = 0.065f;
    GyroData latestGyro = { 0,0,0 };
    std::vector<HandTrackingData> latestHandData;

    while (!WindowShouldClose()) {
        // Input and Update Logic
        while (auto optionalGyro = gyroQueue.tryPop()) { latestGyro = optionalGyro.value(); }
        player.SetYawPitchRoll(latestGyro.yaw, latestGyro.pitch, latestGyro.roll);

        std::vector<HandTrackingData> currentFrameHands;
        while (auto optionalHand = handQueue.tryPop()) { currentFrameHands.push_back(optionalHand.value()); }
        if (!currentFrameHands.empty()) { latestHandData = currentFrameHands; }

        player.Update();
        ScreenCapture::initialize();
        desktopRenderer.update();
        debugLog << "Reached begin texture mode \n";
        // Render Scene to the CURRENT active buffer
        BeginTextureMode(renderBuffers[currentRenderBuffer].texture);
        ClearBackground(BLACK);

        // Left eye
        rlViewport(0, 0, screenWidth / 2, screenHeight);
        BeginMode3D(player.GetLeftEyeCamera(eyeSeparation));
        DrawGrid(20, 1.0f);
        desktopRenderer.renderDesktopPanels(player,player.GetLeftEyeCamera(eyeSeparation));
        player.DrawHands(latestHandData);
        EndMode3D();
        // Right eye
        rlViewport((screenWidth / 2) + 30, 0, screenWidth / 2, screenHeight);
        BeginMode3D(player.GetRightEyeCamera(eyeSeparation));
        DrawGrid(20, 1.0f);
        desktopRenderer.renderDesktopPanels(player,player.GetRightEyeCamera(eyeSeparation));
        player.DrawHands(latestHandData);
        EndMode3D();
        EndTextureMode();
       // /* debug png test 
        bool frameSavedForDebug = false;
        if (!frameSavedForDebug) {
            Image im = LoadImageFromTexture(renderBuffers[currentRenderBuffer].texture.texture);
            ImageFlipVertical(&im);
            ExportImage(im, "debug_frame.png");
            UnloadImage(im);
            frameSavedForDebug = true;
            debugLog << "[DIAGNOSTIC] Saved render texture to debug_frame.png. Please inspect this file.\n";
        }//*/
       
        // Draw to hidden window for display
        rlViewport(0, 0, screenWidth, screenHeight);
        
        debugLog << "Reached end texture mode \n";
        debugLog << "Started drawing \n";
        BeginDrawing();
        ClearBackground(BLACK);
        //BeginBlendMode(BLEND_ALPHA);
        //int bufferToDisplay = (currentRenderBuffer + 1) % 2;
        //DrawTextureRec(renderBuffers[bufferToDisplay].texture.texture, { 0, 0, (float)screenWidth, -(float)screenHeight }, { 0, 0 }, WHITE);
		DrawTexture(renderBuffers[currentRenderBuffer].texture.texture, 0, 0, WHITE);
        //EndBlendMode(); 
        EndDrawing();
        rlDrawRenderBatchActive();

        // Safely swap buffers for the encoder
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            renderBuffers[currentRenderBuffer].readyForEncoder = true;
            renderBuffers[(currentRenderBuffer + 1) % 2].readyForEncoder=false;
            currentRenderBuffer = (currentRenderBuffer + 1) % 2;
        }

        debugLog << "Reached end of drawing \n";
    }

    // --- 6. Cleanup ---
    running = false;
    encoderThread.join(); // It's safer to join the thread
    gyroThread.detach();
    handThread.detach();
    UnloadRenderTexture(renderBuffers[0].texture);
    UnloadRenderTexture(renderBuffers[1].texture);
    desktopRenderer.cleanup();
    CloseWindow();
    debugLog << "[END] VR process terminated.\n";
    return 0;
}
