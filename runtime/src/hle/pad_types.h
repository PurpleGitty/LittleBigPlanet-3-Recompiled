// PS4 libScePad data structures, laid out exactly as the game expects them.
#pragma once
#include <cstdint>

namespace ps4 {

enum PadButton : uint32_t {
    PAD_L3 = 0x2, PAD_R3 = 0x4, PAD_OPTIONS = 0x8,
    PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
    PAD_L2 = 0x100, PAD_R2 = 0x200, PAD_L1 = 0x400, PAD_R1 = 0x800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
    PAD_TOUCH_PAD = 0x100000,
};

struct PadStick { uint8_t x, y; };  // 0..255, 128 = centre, y grows downward
struct PadAnalogButtons { uint8_t l2, r2, pad[2]; };
struct FQuaternion { float x, y, z, w; };
struct FVector3 { float x, y, z; };
struct PadTouch { uint16_t x, y; uint8_t id, reserve[3]; };
struct PadTouchData { uint8_t touchNum, reserve[3]; uint32_t reserve1; PadTouch touch[2]; };
struct PadExtensionUnitData { uint32_t id; uint8_t reserve, dataLength, data[10]; };

struct PadData {
    uint32_t buttons;
    PadStick leftStick, rightStick;
    PadAnalogButtons analogButtons;
    FQuaternion orientation;
    FVector3 acceleration;
    FVector3 angularVelocity;
    PadTouchData touchData;
    bool connected;
    uint64_t timestamp;
    PadExtensionUnitData extensionUnitData;
    uint8_t connectedCount, reserve[2], deviceUniqueDataLen, deviceUniqueData[12];
};
static_assert(sizeof(PadData) == 0x78, "ScePadData layout mismatch");

struct PadControllerInformation {
    float touchPixelDensity;
    uint16_t touchResX, touchResY;
    uint8_t deadZoneLeft, deadZoneRight;
    uint8_t connectionType, connectedCount;
    bool connected;
    int32_t deviceClass;
    uint8_t reserve[8];
};
static_assert(sizeof(PadControllerInformation) == 0x1C, "ScePadControllerInformation layout mismatch");

struct PadLightBarParam { uint8_t r, g, b, reserve; };
struct PadVibrationParam { uint8_t largeMotor, smallMotor; };

}  // namespace ps4
