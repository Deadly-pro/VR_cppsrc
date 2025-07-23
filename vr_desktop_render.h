#pragma once

#include "platform.h"
#include "raylib.h"
#include "screen_capture.h"
#include "windows_input.h"
#include <chrono>
#include <vector>
#include "player.h"

class VRDesktopRenderer {
private:
    std::vector<Texture2D> desktopTextures;
    bool texturesInitialized;
    std::chrono::steady_clock::time_point lastUpdate;
    float maxUpdateRate;

    float panelDistance;
    float panelSpacing;
    float panelWidth;
    float panelHeight;

public:
    VRDesktopRenderer();
    ~VRDesktopRenderer();

    void initialize(Player& player);
    void cleanup();
    void update();
    void renderDesktopPanels(Player& player,Camera3D camera );
    void setMaxUpdateRate(float fps);
    bool isTextureReady() const;
    size_t getQueueSize() const;

    // VR Mouse interaction methods
   /* void sendLeftClick(int x, int y);
    void sendRightClick(int x, int y);
    void sendMouseMove(int x, int y);
    void sendMousePosition(int x, int y);
    void sendMouseDown(int x, int y);
    void sendMouseUp(int x, int y);*/
};
