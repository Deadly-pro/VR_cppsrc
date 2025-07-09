#pragma once

#include "raylib.h"
#include <vector>
#include <string>
// Define these simple structures if not declared already:

struct VRLandmark {
    Vector3 position;
    bool active;
    float confidence;
    int landmark_id;
};

struct VRHand {
    std::string label;
    bool is_tracked;
    float confidence;
    float estimated_depth;
    std::vector<VRLandmark> landmarks;
};

struct HandTrackingData {
    std::string handedness;
    std::vector<Vector3> landmarks;
    float confidence;
    float depth_scale;
    float distance_factor;
    bool shoulder_calibrated;
};

class Player {
public:
    Player();
    VRHand leftHand;
    VRHand rightHand;
    void SetYawPitchRoll(float yaw, float pitch, float roll);
    void HandleMouseLook(Vector2 delta);
    void SetPanelInfo(const Vector3& pos, const Vector3& size);
    void Update();
	void UpdateVRHand(VRHand& hand, const HandTrackingData& handData);
    Camera3D GetLeftEyeCamera(float eyeSeparation);
    Camera3D GetRightEyeCamera(float eyeSeparation);
	void DrawVRHand(const VRHand& hand);
    bool GetVRMouseData(Vector2& uv, bool& leftClick, bool& rightClick, bool& isDragging);
    Vector3 ComputeHandAnchorPosition(const std::string& handedness);
    void DrawHands(const std::vector<HandTrackingData>& hands);
    void DrawLaserPointer();

private:
    Camera3D camera;
    Vector3 position;
    Vector3 rotation;  // yaw, pitch, roll
	float yaw, pitch, roll;
    Vector3 panelPos;
    Vector3 panelSize;
    Vector2 laserUV;
    bool laserIntersecting;
};
