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

namespace fs = std::filesystem;
struct RawFrame {
    std::vector<uint8_t> pixels; // RGBA pixels
    int width;
    int height;
};

ThreadSafeQueue<GyroData> gyroQueue;
ThreadSafeQueue<HandTrackingData> handQueue;
ThreadSafeQueue<RawFrame> frameQueue;
GyroData latestGyro = { 0.0f, 0.0f, 0.0f };
HandTrackingData latestHand = {};
template<typename T>
T Clamp(T value, T minVal, T maxVal) {
    return (value < minVal) ? minVal : (value > maxVal) ? maxVal : value;
}
struct FrameHeader {
    uint32_t magic = 0xDEADBEEF;
    uint32_t timestamp_ms;
    uint32_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;  // 0=RGBA, 1=RGB, 2=H264
}; 
bool isStdoutPiped();
uint32_t GetCurrentTimeMs();
bool SendH264Frame(const std::vector<uint8_t>& frameData, int width, int height);

class H264Encoder {
public:
    H264Encoder(int width, int height, int fps)
        : width(width), height(height), fps(fps) {

        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            throw std::runtime_error("H.264 codec not found");
        }

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            throw std::runtime_error("Failed to allocate codec context");
        }

        ctx->bit_rate = 2000000;
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{ 1, fps };
        ctx->framerate = AVRational{ fps, 1 };
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->gop_size = 10;
        ctx->max_b_frames = 0;

        // Ultra-fast preset for real-time streaming
        // Add these for better rate control:
        av_opt_set(ctx->priv_data, "crf", "23", 0);  // Constant rate factor
        av_opt_set(ctx->priv_data, "rc-lookahead", "0", 0);  // No lookahead for real-time
        av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "profile", "baseline", 0);

        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to open codec");
        }

        frame = av_frame_alloc();
        if (!frame) {
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to allocate frame");
        }

        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;

        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to allocate frame buffer");
        }

        packet = av_packet_alloc();
        if (!packet) {
            av_frame_free(&frame);
            avcodec_free_context(&ctx);
            throw std::runtime_error("Failed to allocate packet");
        }

        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_RGBA,
            width, height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

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

        sws_scale(
            swsCtx,
            inData, inStride,
            0, height,
            frame->data, frame->linesize);

        frame->pts = frameIndex++;

        int ret = avcodec_send_frame(ctx, frame);
        if (ret < 0) {
            throw std::runtime_error("Error sending frame for encoding");
        }

        std::vector<uint8_t> outData;

        while ((ret = avcodec_receive_packet(ctx, packet)) == 0) {
            outData.insert(outData.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            throw std::runtime_error("Error receiving packet from encoder");
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

int main(void) {
    std::ofstream debugLog("debug.log", std::ios::app);
    debugLog << "[START] VR process launched with H.264 encoding\n";
    // config data 
    const int screenWidth = 1920;
    const int screenHeight = 1080;
    //configuring raylib 
    SetTraceLogLevel(LOG_NONE);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_WINDOW_HIDDEN);
    InitWindow(screenWidth,screenHeight,"Raylib");
    if (!IsWindowReady()) {
        debugLog << "[FATAL] Failed to create Raylib window and graphics context. Exiting.\n";
        return 1;
    }
    //setting file mode out to be stdout 
    FILE* nullout = nullptr;
    freopen_s(&nullout, "NUL", "w", stderr);
    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!isStdoutPiped()) {
        debugLog << "[ERROR] Stdout is not piped. Exiting.\n";
        //return 1; temporary fix to allow piping without waiting for stdout to be piped for go side
    }

    //initialisation of raylib objects 
    RenderTexture2D target = LoadRenderTexture(screenWidth, screenHeight);
    VRDesktopRenderer desktopRenderer;
    ScreenCapture::initialize();
    Player player;
    desktopRenderer.initialize(player);
    desktopRenderer.setMaxUpdateRate(60.0f);
    const float eyeSeparation = 0.065f;
    Vector2 lastMousePos = { 0 };
    bool firstMouse = true;

    //threads
    std::atomic<bool> running = true;
    std::thread encoderThread([&] {
        std::unique_ptr<H264Encoder> encoder = nullptr;

        while (running) {
            auto optFrame = frameQueue.tryPop();
            if (!optFrame.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            RawFrame raw = optFrame.value();

            if (!encoder) {
                try {
                    encoder = std::make_unique<H264Encoder>(raw.width, raw.height, 120);
                    std::ofstream debugLog("debug.log", std::ios::app);
                    debugLog << "[INFO] H.264 encoder initialized in thread\n";
                }
                catch (const std::exception& e) {
                    std::ofstream debugLog("debug.log", std::ios::app);
                    debugLog << "[ERROR] Encoder init failed: " << e.what() << "\n";
                    continue;
                }
            }

            try {
                auto encoded = encoder->encodeFrame(raw.pixels.data());
                if (!encoded.empty()) {
                    if (!SendH264Frame(encoded, raw.width, raw.height)) {
                        std::ofstream debugLog("debug.log", std::ios::app);
                        debugLog << "[ERROR] Failed to send encoded frame\n";
                    }
                }
            }
            catch (const std::exception& e) {
                std::ofstream debugLog("debug.log", std::ios::app);
                debugLog << "[ERROR] Encoding error: " << e.what() << "\n";
            }
        }
        });
    encoderThread.detach();
    /*std::thread gyroThread(GyroStdinReaderThread, std::ref(gyroQueue));
    gyroThread.detach();
    debugLog << "[INFO] Started GyroStdinReaderThread\n";
    std::thread handThread(HandStdinReaderThread, std::ref(handQueue));
    handThread.detach();*/

	std::thread stdinThread(StdinReaderThread, std::ref(gyroQueue), std::ref(handQueue));
	stdinThread.detach();

    // commented out the old file paths that we used to do 
    /*fs::path exePath = fs::absolute(fs::path(__argv[0]));
    fs::path sharedDir = exePath.parent_path().parent_path().parent_path().parent_path() / "Shared";
    std::string handFilePath = (sharedDir / "hands.dat").string();*/

    std::unique_ptr<H264Encoder> encoder;
    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    const auto targetFrameTime = std::chrono::microseconds(1000000 / 300); // 300 FPS
   
        while (!WindowShouldClose()) {
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto gyroOpt = gyroQueue.tryPop();
            if (gyroOpt.has_value()) {
                latestGyro = gyroOpt.value();
                player.SetYawPitchRoll(latestGyro.yaw, latestGyro.pitch, latestGyro.roll);
            }
            auto handDat = handQueue.tryPop(); //wait for hand data and try pop 
            std::vector<HandTrackingData> handData;
            if (handDat.has_value()) {
                handData.push_back(handDat.value());  
            }

            player.Update();
            desktopRenderer.update();

            BeginTextureMode(target);
            ClearBackground(BLACK);

            float gap = 30.0f;

            // Left eye
            rlViewport(0, 0, screenWidth / 2, screenHeight);
            BeginMode3D(player.GetLeftEyeCamera(eyeSeparation));
            DrawGrid(20, 1.0f);
            player.DrawHands(handData);
            desktopRenderer.renderDesktopPanels(player, player.GetLeftEyeCamera(eyeSeparation));
            EndMode3D();

            // Right eye
            rlViewport((screenWidth / 2) + (int)gap, 0, screenWidth / 2, screenHeight);
            BeginMode3D(player.GetRightEyeCamera(eyeSeparation));
            DrawGrid(20, 1.0f);
            player.DrawHands(handData);
            desktopRenderer.renderDesktopPanels(player, player.GetRightEyeCamera(eyeSeparation));
            EndMode3D();

            rlViewport(0, 0, screenWidth, screenHeight);
            EndTextureMode();
            BeginDrawing();
            ClearBackground(BLACK);
            DrawTexture(target.texture, 0, 0, WHITE);
            EndDrawing();

            // Frame Rate Control
            auto elapsedTime = std::chrono::high_resolution_clock::now() - lastFrameTime;
            if (elapsedTime >= targetFrameTime) {
                lastFrameTime = currentTime;
                //send raw frame to frame queue to encode on other thread 
                Image frame = LoadImageFromTexture(target.texture);
                ImageFlipVertical(&frame);

                RawFrame rf;
                rf.width = frame.width;
                rf.height = frame.height;
                rf.pixels.resize(frame.width * frame.height * 4);
                memcpy(rf.pixels.data(), frame.data, rf.pixels.size());
                frameQueue.push(rf);

                UnloadImage(frame);
            }
            else {
                // Sleep for remaining time to maintain frame rate
                //std::this_thread::sleep_for(targetFrameTime - elapsedTime); // caused issues with timing
            }
        }


        desktopRenderer.cleanup();
        UnloadRenderTexture(target);
        running = false; // Signal the encoder thread to stop
        CloseWindow();
        debugLog << "[END] VR process terminated\n";
        return 0;
    }


bool isStdoutPiped() {
    return !_isatty(_fileno(stdout));
	
}

uint32_t GetCurrentTimeMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count());
}

bool SendH264Frame(const std::vector<uint8_t>& frameData, int width, int height) {
    try {
        FrameHeader header;
        header.timestamp_ms = GetCurrentTimeMs();
        header.frame_size = static_cast<uint32_t>(frameData.size());
        header.width = static_cast<uint32_t>(width);
        header.height = static_cast<uint32_t>(height);
        header.pixel_format = 2;  // H264 format

        // Write header
        std::cout.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!std::cout.good()) return false;

        // Write frame data
        std::cout.write(reinterpret_cast<const char*>(frameData.data()), frameData.size());
        if (!std::cout.good()) return false;

        std::cout.flush();
        return std::cout.good();
    }
    catch (const std::exception& e) {
        std::ofstream errorLog("frame_error.log", std::ios::app);
        //errorLog << "Error sending H.264 frame: " << e.what() << std::endl;
        return false;
    }
}
