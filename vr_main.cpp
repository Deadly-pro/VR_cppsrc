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
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;
ThreadSafeQueue<GyroData> gyroQueue;
GyroData latestGyro = { 0.0f, 0.0f, 0.0f };

// Define missing functions
template<typename T>
T Clamp(T value, T minVal, T maxVal) {
    return (value < minVal) ? minVal : (value > maxVal) ? maxVal : value;
}

static boost::interprocess::mapped_region* handRegion = nullptr;
static std::unique_ptr<boost::interprocess::file_mapping> handFile;

// Updated header structure to match Python expectations
struct FrameHeader {
    uint32_t magic = 0xDEADBEEF;
    uint32_t timestamp_ms;
    uint32_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;  // 0=RGBA, 1=RGB, 2=H264
}; 

std::vector<HandTrackingData> ReadHandTrackingData(const std::string& filename);
bool isStdoutPiped();
uint32_t GetCurrentTimeMs();
bool SendH264Frame(const std::vector<uint8_t>& frameData, int width, int height);

// -------- H264 Encoder Class --------
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
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
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
// -------- Main Function --------
int main(void) {
    std::ofstream debugLog("debug.log", std::ios::app);
    debugLog << "[START] VR process launched with H.264 encoding\n";
    std::thread gyroThread(GyroStdinReaderThread, std::ref(gyroQueue));
    gyroThread.detach();
    debugLog << "[INFO] Started GyroStdinReaderThread\n";

    if (!isStdoutPiped()) {
        debugLog << "[ERROR] Stdout is not piped. Exiting.\n";
		//return 1; temporary fix to allow piping without waiting for stdout to be piped for go side
    }
    const int screenWidth = 1920;
    const int screenHeight = 1080;

    SetTraceLogLevel(LOG_NONE);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI | FLAG_WINDOW_HIDDEN);
    InitWindow(screenWidth, screenHeight, "VR Hand Viewer");

    // Redirect stderr to null and setup stdout for binary data
    FILE* nullout = nullptr;
    freopen_s(&nullout, "NUL", "w", stderr);
    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, nullptr, _IONBF, 0);

    RenderTexture2D target = LoadRenderTexture(screenWidth, screenHeight);
    VRDesktopRenderer desktopRenderer;
    desktopRenderer.initialize();
    desktopRenderer.setMaxUpdateRate(60.0f);

    Player player;
    const float eyeSeparation = 0.065f;
    Vector2 lastMousePos = { 0 };
    bool firstMouse = true;
    Vector3 panelPosition = { 0.0f, 1.8f, 4.0f };
    Vector3 panelSize = { 17.60f, 5.0f, 0.1f };

    fs::path exePath = fs::absolute(fs::path(__argv[0]));
    fs::path sharedDir = exePath.parent_path().parent_path().parent_path().parent_path() / "Shared";
    std::string handFilePath = (sharedDir / "hands.dat").string();
    std::string gyroFilePath = (sharedDir / "gyro.dat").string();

    debugLog << "[INFO] Hand file path: " << handFilePath << std::endl;
    debugLog << "[INFO] Gyro file path: " << gyroFilePath << std::endl;

    std::unique_ptr<H264Encoder> encoder;
    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    const auto targetFrameTime = std::chrono::microseconds(1000000 / 300); // 300 FPS

    while (!WindowShouldClose()) {
        auto currentTime = std::chrono::high_resolution_clock::now();

        Vector2 mousePos = GetMousePosition();
        if (firstMouse) {
            lastMousePos = mousePos;
            firstMouse = false;
        }
        Vector2 delta = { mousePos.x - lastMousePos.x, mousePos.y - lastMousePos.y };
        lastMousePos = mousePos;
        player.HandleMouseLook(delta);
        debugLog << "[DEBUG] Start of if \n";
        auto gyroOpt = gyroQueue.tryPop();
		debugLog << "[DEBUG] gyroOpt: " << (gyroOpt.has_value() ? "true" : "false") << "\n";
        if((gyroOpt.has_value()?true:false)) {
        latestGyro = gyroOpt.value();
        debugLog << "[DEBUG] Entered \n";
		//for debugging purposes
                if (latestGyro.yaw != 0.0f || latestGyro.pitch != 0.0f || latestGyro.roll != 0.0f) {  
                    debugLog << "[DEBUG]latestGyro values not 0 \n";
                    debugLog << "[INFO] Gyro data received: Yaw=" << latestGyro.yaw << ", Pitch=" << latestGyro.pitch << ", Roll=" << latestGyro.roll << std::endl;  
                } else {  
                    debugLog << "[INFO] No new gyro data available, using last known values." << std::endl;  
                }
            
        player.SetYawPitchRoll(latestGyro.yaw, latestGyro.pitch, latestGyro.roll);
        }

        auto handData = ReadHandTrackingData(handFilePath);


        player.Update();
        desktopRenderer.update();
        player.SetPanelInfo(panelPosition, panelSize);

        BeginTextureMode(target);
        ClearBackground(BLACK);

        float gap = 30.0f;

        // Left eye
        rlViewport(0, 0, screenWidth / 2, screenHeight);
        BeginMode3D(player.GetLeftEyeCamera(eyeSeparation));
        DrawGrid(20, 1.0f);
        desktopRenderer.renderDesktopPanel(panelPosition, panelSize);
        player.DrawHands(handData);
        player.DrawLaserPointer();
        EndMode3D();

        // Right eye
        rlViewport((screenWidth / 2) + (int)gap, 0, screenWidth / 2, screenHeight);
        BeginMode3D(player.GetRightEyeCamera(eyeSeparation));
        DrawGrid(20, 1.0f);
        desktopRenderer.renderDesktopPanel(panelPosition, panelSize);
        player.DrawHands(handData);
        player.DrawLaserPointer();
        EndMode3D();

        rlViewport(0, 0, screenWidth, screenHeight);
        EndTextureMode();

        BeginDrawing();
        EndDrawing();

        // Frame Rate Control
        auto elapsedTime = std::chrono::high_resolution_clock::now() - lastFrameTime;
        if (elapsedTime >= targetFrameTime) {
            lastFrameTime = currentTime;

            // Grab Frame and Encode
            Image frame = LoadImageFromTexture(target.texture);
            ImageFlipVertical(&frame);

            if (!encoder) {
                try {
                    encoder = std::make_unique<H264Encoder>(frame.width, frame.height, 120);
                    debugLog << "[INFO] H.264 encoder initialized: " << frame.width << "x" << frame.height << std::endl;
                }
                catch (const std::exception& e) {
                    debugLog << "[ERROR] Failed to initialize encoder: " << e.what() << std::endl;
                    UnloadImage(frame);
                    break;
                }
            }

            try {
                auto encoded = encoder->encodeFrame((uint8_t*)frame.data);

                if (!encoded.empty()) {
                    if (!SendH264Frame(encoded, frame.width, frame.height)) {
                        debugLog << "[ERROR] Failed to send H.264 frame" << std::endl;
                        UnloadImage(frame);
                        break;
                    }
                }
            }
            catch (const std::exception& e) {
                debugLog << "[ERROR] Encoding error: " << e.what() << std::endl;
            }

            UnloadImage(frame);
        }
        else {
            // Sleep for remaining time to maintain frame rate
			//std::this_thread::sleep_for(targetFrameTime - elapsedTime); // caused issues with timing
        }
    }

    // Cleanup
    if (handRegion) {
        delete handRegion;
        handRegion = nullptr;
    }
    handFile.reset();

    desktopRenderer.cleanup();
    UnloadRenderTexture(target);
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
        errorLog << "Error sending H.264 frame: " << e.what() << std::endl;
        return false;
    }
}

std::vector<HandTrackingData> ReadHandTrackingData(const std::string& filename) {
    namespace bip = boost::interprocess;
    std::vector<HandTrackingData> handData;

    try {
        if (!fs::exists(filename)) {
            return handData;
        }

        if (!handFile) {
            handFile = std::make_unique<bip::file_mapping>(filename.c_str(), bip::read_only);
            handRegion = new bip::mapped_region(*handFile, bip::read_only);
        }

        const char* mem = static_cast<const char*>(handRegion->get_address());
        size_t regionSize = handRegion->get_size();

        if (regionSize < sizeof(uint32_t)) {
            return handData;
        }

        uint32_t size;
        memcpy(&size, mem, sizeof(uint32_t));

        if (size == 0 || size > regionSize - sizeof(uint32_t)) {
            return handData;
        }

        std::string json_data(mem + sizeof(uint32_t), size);
        auto parsed = nlohmann::json::parse(json_data);

        for (const auto& hand : parsed) {
            HandTrackingData tracked;
            tracked.handedness = hand.value("handedness", "");
            tracked.distance_factor = hand.value("distance_factor", 1.0f);
            tracked.depth_scale = hand.value("depth_scale", 1.0f);
            tracked.shoulder_calibrated = hand.value("shoulder_calibrated", false);
            tracked.confidence = hand.value("confidence", 0.7f);

            if (hand.contains("landmarks")) {
                for (const auto& lm : hand["landmarks"]) {
                    Vector3 pt = {
                        lm.value("x", 0.0f),
                        lm.value("y", 0.0f),
                        lm.value("z", 0.0f)
                    };
                    tracked.landmarks.push_back(pt);
                }
            }
            handData.push_back(tracked);
        }
    }
    catch (const std::exception& e) {
        std::ofstream errorLog("hand_error.log", std::ios::app);
        errorLog << "Error reading hand tracking data: " << e.what() << std::endl;
    }

    return handData;
}
/*
bool ReadGyroData(const std::string& filename, float& yaw, float& pitch, float& roll) {
    if (std::cin.eof()) return false;
    try {
        std::string line;
        if (!std::getline(std::cin, line)) {
            return false; // EOF
        }

        if (line.empty()) {
            return false;
        }

        auto j = nlohmann::json::parse(line);
        float alpha = j.value("alpha", 0.0f);
        float beta = j.value("beta", 0.0f);
        float gamma = j.value("gamma", 0.0f);
    
        if (alpha == 0.0f && beta == 0.0f && gamma == 0.0f) {
            yaw += DEG2RAD * alpha;
            pitch += DEG2RAD * gamma;
            roll += DEG2RAD * beta;
        }
        else {
            yaw = DEG2RAD * alpha;
            pitch = DEG2RAD * gamma;
            roll = DEG2RAD * beta;
        }

        return true;
    }
    catch (const std::exception& e) {
        std::ofstream errorLog("gyro_error.log", std::ios::app);
        errorLog << "Error reading gyro data: " << e.what() << std::endl;
        return false;
    }
}
*/