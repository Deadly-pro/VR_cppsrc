#include "vr_desktop_render.h"
#include "rlgl.h"
#include "player.h"
#include <raymath.h>

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
    camera = player.GetLeftEyeCamera(0.065f);
    size_t screenCount = ScreenCapture::getScreenCount();
    desktopTextures.resize(screenCount);

    for (size_t i = 0; i < screenCount; ++i) {
        Image img = GenImageColor(1280, 720, DARKGRAY); // Placeholder texture
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

    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - lastUpdate).count();

    if (elapsed < (1.0f / maxUpdateRate)) {
        return;
    }

    lastUpdate = now;

    // Process all captured frames
    std::optional<CapturedFrame> frameOpt;
    while ((frameOpt = ScreenCapture::getLatestFrame())) {
        CapturedFrame frame = *frameOpt;

        if (!frame.isValid || frame.screenIndex >= desktopTextures.size()) {
            continue;
        }

        Image img = {
            .data = frame.pixels.data(),
            .width = frame.width,
            .height = frame.height,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
        };

        // Update existing texture
        UpdateTexture(desktopTextures[frame.screenIndex], img.data);
    }
}

void VRDesktopRenderer::renderDesktopPanels(const Player& player)
{
    if (!texturesInitialized) return;

    Vector3 playerPos = player.GetPosition();
    Vector3 playerForward = player.GetForward();
    Vector3 right = Vector3Normalize(Vector3CrossProduct(playerForward, { 0, 1, 0 }));

    size_t numPanels = desktopTextures.size();
    float totalWidth = (numPanels * panelWidth) + ((numPanels - 1) * panelSpacing);
    Vector3 centerOffset = Vector3Scale(right, -totalWidth / 2.0f + panelWidth / 2.0f);

    for (size_t i = 0; i < numPanels; ++i) {
        Vector3 offset = Vector3Add(centerOffset, Vector3Scale(right, i * (panelWidth + panelSpacing)));
        /*Vector3 panelPos = Vector3Add(playerPos, Vector3Add(Vector3Scale(playerForward, panelDistance), offset));*/
        Vector3 panelPos = Vector3{1.0f,2.0f,0.0f};
        Vector2 panelSize = { panelWidth, panelHeight}; 
        DrawBillboardRec(
			camera,
            desktopTextures[i],
            { 0, 0, (float)desktopTextures[i].width, (float)desktopTextures[i].height },
            panelPos,
            panelSize,
            WHITE
        );
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