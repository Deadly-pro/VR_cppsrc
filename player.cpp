#include "player.h"
#include "raymath.h"
#include <cmath>
#define DEGTORAD (PI / 180.0f)


Player::Player() {
    position = { 0.0f, 1.6f, 0.0f };
    rotation = { 0.0f, 0.0f,0.0f };
    camera = { 0 };
    camera.fovy = 90.0f;
    camera.up = { 0.0f, 1.0f, 0.0f };
    camera.projection = CAMERA_PERSPECTIVE;

    laserUV = { 0 };
    laserIntersecting = false;

    // initialize VR hands
    leftHand.is_tracked = false;
    rightHand.is_tracked = false;
    leftHand.landmarks.resize(21);
    rightHand.landmarks.resize(21);
}

void Player::SetYawPitchRoll(float yaw, float pitch, float roll) {
	pitch = -pitch; 
    yaw = -yaw;
    pitch -= DEGTORAD * (90.f);
    while (yaw > 2.0f * PI) yaw -= 2.0f * PI;
    while (yaw < 0.0f) yaw += 2.0f * PI;
    if (yaw > PI) yaw -= 2.0f * PI;
    pitch = Clamp(pitch, -2.0f*PI, PI *2.0f);
	roll = Clamp(roll, -PI / 1.0f, PI / 1.0f); 
    float smoothYaw = yaw, smoothPitch = pitch, smoothRoll = roll;
    const float smoothingFactor = 0.15f;  // Adjust for more/less smoothing
    smoothYaw = smoothYaw * (1.0f - smoothingFactor) + yaw * smoothingFactor;
    smoothPitch = smoothPitch * (1.0f - smoothingFactor) + pitch * smoothingFactor;
    smoothRoll = smoothRoll * (1.0f - smoothingFactor) + roll * smoothingFactor;
    rotation = { smoothPitch,smoothYaw, smoothRoll };
}

void Player::HandleMouseLook(Vector2 delta) {
    rotation.y += delta.x * 0.003f;
    rotation.x += delta.y * 0.003f;
}

void Player::SetPanelInfo(const Vector3& pos, const Vector3& size) {
    panelPos = pos;
    panelSize = size;
}

void Player::Update() {
    camera.position = position;

    Vector3 forward = {
        cosf(rotation.y) * cosf(rotation.x),
        sinf(rotation.x),
        sinf(rotation.y) * cosf(rotation.x)
    };
    camera.target = Vector3Add(position, forward);
}
void Player::UpdateVRHand(VRHand& hand, const HandTrackingData& handData) {
    hand.label = handData.handedness;
    hand.is_tracked = !handData.landmarks.empty();
    hand.confidence = 1.0f;
    hand.estimated_depth = 1.0f;

    if (hand.is_tracked && handData.landmarks.size() >= 21) {
        Vector3 anchorPos = ComputeHandAnchorPosition(handData.handedness);

        // Get camera axes
        Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
        Vector3 up = camera.up;

        for (size_t i = 0; i < 21; ++i) {
            Vector3 local = handData.landmarks[i];

            // Remap from [0,1] → [-0.5,0.5]
            local.x = (local.x - 0.5f);
            local.y = (local.y - 0.5f);
            local.z = (local.z - 0.5f);

            // Scale hand size
            float handWidth = 0.25f;
            float handHeight = 0.25f;
            float handDepth = 0.25f;

            local.x *= handWidth;
            local.y *= handHeight;
            local.z *= handDepth;

            // Rotate into world space
            Vector3 rotated =
                Vector3Add(
                    Vector3Add(
                        Vector3Scale(right, local.x),
                        Vector3Scale(up, local.y)),
                    Vector3Scale(forward, local.z));

            Vector3 worldPos = Vector3Add(anchorPos, rotated);
            if (handData.handedness == "Left") {
                local.x = -local.x;
            }
            hand.landmarks[i].position = worldPos;
            hand.landmarks[i].active = true;
            hand.landmarks[i].confidence = 1.0f;
            hand.landmarks[i].landmark_id = static_cast<int>(i);
        }
    }
    else {
        hand.is_tracked = false;
        for (auto& lm : hand.landmarks) {
            lm.active = false;
        }
    }
}

Vector3 Player::ComputeHandAnchorPosition(const std::string& handedness) {
    Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    Vector3 up = camera.up;

    float zOffset = 0.5f;
    float sideOffset = 0.25f;
    float verticalOffset = -0.2f;

    Vector3 anchor = camera.position;
    anchor = Vector3Add(anchor, Vector3Scale(forward, zOffset));
    if (handedness == "Left") {
        anchor = Vector3Add(anchor, Vector3Scale(right, -sideOffset));
    }
    else {
        anchor = Vector3Add(anchor, Vector3Scale(right, sideOffset));
    }
    anchor = Vector3Add(anchor, Vector3Scale(up, verticalOffset));

    return anchor;
}

Camera3D Player::GetLeftEyeCamera(float eyeSeparation) {
    Camera3D left = camera;
    Vector3 right = Vector3Normalize(Vector3CrossProduct(camera.target, camera.up));
    left.position = Vector3Subtract(camera.position, Vector3Scale(right, eyeSeparation / 2.0f));
    left.target = Vector3Subtract(camera.target, Vector3Scale(right, eyeSeparation / 2.0f));
    return left;
}

Camera3D Player::GetRightEyeCamera(float eyeSeparation) {
    Camera3D rightCam = camera;
    Vector3 right = Vector3Normalize(Vector3CrossProduct(camera.target, camera.up));
    rightCam.position = Vector3Add(camera.position, Vector3Scale(right, eyeSeparation / 2.0f));
    rightCam.target = Vector3Add(camera.target, Vector3Scale(right, eyeSeparation / 2.0f));
    return rightCam;
}

bool Player::GetVRMouseData(Vector2& uv, bool& leftClick, bool& rightClick, bool& isDragging) {
    uv = laserUV;
    leftClick = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);   // Placeholder for gesture
    rightClick = IsMouseButtonPressed(MOUSE_RIGHT_BUTTON); // Placeholder
    isDragging = IsMouseButtonDown(MOUSE_LEFT_BUTTON);     // Placeholder
    return laserIntersecting;
}
void Player::DrawVRHand(const VRHand& hand) {
    if (!hand.is_tracked) return;

    Color color = (hand.label == "Left") ? SKYBLUE : ORANGE;

    // Draw landmarks
    for (const auto& lm : hand.landmarks) {
        if (lm.active) {
            float size = (lm.landmark_id == 0) ? 0.015f : 0.01f;
            DrawSphere(lm.position, size, color);
        }
    }

    // Draw connections
    const int connections[][2] = {
        {0,1}, {1,2}, {2,3}, {3,4},
        {0,5}, {5,6}, {6,7}, {7,8},
        {0,9}, {9,10}, {10,11}, {11,12},
        {0,13}, {13,14}, {14,15}, {15,16},
        {0,17}, {17,18}, {18,19}, {19,20},
        {5,9}, {9,13}, {13,17}
    };

    for (const auto& conn : connections) {
        if (hand.landmarks[conn[0]].active && hand.landmarks[conn[1]].active) {
            DrawLine3D(hand.landmarks[conn[0]].position,
                hand.landmarks[conn[1]].position,
                color);
        }
    }
}

void Player::DrawHands(const std::vector<HandTrackingData>& hands) {
    // Update left/right hand objects
    for (const auto& hand : hands) {
        if (hand.handedness == "Left") {
            UpdateVRHand(leftHand, hand);
        }
        else if (hand.handedness == "Right") {
            UpdateVRHand(rightHand, hand);
        }
    }

    // Draw both hands
    DrawVRHand(leftHand);
    DrawVRHand(rightHand);
}

void Player::DrawLaserPointer() {
    Vector3 dir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Ray ray = { camera.position, dir };

    Vector3 hit = { 0 };
    laserIntersecting = false;

    // Simple plane intersection (Z plane)
    float t = (panelPos.z - ray.position.z) / ray.direction.z;
    if (t > 0 && t < 100.0f) {
        hit = Vector3Add(ray.position, Vector3Scale(ray.direction, t));

        Vector3 rel = Vector3Subtract(hit, panelPos);
        if (fabs(rel.x) <= panelSize.x / 2 && fabs(rel.y) <= panelSize.y / 2) {
            laserUV = {
                (rel.x + panelSize.x / 2) / panelSize.x,
                1.0f - (rel.y + panelSize.y / 2) / panelSize.y
            };
            laserIntersecting = true;
            DrawSphere(hit, 0.015f, YELLOW);
        }
    }

    Vector3 laserEnd = Vector3Add(ray.position, Vector3Scale(ray.direction, 100.0f));
    DrawLine3D(ray.position, laserEnd, RED);
}
