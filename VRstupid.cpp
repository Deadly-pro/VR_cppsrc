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
        Image img = GenImageColor(1920, 1080, WHITE); // Placeholder texture to preload stuff assuming its 1080p
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
void PlacePanelsInArc(Vector3 playerPos, Vector3 playerFwd, float yawDeg,
    float panelDistance, float panelWidth, int panelCount, std::vector<Vector3>& outPositions)
{
    outPositions.clear();

    // Convert degrees to radians
    float yaw = DEG2RAD * yawDeg;

    // Ensure forward vector matches yaw (pitch optional)
    playerFwd = { sinf(yaw), 0.0f, cosf(yaw) };

    // Axis to rotate around (Y-up world)
    Vector3 worldUp = { 0.0f, 1.0f, 0.0f };

    // Correct angular spacing per panel
    float angleStep = panelWidth / panelDistance; // radians
    float totalArc = angleStep * (panelCount - 1);
    float startAngle = -totalArc / 2.0f;

    for (int i = 0; i < panelCount; ++i) {
        float angle = startAngle + i * angleStep;

        // Rotate the forward vector around Y axis
        Vector3 dir = Vector3RotateByAxisAngle(playerFwd, worldUp, -angle); // raylib is RH
        Vector3 pos = Vector3Add(playerPos, Vector3Scale(dir, panelDistance));
        outPositions.push_back(pos);
    }
}

void DrawFixedTexturedQuad(Texture2D texture, Vector3 corners[4], Vector3 normal)
{
    rlSetTexture(texture.id);
    rlSetBlendMode(BLEND_ALPHA);
    rlColor4f(1.0f, 1.0f, 1.0f, 1.0f); // Ensure full white color
    rlBegin(RL_QUADS);
    rlNormal3f(normal.x, normal.y, normal.z);

    // Alternative: Flip X coordinates instead
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(corners[0].x, corners[0].y, corners[0].z); // Bottom-left
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(corners[1].x, corners[1].y, corners[1].z); // Top-left
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(corners[2].x, corners[2].y, corners[2].z); // Top-right
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(corners[3].x, corners[3].y, corners[3].z); // Bottom-right

    rlEnd();
    rlSetTexture(0);
}

void VRDesktopRenderer::renderDesktopPanels(Player& player, Camera3D camera)
{
    if (!texturesInitialized || desktopTextures.empty()) return;

    static bool panelsInitialized = false;
    static Vector3 fixedPanelPositions[3];
    static Vector3 fixedPanelRotations[3]; 
    size_t panelCount = desktopTextures.size();
    if (panelCount == 0) return;
    if (!panelsInitialized) {
        Vector3 playerPos = player.GetPosition();
        Vector3 forwardDir = { 1, 0, 0 };  // Player faces +X
        float curveRadius = 1.0f;
        float baseHeight = playerPos.y - 0.15f;

        // FIXED: Simple adjacent positioning without wide arcs
        if (panelCount == 1) {
            // Single panel: directly in front
            fixedPanelPositions[0] = Vector3Add(playerPos, Vector3Scale(forwardDir, curveRadius));
            fixedPanelPositions[0].y = baseHeight;
            fixedPanelRotations[0] = { 0, 0, 0 };  // No rotation, faces player
        }
        else if (panelCount == 2) {
            // Two panels: side by side with minimal curve
            Vector3 centerPos = Vector3Add(playerPos, Vector3Scale(forwardDir, curveRadius));
            Vector3 rightDir = { 0, 0, 1 };  // +Z is right

            float panelSpacing = 1.8f;  // Spacing between panels
            float curveAngle = 15.0f;   // Very gentle curve (15 degrees)

            // Left panel (slightly angled inward)
            fixedPanelPositions[0] = Vector3Add(centerPos, Vector3Scale(rightDir, -panelSpacing / 2));
            fixedPanelPositions[0].y = baseHeight;
            fixedPanelRotations[0] = { 0, DEG2RAD * -curveAngle, 0 };  // Rotate toward center

            // Right panel (slightly angled inward)
            fixedPanelPositions[1] = Vector3Add(centerPos, Vector3Scale(rightDir, panelSpacing / 2));
            fixedPanelPositions[1].y = baseHeight;
            fixedPanelRotations[1] = { 0, DEG2RAD * curveAngle, 0 };  // Rotate toward center
        }
        else if (panelCount >= 3) {
            // Three panels: left, center, right with gentle curve
            Vector3 centerPos = Vector3Add(playerPos, Vector3Scale(forwardDir, curveRadius));
            Vector3 rightDir = { 0, 0, 1 };

            float panelSpacing = 1.3f;  // Closer spacing for 3 panels
            float curveAngle = 20.0f;   // Gentle curve angle

            // Left panel
            fixedPanelPositions[0] = Vector3Add(centerPos, Vector3Scale(rightDir, -panelSpacing));
            fixedPanelPositions[0].y = baseHeight;
            fixedPanelRotations[0] = { 0, DEG2RAD * -curveAngle, 0 };

            // Center panel  
            fixedPanelPositions[1] = centerPos;
            fixedPanelPositions[1].y = baseHeight;
            fixedPanelRotations[1] = { 0, 0, 0 };  // No rotation

            // Right panel
            fixedPanelPositions[2] = Vector3Add(centerPos, Vector3Scale(rightDir, panelSpacing));
            fixedPanelPositions[2].y = baseHeight;
            fixedPanelRotations[2] = { 0, DEG2RAD * curveAngle, 0 };
        }

        panelsInitialized = true;
    }

    // Panel sizing
    float panelWidth;
    if (panelCount == 1) {
        panelWidth = 1.8f;
    }
    else if (panelCount == 2) {
        panelWidth = 1.2f;
    }
    else {
        panelWidth = 0.8f;
    }

    float panelAspect = 9.0f / 16.0f;
    float scalingfactor = 1.5f;
    Vector2 panelSize = { panelWidth*scalingfactor, panelWidth * panelAspect*scalingfactor };
    for (size_t i = 0; i < panelCount; ++i)
    {
        Vector3 panelPos = fixedPanelPositions[i];
        Vector3 panelRot = fixedPanelRotations[i];
        Vector3 forward = { cosf(panelRot.y), 0, sinf(panelRot.y) };
        Vector3 right = Vector3Normalize(Vector3CrossProduct({ 0, 1, 0 }, forward));
        Vector3 up = { 0, 1, 0 };

        Vector3 halfWidth = Vector3Scale(right, panelSize.x / 2.0f);
        Vector3 halfHeight = Vector3Scale(up, panelSize.y / 2.0f);

        Vector3 corners[4] = {
            Vector3Subtract(Vector3Subtract(panelPos, halfWidth), halfHeight),
            Vector3Add(Vector3Subtract(panelPos, halfWidth), halfHeight),
            Vector3Add(Vector3Add(panelPos, halfWidth), halfHeight),
            Vector3Subtract(Vector3Add(panelPos, halfWidth), halfHeight)
        };

        // Use ONLY the fixed textured quad - remove billboard completely
        DrawFixedTexturedQuad(desktopTextures[i], corners, forward);

        DrawCube(panelPos, 0.05f, 0.05f, 0.05f, RED); // Debug
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