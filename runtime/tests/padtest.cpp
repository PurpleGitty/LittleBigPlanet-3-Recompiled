// Drives the HLE libScePad exactly like the game does and prints what it sees.
// Hold A (Cross) for a rumble check. Press Back + Start together to quit.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "hle/libScePad.h"

using namespace hle::pad;

int main() {
    scePadInit();
    int handle = scePadOpen(1, 0, 0, nullptr);
    if (handle < 0) {
        std::printf("scePadOpen failed: 0x%08X\n", handle);
        return 1;
    }

    ps4::PadControllerInformation info;
    scePadGetControllerInformation(handle, &info);
    std::printf("pad handle %d, connected=%d\n", handle, info.connected);

    static const struct { uint32_t bit; const char* name; } kNames[] = {
        {ps4::PAD_CROSS, "X"}, {ps4::PAD_CIRCLE, "O"}, {ps4::PAD_SQUARE, "SQ"}, {ps4::PAD_TRIANGLE, "TRI"},
        {ps4::PAD_L1, "L1"}, {ps4::PAD_R1, "R1"}, {ps4::PAD_L2, "L2"}, {ps4::PAD_R2, "R2"},
        {ps4::PAD_L3, "L3"}, {ps4::PAD_R3, "R3"}, {ps4::PAD_OPTIONS, "OPT"}, {ps4::PAD_TOUCH_PAD, "TPAD"},
        {ps4::PAD_UP, "UP"}, {ps4::PAD_DOWN, "DN"}, {ps4::PAD_LEFT, "LT"}, {ps4::PAD_RIGHT, "RT"},
    };

    ps4::PadData d;
    for (;;) {
        scePadReadState(handle, &d);
        char held[96] = "";
        for (const auto& n : kNames) {
            if (d.buttons & n.bit) {
                std::strcat(held, n.name);
                std::strcat(held, " ");
            }
        }
        std::printf("\r%-3s L(%3u,%3u) R(%3u,%3u) L2=%3u R2=%3u  %-40s", d.connected ? "ok" : "--",
                    d.leftStick.x, d.leftStick.y, d.rightStick.x, d.rightStick.y,
                    d.analogButtons.l2, d.analogButtons.r2, held);
        std::fflush(stdout);

        ps4::PadVibrationParam v{static_cast<uint8_t>((d.buttons & ps4::PAD_CROSS) ? 128 : 0), 0};
        scePadSetVibration(handle, &v);

        if ((d.buttons & ps4::PAD_OPTIONS) && (d.buttons & ps4::PAD_TOUCH_PAD)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    ps4::PadVibrationParam off{0, 0};
    scePadSetVibration(handle, &off);
    scePadClose(handle);
    std::printf("\nbye\n");
}
