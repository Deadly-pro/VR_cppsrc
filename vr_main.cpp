#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "player.h"
#include "vr_desktop_render.h"
#include <nlohmann/json.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
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
#include "gyro_thread.h"
#include "handat_thread.h"
#include "stdin_reader_thread.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// Data structure for passing raw frames to the encoder thread
struct RawFrame {
    std::vector<uint8_t> pixels; // RGBA pixels
    int width;
    int height;
};

// Thread-safe queues for inter-thread communication
ThreadSafeQueue<GyroData> gyroQueue;
ThreadSafeQueue<HandTrackingData> handQueue;
ThreadSafeQueue<RawFrame> frameQueue; // Main thread produces, encoder thread consumes

// Forward declarations
bool isStdoutPiped();
uint32_t GetCurrentTimeMs();
bool SendH264Frame(const std::vector<uint8_t>& frameData, int width, int height);

// H264 Encoder Class
class H264Encoder {
public:
    H264Encoder(int width, int height, int fps)
        : width(width), height(height), fps(fps) {

        // *** FIX: Attempt to find the NVIDIA NVENC hardware encoder first ***
        codec = avcodec_find_encoder_by_name("h264_nvenc");
        const char* encoderName = "h264_nvenc";

        // If NVENC is not found, fall back to the software encoder
        if (!codec) {
            encoderName = "libx264"; // The fast software encoder
            codec = avcodec_find_encoder_by_name(encoderName);
            if (!codec) {
                throw std::runtime_error("H.264 software encoder (libx264) not found");
            }
        }

        // Log which encoder is being used
        std::ofstream debugLog("debug.log", std::ios::app);
        debugLog << "[INFO] Using H.264 encoder: " << encoderName << "\n";


        ctx = avcodec_alloc_context3(codec);
        if (!ctx) throw std::runtime_error("Failed to allocate codec context");

        ctx->bit_rate = 16000000; // 16 Mbps for high quality VR streaming
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{ 1, fps };
        ctx->framerate = AVRational{ fps, 1 };
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->gop_size = 10;
        ctx->max_b_frames = 1;

        // *** FIX: Set options specific to the chosen encoder ***
        if (strcmp(encoderName, "h264_nvenc") == 0) {
            // Options for NVIDIA NVENC
            av_opt_set(ctx->priv_data, "preset", "p5", 0); // p5 is a good balance of speed and quality for NVENC
            av_opt_set(ctx->priv_data, "tune", "ll", 0);   // ll = low latency
            av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
        }
        else {
            // Options for libx264 (software encoder)
            av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        }


        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to open codec");
        }

        frame = av_frame_alloc();
        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to allocate frame buffer");
        }

        packet = av_packet_alloc();

        swsCtx = sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to create SWS context");
        }
    }

    ~H264Encoder() {
        if (ctx) avcodec_free_context(&ctx);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (swsCtx) sws_freeContext(swsCtx);
    }

    std::vector<uint8_t> encodeFrame(const uint8_t* rgba) {
        const uint8_t* inData[1] = { rgba };
        int inStride[1] = { 4 * width };

        sws_scale(swsCtx, inData, inStride, 0, height, frame->data, frame->linesize);
        frame->pts = frameIndex++;

        if (avcodec_send_frame(ctx, frame) < 0) {
            return {};
        }

        std::vector<uint8_t> outData;
        int ret;
        while ((ret = avcodec_receive_packet(ctx, packet)) == 0) {
            outData.insert(outData.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }

        return outData;
    }

private:
    int width, height, fps;
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* swsCtx = nullptr;
    int64_t frameIndex = 0;
};

void encoder_thread_function(std::atomic<bool>& running) {
    std::unique_ptr<H264Encoder> encoder = nullptr;
    std::ofstream debugLog("debug.log", std::ios::app);

    while (running) {
        auto optFrame = frameQueue.tryPop();
        if (!optFrame.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        RawFrame raw = std::move(optFrame.value());

        if (!encoder) {
            try {
                encoder = std::make_unique<H264Encoder>(raw.width, raw.height, 90);
                // Log is now inside the constructor
            }
            catch (const std::exception& e) {
                debugLog << "[ERROR] Encoder init failed: " << e.what() << "\n";
                return;
            }
        }

        try {
            auto encoded = encoder->encodeFrame(raw.pixels.data());
            if (!encoded.empty()) {
                if (!SendH264Frame(encoded, raw.width, raw.height)) {
                    debugLog << "[WARN] Failed to send encoded frame to stdout.\n";
                }
            }
        }
        catch (const std::exception& e) {
            debugLog << "[ERROR] Encoding exception: " << e.what() << "\n";
        }
    }
    debugLog << "[INFO] Encoder thread shutting down.\n";
}

int main(void) {
    std::ofstream debugLog("debug.log");
    debugLog << "[START] VR process launched.\n";

    av_log_set_level(AV_LOG_QUIET);

    const int screenWidth = 1920;
    const int screenHeight = 1080;

    SetTraceLogLevel(LOG_WARNING);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_WINDOW_HIDDEN);
    InitWindow(screenWidth, screenHeight, "Raylib VR Stream");
    if (!IsWindowReady()) {
        debugLog << "[FATAL] Failed to create Raylib window. Exiting.\n";
        return 1;
    }
    SetTargetFPS(90);

    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!isStdoutPiped()) {
        debugLog << "[WARN] Stdout is not piped. Video stream will go to console.\n";
    }

    Player player;
    VRDesktopRenderer desktopRenderer;
    ScreenCapture::initialize();
    desktopRenderer.initialize(player);
    desktopRenderer.setMaxUpdateRate(60.0f);
    const float eyeSeparation = 0.065f;

    std::atomic<bool> running = true;
    std::thread encoderThread(encoder_thread_function, std::ref(running));
    std::thread stdinThread(StdinReaderThread, std::ref(gyroQueue), std::ref(handQueue));
    stdinThread.detach();

    RenderTexture2D target = LoadRenderTexture(screenWidth, screenHeight);
    GyroData latestGyro = { 0.0f, 0.0f, 0.0f };

    while (!WindowShouldClose()) {
        auto gyroOpt = gyroQueue.tryPop();
        if (gyroOpt.has_value()) {
            latestGyro = gyroOpt.value();
            player.SetYawPitchRoll(latestGyro.yaw, latestGyro.pitch, latestGyro.roll);
        }

        std::vector<HandTrackingData> handData;
        auto handDat = handQueue.tryPop();
        if (handDat.has_value()) {
            handData.push_back(handDat.value());
        }

        player.Update();
        desktopRenderer.update();

        BeginTextureMode(target);
        ClearBackground(BLACK);
        {
            rlViewport(0, 0, screenWidth / 2, screenHeight);
            BeginMode3D(player.GetLeftEyeCamera(eyeSeparation));
            DrawGrid(20, 1.0f);
            player.DrawHands(handData);
            desktopRenderer.renderDesktopPanels(player, player.GetLeftEyeCamera(eyeSeparation));
            EndMode3D();

            rlViewport(screenWidth / 2, 0, screenWidth / 2, screenHeight);
            BeginMode3D(player.GetRightEyeCamera(eyeSeparation));
            DrawGrid(20, 1.0f);
            player.DrawHands(handData);
            desktopRenderer.renderDesktopPanels(player, player.GetRightEyeCamera(eyeSeparation));
            EndMode3D();
        }
        EndTextureMode();

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexture(target.texture, 0, 0, WHITE);
        EndDrawing();

        Image frameImg = LoadImageFromTexture(target.texture);
        ImageFlipVertical(&frameImg);

        if (frameImg.data != nullptr) {
            RawFrame rf;
            rf.width = frameImg.width;
            rf.height = frameImg.height;

            size_t dataSize = GetPixelDataSize(frameImg.width, frameImg.height, frameImg.format);

            if (dataSize > 0) {
                rf.pixels.resize(dataSize);
                memcpy(rf.pixels.data(), frameImg.data, dataSize);
                frameQueue.push(std::move(rf));
            }
        }
        UnloadImage(frameImg);
    }

    debugLog << "[INFO] Main loop finished. Shutting down threads...\n";
    running = false;
    encoderThread.join();

    desktopRenderer.cleanup();
    UnloadRenderTexture(target);
    CloseWindow();

    debugLog << "[END] VR process terminated successfully.\n";
    return 0;
}

bool isStdoutPiped() {
    return !_isatty(_fileno(stdout));
}

uint32_t GetCurrentTimeMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count());
}

struct FrameHeader {
    uint32_t magic = 0xDEADBEEF;
    uint32_t timestamp_ms;
    uint32_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;  // 2 = H264
};

bool SendH264Frame(const std::vector<uint8_t>& frameData, int width, int height) {
    try {
        FrameHeader header;
        header.timestamp_ms = GetCurrentTimeMs();
        header.frame_size = static_cast<uint32_t>(frameData.size());
        header.width = static_cast<uint32_t>(width);
        header.height = static_cast<uint32_t>(height);
        header.pixel_format = 2;

        std::cout.write(reinterpret_cast<const char*>(&header), sizeof(header));
        std::cout.write(reinterpret_cast<const char*>(frameData.data()), frameData.size());
        std::cout.flush();
        return std::cout.good();
    }
    catch (...) {
        return false;
    }
}