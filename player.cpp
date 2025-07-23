#include "player.h"
#include "raymath.h"
#include "thread_safe_queue.h"
#include <cmath>
#include "handat_thread.h"
#define DEGTORAD (PI / 180.0f)


Player::Player() {
    position = { 0.0f, 1.6f, 0.0f };
    rotation = { 0.0f, 0.0f,0.0f };
    camera = { 0 };
    camera.fovy = 90.0f;
    camera.up = { 0.0f, 1.0f, 0.0f };
    camera.projection = CAMERA_PERSPECTIVE;
    vrShaderInitialized = false;
    distortionShader.id = 0;
    laserUV = { 0 };
    laserIntersecting = false;
    //starting mediapipe
    //StartHandTracking();
    // initialize VR hands
    leftHand.is_tracked = false;
    rightHand.is_tracked = false;
    leftHand.landmarks.resize(21);
    rightHand.landmarks.resize(21);
   
}
Player::~Player() {
    if (vrShaderInitialized && distortionShader.id > 0) {
        UnloadShader(distortionShader);
    }
}
Vector3 Player::GetPosition() const {
    return position;
}
/*
#define GLSL_VERSION 330
void Player::InitializeDistortionShader() {
    if (vrShaderInitialized) return;

    // Load VR stereo config for a default device (e.g., Oculus Rift CV1 parameters)
    // You can create a VrDeviceInfo struct with your target headset's parameters.
    VrDeviceInfo device = { 0 };
    device.hResolution = 2160;
    device.vResolution = 1200;
    device.hScreenSize = 0.133793f;
    device.vScreenSize = 0.0669f;
    //device.vScreenCenter = 0.04678f;
    device.eyeToScreenDistance = 0.041f;
    device.lensSeparationDistance = 0.07f;
    device.interpupillaryDistance = 0.07f;
    device.lensDistortionValues[0] = 1.0f;
    device.lensDistortionValues[1] = 0.22f;
    device.lensDistortionValues[2] = 0.24f;
    device.lensDistortionValues[3] = 0.0f;
    device.chromaAbCorrection[0] = 0.996f;
    device.chromaAbCorrection[1] = -0.004f;
    device.chromaAbCorrection[2] = 1.014f;
    device.chromaAbCorrection[3] = 0.0f;

    VrStereoConfig config = LoadVrStereoConfig(device);

    // Load distortion shader
    // Make sure the shader file 'distortion330.fs' is in a 'resources' folder
    // relative to your executable.
    distortionShader = LoadShader(0, TextFormat("resources/distortion%i.fs", GLSL_VERSION));
    if (distortionShader.id == 0) {
        // Handle error: shader not loaded
        return;
    }

    // Update distortion shader with lens and distortion-scale parameters
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "leftLensCenter"),
        config.leftLensCenter, SHADER_UNIFORM_VEC2);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "rightLensCenter"),
        config.rightLensCenter, SHADER_UNIFORM_VEC2);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "leftScreenCenter"),
        config.leftScreenCenter, SHADER_UNIFORM_VEC2);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "rightScreenCenter"),
        config.rightScreenCenter, SHADER_UNIFORM_VEC2);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "scale"),
        config.scale, SHADER_UNIFORM_VEC2);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "scaleIn"),
        config.scaleIn, SHADER_UNIFORM_VEC2);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "deviceWarpParam"),
        device.lensDistortionValues, SHADER_UNIFORM_VEC4);
    SetShaderValue(distortionShader, GetShaderLocation(distortionShader, "chromaAbParam"),
        device.chromaAbCorrection, SHADER_UNIFORM_VEC4);

    vrShaderInitialized = true;
}

// This method applies the distortion shader to the final rendered texture
void Player::ApplyDistortion(RenderTexture2D sourceTexture) {
    if (!vrShaderInitialized || distortionShader.id == 0) {
        DrawTextureRec(sourceTexture.texture, Rectangle{ 0, 0, (float)sourceTexture.texture.width, (float)-sourceTexture.texture.height }, Vector2 { 0, 0 }, WHITE);
        return;
    }

    BeginShaderMode(distortionShader);
    DrawTextureRec(sourceTexture.texture,
        Rectangle {
        0, 0, (float)sourceTexture.texture.width, (float)-sourceTexture.texture.height
    },
        Vector2 {
        0, 0
    },
        WHITE);
    EndShaderMode();
}
*/
Vector3 Player::GetForward()  {
    float radYaw = DEG2RAD * yaw;
    float radPitch = DEG2RAD * pitch;
    return Vector3{ -(cos(radPitch) * sin(radYaw)),sin(radPitch),(cos(radPitch) * cos(radYaw)) };
}

void Player::SetYawPitchRoll(float yaw, float pitch, float roll) {
	pitch = -pitch; 
    yaw = -yaw;
    pitch -= DEGTORAD * (90.f);//offSet random shit 
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
