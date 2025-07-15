#pragma once
#include "raylib.h"
#include <vector>
#include <string>
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
    };

class Player {
public:
    Player();
    VRHand leftHand;
    VRHand rightHand;
    void SetYawPitchRoll(float yaw, float pitch, float roll);
    void SetPanelInfo(const Vector3& pos, const Vector3& size);
    void Update();
	void UpdateVRHand(VRHand& hand, const HandTrackingData& handData);
    Camera3D GetLeftEyeCamera(float eyeSeparation);
    Camera3D GetRightEyeCamera(float eyeSeparation);
	void DrawVRHand(const VRHand& hand);
    Vector3 ComputeHandAnchorPosition(const std::string& handedness);
    void DrawHands(const std::vector<HandTrackingData>& hands);
    Vector3 GetPosition() const;
    Vector3 GetForward() const;
    
private:
    Vector3 position;
    Camera3D camera;
    Vector3 rotation;  // yaw, pitch, roll
	float yaw, pitch, roll;
    Vector3 panelPos;
    Vector3 panelSize;
    Vector2 laserUV;
    bool laserIntersecting;
};
