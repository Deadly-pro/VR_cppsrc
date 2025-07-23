#include "platform.h"
#include "vr_desktop_render.h"
#include "rlgl.h"
#include "player.h"
#include <raymath.h>
#include <fstream>
VRDesktopRenderer::VRDesktopRenderer()
    : texturesInitialized(false),
    maxUpdateRate(30.0f),
    panelDistance(2.0f),
    panelSpacing(0.5f),
    panelWidth(1.2f),
    panelHeight(0.7f)
{
}

VRDesktopRenderer::~VRDesktopRenderer()
{
    cleanup();
}

void VRDesktopRenderer::initialize(Player& player)
{
    size_t screenCount = ScreenCapture::getScreenCount();
    desktopTextures.resize(screenCount);

    for (size_t i = 0; i < screenCount; ++i) {
        Image img = GenImageColor(1920, 1080, WHITE); // Placeholder texture
        desktopTextures[i] = LoadTextureFromImage(img);
        UnloadImage(img);
    }

    lastUpdate = std::chrono::steady_clock::now();
    texturesInitialized = true;
}

void VRDesktopRenderer::cleanup()
{
    for (auto& tex : desktopTextures) {
        if (tex.id > 0) {
            UnloadTexture(tex);
        }
    }
    desktopTextures.clear();
    texturesInitialized = false;
}

void VRDesktopRenderer::update()
{
    if (!texturesInitialized) return;

    // The update rate check can remain to prevent updating textures too frequently
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - lastUpdate).count();

    if (elapsed < (1.0f / maxUpdateRate)) {
        return;
    }
    lastUpdate = now;

    // --- MODIFICATION: Process frames using pointers and release them ---
    std::optional<CapturedFrame*> frameOpt;
    while ((frameOpt = ScreenCapture::getLatestFrame())) {
        CapturedFrame* frame = *frameOpt; // Get pointer from optional

        if (!frame->isValid || frame->screenIndex >= desktopTextures.size()) {
            ScreenCapture::releaseFrame(frame); // Release invalid frame
            continue;
        }

        Image img = {
            .data = frame->pixels.data(),
            .width = frame->width,
            .height = frame->height,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
        };

        UpdateTexture(desktopTextures[frame->screenIndex], img.data);

        // *** CRUCIAL: Release the frame back to the pool after use ***
        ScreenCapture::releaseFrame(frame);
    }
}
void VRDesktopRenderer::renderDesktopPanels(Player& player, Camera3D camera)
{
    if (!texturesInitialized) return;

    // === Configurable ===
    float panelHeightOffset = -1.0f;      // Panels sit at eye-level + offset
    float panelForwardDistance = 1.0f;   // Distance from player
    float panelSpacingFactor = 0.25f;    // Gap between panels (m)

    // === Player Positioning ===
    Vector3 playerPos = player.GetPosition();      // World origin of player
    Vector3 playerFwd = Vector3Normalize(player.GetForward());  // Where player looks
    Vector3 right = Vector3Normalize(Vector3CrossProduct(playerFwd, { 0, 1, 0 }));

    // === Position Panels in Front of Player ===
    Vector3 baseForward = Vector3Scale(playerFwd, panelForwardDistance);
    Vector3 basePos = Vector3Add(playerPos, baseForward);
    basePos.y += panelHeightOffset;

    size_t numPanels = desktopTextures.size();
    float totalWidth = numPanels * panelWidth + (numPanels - 1) * panelSpacingFactor;
    Vector3 leftOffset = Vector3Scale(right, -totalWidth / 2.0f + panelWidth / 2.0f);

    for (size_t i = 0; i < numPanels; ++i) {
        Vector3 offset = Vector3Scale(right, i * (panelWidth + panelSpacingFactor));
        Vector3 panelPos = Vector3Add(basePos, Vector3Add(leftOffset, offset));

        float texAspect = (float)desktopTextures[i].height / desktopTextures[i].width;
        Vector2 size = { panelWidth, panelWidth * texAspect };
        //panel 
        DrawBillboardRec(
            camera,
            desktopTextures[i],
            { 0, 0, (float)desktopTextures[i].width, (float)desktopTextures[i].height },
            panelPos,
            size,
            WHITE
        );

        // Debug cube to verify location
        // DrawCube(panelPos, 0.05f, 0.05f, 0.05f, RED);
    }
}

void VRDesktopRenderer::setMaxUpdateRate(float fps)
{
    maxUpdateRate = fps;
}

bool VRDesktopRenderer::isTextureReady() const
{
    return texturesInitialized && !desktopTextures.empty();
}

size_t VRDesktopRenderer::getQueueSize() const
{
    return ScreenCapture::getQueueSize();
}