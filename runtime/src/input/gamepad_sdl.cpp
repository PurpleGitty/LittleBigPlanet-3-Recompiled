// SDL3 backend: XInput, DirectInput and generic HID pads, plus DualShock 4 /
// DualSense with motion sensors, touch pad and light bar. Pads are hot-plugged;
// player slots are filled in the order controllers are found.
#include "input/gamepad.h"
#include "input/actions.h"
#include "input/game_state.h"
#include "video/presenter.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <thread>
#include <mutex>
#include <sstream>
#include <fstream>
#include <deque>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace input {
namespace {

SDL_Gamepad* g_pads[kMaxPads] = {};
bool g_ready = false;

struct ButtonMap { SDL_GamepadButton sdl; uint32_t ps4; };
// Positional mapping: south/east/west/north = Cross/Circle/Square/Triangle on any pad.
constexpr ButtonMap kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, ps4::PAD_CROSS},
    {SDL_GAMEPAD_BUTTON_EAST, ps4::PAD_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST, ps4::PAD_SQUARE},
    {SDL_GAMEPAD_BUTTON_NORTH, ps4::PAD_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ps4::PAD_L1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ps4::PAD_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, ps4::PAD_L3},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, ps4::PAD_R3},
    {SDL_GAMEPAD_BUTTON_START, ps4::PAD_OPTIONS},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, ps4::PAD_TOUCH_PAD},
    {SDL_GAMEPAD_BUTTON_BACK, ps4::PAD_TOUCH_PAD},  // pads without a touch pad
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ps4::PAD_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ps4::PAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ps4::PAD_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ps4::PAD_RIGHT},
};

// The PS4 sets the digital L2/R2 bits once the trigger is past a small threshold.
constexpr uint8_t kTriggerDigitalThreshold = 30;
// DualShock 4 touch pad resolution, which the game expects touch coordinates in.
constexpr float kTouchResX = 1920.f, kTouchResY = 943.f;

uint8_t StickAxis(Sint16 v) { return static_cast<uint8_t>((static_cast<int>(v) + 32768) >> 8); }
uint8_t TriggerAxis(Sint16 v) { return static_cast<uint8_t>(v <= 0 ? 0 : v * 255 / 32767); }

bool InUse(SDL_JoystickID id) {
    for (SDL_Gamepad* p : g_pads)
        if (p && SDL_GetGamepadID(p) == id) return true;
    return false;
}

// Drops unplugged pads and puts newly plugged ones into free slots.
void RefreshSlots() {
    for (SDL_Gamepad*& p : g_pads) {
        if (p && !SDL_GamepadConnected(p)) {
            SDL_CloseGamepad(p);
            p = nullptr;
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; ++i) {
        if (InUse(ids[i])) continue;
        for (SDL_Gamepad*& p : g_pads) {
            if (p) continue;
            p = SDL_OpenGamepad(ids[i]);
            if (p) {
                SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_ACCEL, true);
                SDL_SetGamepadSensorEnabled(p, SDL_SENSOR_GYRO, true);
            }
            break;
        }
    }
    SDL_free(ids);
}

}  // namespace

void Init() {
    if (g_ready) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    g_ready = SDL_Init(SDL_INIT_GAMEPAD);
    if (g_ready) RefreshSlots();
    actions::Init();
}

bool AutoPressCross(int slot, ps4::PadData& out);

// Test-run remote control (LBP3_REMOTE_INPUT=1 only): every 50 ms the game looks
// for analysis\input_cmd.txt, queues its lines and deletes it. A line is
// "<button>[+<button>...] [ms]" (held that long, default 150), "lstick <x> <y> [ms]"
// or "rstick <x> <y> [ms]" with x/y in -1..1, or "wait <ms>". Buttons: cross circle
// square triangle options touchpad up down left right l1 r1 l2 r2 l3 r3.
namespace remote {

struct Command {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    bool left = false, right = false;
    uint64_t ms = 150;
};

std::mutex g_mutex;
std::deque<Command> g_queue;
uint64_t g_activeUntil = 0;
Command g_active;
bool g_hasActive = false;

uint32_t ButtonBit(const std::string& name) {
    static const std::pair<const char*, uint32_t> names[] = {
        {"cross", ps4::PAD_CROSS},     {"circle", ps4::PAD_CIRCLE}, {"square", ps4::PAD_SQUARE},
        {"triangle", ps4::PAD_TRIANGLE}, {"options", ps4::PAD_OPTIONS}, {"touchpad", ps4::PAD_TOUCH_PAD},
        {"up", ps4::PAD_UP},           {"down", ps4::PAD_DOWN},     {"left", ps4::PAD_LEFT},
        {"right", ps4::PAD_RIGHT},     {"l1", ps4::PAD_L1},         {"r1", ps4::PAD_R1},
        {"l2", ps4::PAD_L2},           {"r2", ps4::PAD_R2},         {"l3", ps4::PAD_L3},
        {"r3", ps4::PAD_R3}};
    for (const auto& [n, bit] : names)
        if (name == n) return bit;
    return 0;
}

void Parse(const std::string& line) {
    std::istringstream in(line);
    std::string word;
    if (!(in >> word)) return;
    Command c;
    if (word == "f9" || word == "f10") {  // the debug hotkeys (dump / perf report)
        video::TriggerHotkey(word == "f9" ? SDLK_F9 : SDLK_F10);
        return;
    }
    if (word == "action") {  // "action <name> [ms]": hold a keyboard action (input.ini names)
        std::string name;
        uint64_t ms = 150;
        in >> name >> ms;
        if (!actions::HoldForTest(name, ms)) std::fprintf(stderr, "[remote] unknown action '%s'\n", name.c_str());
        return;
    }
    if (word == "frames") {  // "frames N": record the next N presented frames
        int n = 0;
        in >> n;
        video::RequestFrameDump(n);
        return;
    }
    if (word == "wait") {
        in >> c.ms;
    } else if (word == "lstick" || word == "rstick") {
        float x = 0, y = 0;
        in >> x >> y;
        (word == "lstick" ? c.lx : c.rx) = x;
        (word == "lstick" ? c.ly : c.ry) = y;
        (word == "lstick" ? c.left : c.right) = true;
        in >> c.ms;
    } else {
        size_t start = 0;
        while (start <= word.size()) {
            const size_t plus = word.find('+', start);
            c.buttons |= ButtonBit(word.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        in >> c.ms;
    }
    std::lock_guard lock(g_mutex);
    g_queue.push_back(c);
}

void Poller() {
    for (;;) {
        Sleep(50);
        std::ifstream f("analysis\\input_cmd.txt");
        if (!f) continue;
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(f, line)) lines.push_back(line);
        f.close();
        DeleteFileA("analysis\\input_cmd.txt");
        for (const auto& l : lines) Parse(l);
    }
}

bool Apply(int slot, ps4::PadData& out) {
    static const bool enabled = [] {
        if (!std::getenv("LBP3_REMOTE_INPUT")) return false;
        std::thread(Poller).detach();
        return true;
    }();
    if (!enabled || slot != 0) return false;
    std::lock_guard lock(g_mutex);
    const uint64_t now = SDL_GetTicks();
    if (!g_hasActive || now >= g_activeUntil) {
        g_hasActive = false;
        // A short release between consecutive presses, so repeated buttons register.
        if (now < g_activeUntil + 60) return true;
        if (!g_queue.empty()) {
            g_active = g_queue.front();
            g_queue.pop_front();
            g_activeUntil = now + g_active.ms;
            g_hasActive = true;
        }
    }
    if (g_hasActive) {
        out.buttons |= g_active.buttons;
        auto axis = [](float v) { return static_cast<uint8_t>(std::clamp(128.f + v * 127.f, 0.f, 255.f)); };
        if (g_active.left) out.leftStick = {axis(g_active.lx), axis(g_active.ly)};
        if (g_active.right) out.rightStick = {axis(g_active.rx), axis(g_active.ry)};
    }
    out.connected = true;
    out.connectedCount = 1;
    return true;
}

}  // namespace remote

bool Read(int slot, ps4::PadData& out) {
    std::memset(&out, 0, sizeof(out));
    out.leftStick = out.rightStick = {128, 128};
    out.orientation = {0.f, 0.f, 0.f, 1.f};
    out.acceleration = {0.f, -1.f, 0.f};  // at rest, 1 g down
    out.timestamp = SDL_GetTicksNS() / 1000;
    if (!g_ready || slot < 0 || slot >= kMaxPads) return false;

    SDL_UpdateGamepads();
    RefreshSlots();
    if (slot == 0) game_state::CutSceneWatchdog();  // the game reads pads once a frame
    SDL_Gamepad* p = g_pads[slot];
    if (!p) {
        const bool remote = remote::Apply(slot, out) | AutoPressCross(slot, out);
        if (slot != 0) return remote;
        // Player 1 always has the keyboard, so the game sees a player without a pad.
        actions::ApplyToFrame(out);
        out.connected = true;
        out.connectedCount = 1;
        return true;
    }

    for (const auto& b : kButtons)
        if (SDL_GetGamepadButton(p, b.sdl)) out.buttons |= b.ps4;

    out.leftStick = {StickAxis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX)),
                     StickAxis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY))};
    out.rightStick = {StickAxis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX)),
                      StickAxis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY))};
    out.analogButtons.l2 = TriggerAxis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    out.analogButtons.r2 = TriggerAxis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    if (out.analogButtons.l2 > kTriggerDigitalThreshold) out.buttons |= ps4::PAD_L2;
    if (out.analogButtons.r2 > kTriggerDigitalThreshold) out.buttons |= ps4::PAD_R2;

    // TODO: verify axis directions against the game (Sixaxis-controlled objects),
    // and integrate the gyro into `orientation` if LBP3 reads it.
    float v[3];
    if (SDL_GetGamepadSensorData(p, SDL_SENSOR_ACCEL, v, 3))
        out.acceleration = {v[0] / SDL_STANDARD_GRAVITY, v[1] / SDL_STANDARD_GRAVITY, v[2] / SDL_STANDARD_GRAVITY};
    if (SDL_GetGamepadSensorData(p, SDL_SENSOR_GYRO, v, 3))
        out.angularVelocity = {v[0], v[1], v[2]};

    if (SDL_GetNumGamepadTouchpads(p) > 0) {
        for (int f = 0; f < 2; ++f) {
            bool down = false;
            float x = 0.f, y = 0.f;
            if (!SDL_GetGamepadTouchpadFinger(p, 0, f, &down, &x, &y, nullptr) || !down) continue;
            ps4::PadTouch& t = out.touchData.touch[out.touchData.touchNum++];
            t.x = static_cast<uint16_t>(x * (kTouchResX - 1));
            t.y = static_cast<uint16_t>(y * (kTouchResY - 1));
            t.id = static_cast<uint8_t>(f);
        }
    }

    AutoPressCross(slot, out);
    remote::Apply(slot, out);
    if (slot == 0) actions::ApplyToFrame(out);  // keyboard works alongside a pad
    out.connected = true;
    out.connectedCount = 1;
    return true;
}

// Debug aid for getting past prompts while nothing is drawn yet:
// LBP3_AUTOPRESS taps a button every 2 s on slot 0. Value: a button name
// (cross, circle, square, triangle, options), a list "cross,circle" to take
// turns, or anything else for cross.
bool AutoPressCross(int slot, ps4::PadData& out) {
    static const std::vector<uint32_t> buttons = [] {
        std::vector<uint32_t> list;
        const char* env = std::getenv("LBP3_AUTOPRESS");
        if (!env) return list;
        std::string v = env;
        for (const auto& [name, bit] : {std::pair{"cross", ps4::PAD_CROSS}, std::pair{"circle", ps4::PAD_CIRCLE},
                                        std::pair{"square", ps4::PAD_SQUARE}, std::pair{"triangle", ps4::PAD_TRIANGLE},
                                        std::pair{"options", ps4::PAD_OPTIONS}})
            if (v.find(name) != std::string::npos) list.push_back(bit);
        if (list.empty()) list.push_back(ps4::PAD_CROSS);
        return list;
    }();
    if (buttons.empty() || slot != 0) return false;
    const uint64_t ms = SDL_GetTicks();
    if (ms % 2000 < 150) out.buttons |= buttons[(ms / 2000) % buttons.size()];
    out.connected = true;
    out.connectedCount = 1;
    return true;
}

void SetVibration(int slot, uint8_t largeMotor, uint8_t smallMotor) {
    if (slot < 0 || slot >= kMaxPads || !g_pads[slot]) return;
    // The PS4 holds a rumble level until it's changed; SDL needs a duration, so use a long one.
    SDL_RumbleGamepad(g_pads[slot], largeMotor * 257, smallMotor * 257, 10000);
}

void SetLightBar(int slot, uint8_t r, uint8_t g, uint8_t b) {
    if (slot < 0 || slot >= kMaxPads || !g_pads[slot]) return;
    SDL_SetGamepadLED(g_pads[slot], r, g, b);
}

}  // namespace input
