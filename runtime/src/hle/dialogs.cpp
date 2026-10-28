// System dialogs. On a PS4 these are drawn by the system over the game; here
// they're native Windows message boxes, so they work outside the game window.
//
// libSceErrorDialog: the game passes an error code (e.g. after failing to
// reach its servers) and polls until the player dismisses it. Dismissed by
// the message box's OK button or by Cross on the gamepad (which includes
// LBP3_AUTOPRESS, so automated runs don't stall).
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hle/kernel_errors.h"
#include "hle/register.h"
#include "input/gamepad.h"

namespace hle::dialogs {
namespace {

// SceCommonDialogStatus
enum Status : int32_t { kNone = 0, kInitialized = 1, kRunning = 2, kFinished = 3 };
constexpr int kErrorNotInitialized = static_cast<int>(0x80B80003);
constexpr int kErrorBusy = static_cast<int>(0x80B80004);
constexpr int kErrorParam = static_cast<int>(0x80B8000A);
constexpr uint32_t kPadCross = 0x4000;

struct ErrorDialogParam {
    int32_t size;
    int32_t errorCode;
    int32_t userId;
    int32_t reserved;
};

std::mutex g_mutex;
std::atomic<int32_t> g_status{kNone};
std::atomic<bool> g_closed{false};  // the message box went away
std::wstring g_caption;
bool g_crossWasDown = true;  // require a fresh press, not the one that triggered the error

void ShowBox(int32_t code) {
    wchar_t text[512];
    std::swprintf(text, 512,
                  L"LittleBigPlanet 3 couldn't connect to its online services.\n\n"
                  L"You can keep playing offline with everything you've downloaded.\n\n"
                  L"Error code: %08X",
                  static_cast<uint32_t>(code));
    MessageBoxW(nullptr, text, g_caption.c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    g_closed = true;
}

bool CrossPressed() {
    ps4::PadData pad{};
    const bool down = input::Read(0, pad) && (pad.buttons & kPadCross);
    const bool pressed = down && !g_crossWasDown;
    g_crossWasDown = down;
    return pressed;
}

}  // namespace

int PS4_SYSV_ABI sceErrorDialogInitialize() {
    int32_t expected = kNone;
    if (!g_status.compare_exchange_strong(expected, kInitialized)) return kErrorBusy;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceErrorDialogOpen(const ErrorDialogParam* param) {
    if (!param) return kErrorParam;
    if (g_status != kInitialized && g_status != kFinished) return kErrorNotInitialized;
    std::fprintf(stderr, "[dialog] error dialog: code %08X\n", static_cast<uint32_t>(param->errorCode));
    {
        std::lock_guard lock(g_mutex);
        static int n = 0;
        g_caption = L"LittleBigPlanet 3 - system message " + std::to_wstring(++n);
    }
    g_closed = false;
    g_crossWasDown = true;
    g_status = kRunning;
    std::thread(ShowBox, param->errorCode).detach();
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceErrorDialogUpdateStatus() {
    if (g_status == kRunning && (g_closed || CrossPressed())) {
        if (!g_closed) {
            std::lock_guard lock(g_mutex);
            if (HWND box = FindWindowW(nullptr, g_caption.c_str())) PostMessageW(box, WM_CLOSE, 0, 0);
        }
        std::fprintf(stderr, "[dialog] error dialog dismissed\n");
        g_status = kFinished;
    }
    return g_status;
}

int PS4_SYSV_ABI sceErrorDialogGetStatus() { return g_status; }

int PS4_SYSV_ABI sceErrorDialogClose() {
    if (g_status == kRunning) g_status = kFinished;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceErrorDialogTerminate() {
    g_status = kNone;
    return kernel::SCE_OK;
}

}  // namespace hle::dialogs

namespace hle {

void RegisterDialogs(core::Linker& linker) {
    using namespace dialogs;
    const char* e = "libSceErrorDialog";
    HLE_REG(linker, e, sceErrorDialogInitialize);
    HLE_REG(linker, e, sceErrorDialogOpen);
    HLE_REG(linker, e, sceErrorDialogUpdateStatus);
    HLE_REG(linker, e, sceErrorDialogGetStatus);
    HLE_REG(linker, e, sceErrorDialogClose);
    HLE_REG(linker, e, sceErrorDialogTerminate);
}

}  // namespace hle
