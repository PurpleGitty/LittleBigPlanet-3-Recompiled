// libSceNpManager / libSceNpAuth / libSceNetCtl as on a PS4 with no network:
// the player is signed out of PSN and the console is disconnected. LBP3
// handles that the way it does on a console without a connection: it says it
// can't connect and carries on offline. Error codes follow shadPS4.
#include <atomic>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mutex>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::np {
namespace {

constexpr int kErrorInvalidArgument = static_cast<int>(0x80550003);
constexpr int kErrorSignedOut = static_cast<int>(0x80550006);
constexpr int kErrorCallbackAlreadyRegistered = static_cast<int>(0x80550008);
constexpr int32_t kRequestIdBase = 0x10000000;
enum State : int32_t { kUnknown = 0, kSignedOut = 1, kSignedIn = 2 };

using StateCallback = void(PS4_SYSV_ABI*)(int32_t userId, int32_t state, void* npId, void* userdata);

std::mutex g_mutex;
StateCallback g_stateCallback = nullptr;
void* g_stateUserdata = nullptr;
bool g_statePending = false;  // the initial state notification hasn't been delivered yet
std::atomic<int32_t> g_nextRequest{kRequestIdBase + 1};

// "Signed out" answer for calls that need PSN; logs each function once.
int SignedOut(const char* fn) {
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard lk(m);
    if (seen.insert(fn).second) std::fprintf(stderr, "[np] %s -> signed out\n", fn);
    return kErrorSignedOut;
}

}  // namespace

int PS4_SYSV_ABI sceNpGetState(int32_t userId, int32_t* state) {
    (void)userId;
    if (!state) return kErrorInvalidArgument;
    // Experiment switch: LBP3_NP_STATE=signedin reports the account as signed
    // in while the network stays down.
    static const bool signedIn = [] {
        const char* v = std::getenv("LBP3_NP_STATE");
        return v && std::strcmp(v, "signedin") == 0;
    }();
    *state = signedIn ? kSignedIn : kSignedOut;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpRegisterStateCallback(StateCallback cb, void* userdata) {
    if (!cb) return kErrorInvalidArgument;
    std::lock_guard lk(g_mutex);
    if (g_stateCallback) return kErrorCallbackAlreadyRegistered;
    g_stateCallback = cb;
    g_stateUserdata = userdata;
    g_statePending = true;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpUnregisterStateCallback() {
    std::lock_guard lk(g_mutex);
    g_stateCallback = nullptr;
    return kernel::SCE_OK;
}

// Like the console (and shadPS4), the user's sign-in state is reported once
// after login, delivered on the game's next sceNpCheckCallback. LBP3's connect
// flow waits for it before deciding to carry on offline. Offline it's
// "signed out" with no NpId; nothing changes after that.
int PS4_SYSV_ABI sceNpCheckCallback() {
    StateCallback cb = nullptr;
    void* userdata = nullptr;
    {
        std::lock_guard lk(g_mutex);
        if (!g_statePending || !g_stateCallback) return kernel::SCE_OK;
        g_statePending = false;
        cb = g_stateCallback;
        userdata = g_stateUserdata;
    }
    constexpr int32_t kUserId = 1;  // matches system_services.cpp
    int32_t state = kSignedOut;
    sceNpGetState(kUserId, &state);
    std::fprintf(stderr, "[np] reporting sign-in state %d to the game\n", state);
    cb(kUserId, state, nullptr, userdata);
    return kernel::SCE_OK;
}

// The account is linked to PSN, so its online ID is known locally even while
// signed out (LBP3 waits for it, and shows it as the player's name).
// LBP3_PLAYER_NAME overrides the default, which is the Windows user name.
constexpr size_t kOnlineIdMax = 16;
constexpr uint64_t kAccountId = 0x0123456789ABCDEFull;

struct OnlineId { char data[kOnlineIdMax]; int8_t term; int8_t dummy[3]; };
struct NpId { OnlineId handle; uint8_t opt[8]; uint8_t reserved[8]; };

const std::string& PlayerName() {
    static const std::string name = [] {
        std::string n;
        if (const char* env = std::getenv("LBP3_PLAYER_NAME")) n = env;
        if (n.empty()) {
            char buf[64] = {};
            DWORD len = sizeof(buf);
            if (GetUserNameA(buf, &len)) n = buf;
        }
        // PSN IDs: 3-16 characters of letters, digits, '-' and '_'.
        std::string clean;
        for (char c : n)
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') clean += c;
        if (clean.size() < 3) clean = "Sackboy";
        return clean.substr(0, kOnlineIdMax);
    }();
    return name;
}

// While signed out the player's NP identity is "OfflinePlayer", the exact
// placeholder LBP3 itself uses (eboot+0x1190cd0). LBP3 is inconsistent about a
// failed sceNpGetNpId: the network manager substitutes "OfflinePlayer", but the
// session code fills the player's slot with an empty ID, so "is the local player
// in the session?" (eboot+0x36c150) never matched and the first level never
// left the loading screen. Answering with the same placeholder everywhere keeps
// one identity. (A user name would mismatch the network manager the same way.)
constexpr char kOfflinePlayer[] = "OfflinePlayer";

bool IdentityAvailable() {
    int32_t state = kSignedOut;
    sceNpGetState(0, &state);
    return state == kSignedIn;
}

void FillOnlineId(OnlineId& id) {
    std::memset(&id, 0, sizeof(id));
    static const bool realIdOffline = std::getenv("LBP3_OFFLINE_REAL_ID") != nullptr;  // EXPERIMENT
    if (IdentityAvailable() || realIdOffline) std::memcpy(id.data, PlayerName().data(), PlayerName().size());
    else std::memcpy(id.data, kOfflinePlayer, sizeof(kOfflinePlayer) - 1);
}

int PS4_SYSV_ABI sceNpGetOnlineId(int32_t, OnlineId* out) {
    if (!out) return kErrorInvalidArgument;
    FillOnlineId(*out);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpGetNpId(int32_t, NpId* out) {
    if (!out) return kErrorInvalidArgument;
    std::memset(out, 0, sizeof(*out));
    FillOnlineId(out->handle);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpGetAccountId(const void*, uint64_t* id) {
    if (!id) return kErrorInvalidArgument;
    *id = kAccountId;
    return kernel::SCE_OK;
}

// Signed out (default): everything that needs PSN answers "signed out".
// Signed in (LBP3_NP_STATE=signedin): PSN itself works like on a console whose
// account is online; only LBP3's own servers are unreachable (libSceHttp fails
// with a network error), so the game falls back to offline play itself.
bool SignedIn() {
    int32_t state = kSignedOut;
    sceNpGetState(0, &state);
    return state == kSignedIn;
}

int PS4_SYSV_ABI sceNpGetAccountCountry(const void*, char* country) {
    if (!SignedIn()) return SignedOut(__func__);
    if (country) std::memcpy(country, "us", 3);  // SceNpCountryCode: 2 chars + terminator
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNpGetAccountLanguage(int32_t, const void*, void*) { return SignedOut(__func__); }
int PS4_SYSV_ABI sceNpGetAccountDateOfBirth(int32_t, const void*, void*) { return SignedOut(__func__); }
int PS4_SYSV_ABI sceNpGetParentalControlInfo(int32_t, const void*, int8_t* age, void* info) {
    if (!SignedIn()) return SignedOut(__func__);
    if (age) *age = 0;                         // no age restriction
    if (info) std::memset(info, 0, 4);         // chat/UGC not restricted
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNpCheckNpAvailability(int32_t, const void*) {
    return SignedIn() ? kernel::SCE_OK : SignedOut(__func__);
}
int PS4_SYSV_ABI sceNpCheckPlus(int32_t, const void*, void* result) {
    if (!SignedIn()) return SignedOut(__func__);
    if (result) std::memset(result, 0, 8);     // authorized = false: no PS Plus needed offline
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNpNotifyPlusFeature(const void*) { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceNpCreateRequest() { return g_nextRequest++; }
int PS4_SYSV_ABI sceNpCreateAsyncRequest(const void*) { return g_nextRequest++; }
int PS4_SYSV_ABI sceNpDeleteRequest(int32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpAbortRequest(int32_t) { return kernel::SCE_OK; }

// Async requests finish immediately, with the same answer as the sync calls.
int PS4_SYSV_ABI sceNpPollAsync(int32_t, int32_t* result) {
    if (!result) return kErrorInvalidArgument;
    *result = SignedIn() ? kernel::SCE_OK : SignedOut(__func__);
    return kernel::SCE_OK;  // 0 = finished
}

int PS4_SYSV_ABI sceNpSetContentRestriction(const void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpSetNpTitleId(const void*, const void*) { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceNpInGameMessageInitialize(size_t, void*) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpInGameMessageTerminate(int32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpInGameMessageCreateHandle(int32_t) { return SignedOut(__func__); }
int PS4_SYSV_ABI sceNpInGameMessageDeleteHandle(int32_t, int32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNpInGameMessagePrepare(int32_t, int32_t, const void*, void*) { return SignedOut(__func__); }
int PS4_SYSV_ABI sceNpInGameMessageSendData(int32_t, int32_t, const void*, const void*, size_t) {
    return SignedOut(__func__);
}

// --- libSceNpAuth --------------------------------------------------------------

int PS4_SYSV_ABI sceNpAuthCreateRequest() { return g_nextRequest++; }
int PS4_SYSV_ABI sceNpAuthDeleteRequest(int32_t) { return kernel::SCE_OK; }
// Signed in: a dummy code. The servers it would be redeemed at are gone anyway.
int PS4_SYSV_ABI sceNpAuthGetAuthorizationCode(int32_t, const void*, char* code, int32_t* issuerId) {
    if (!SignedIn()) return SignedOut(__func__);
    if (code) {
        std::memset(code, 0, 136);  // SceNpAuthorizationCode: char[128] + padding
        std::memcpy(code, "LBP3NATIVEOFFLINE", 17);
    }
    if (issuerId) *issuerId = 1;
    return kernel::SCE_OK;
}

// --- libSceNpCommon ------------------------------------------------------------

int PS4_SYSV_ABI sceNpCmpOnlineId(const char* a, const char* b) {
    constexpr size_t kOnlineIdSize = 16;
    if (!a || !b) return kErrorInvalidArgument;
    const int result = std::strncmp(a, b, kOnlineIdSize) == 0 ? 0 : static_cast<int>(0x80550019);  // NOT_MATCH
    // LBP3_TRACE_ONLINEID: log distinct (caller, a, b) combinations.
    static const bool trace = std::getenv("LBP3_TRACE_ONLINEID") != nullptr;
    if (trace) {
        static std::mutex m;
        static std::set<std::string> seen;
        char key[128];
        std::snprintf(key, sizeof(key), "%p '%.16s' '%.16s' -> %s", __builtin_return_address(0), a, b,
                      result ? "differ" : "same");
        std::lock_guard lk(m);
        if (seen.size() < 40 && seen.insert(key).second) std::fprintf(stderr, "[np] cmp %s\n", key);
    }
    return result;
}

}  // namespace hle::np

namespace hle::netctl {
namespace {

constexpr int kErrorNotConnected = static_cast<int>(0x80412108);
constexpr int kErrorInvalidAddr = static_cast<int>(0x80412107);
constexpr int32_t kStateDisconnected = 0;

}  // namespace

// Experiment switch: LBP3_NET=online reports a working LAN connection (IP
// obtained) while every server stays unreachable, like a console whose
// internet works but whose game servers are gone.
bool LanUp() {
    static const bool up = [] {
        const char* v = std::getenv("LBP3_NET");
        return v && std::strcmp(v, "online") == 0;
    }();
    return up;
}

int PS4_SYSV_ABI sceNetCtlGetState(int32_t* state) {
    if (!state) return kErrorInvalidAddr;
    constexpr int32_t kStateIpObtained = 3;
    *state = LanUp() ? kStateIpObtained : kStateDisconnected;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNetCtlGetInfo(int32_t code, void* info) {
    if (!LanUp()) return kErrorNotConnected;
    if (!info) return kErrorInvalidAddr;
    std::memset(info, 0, 256);  // SceNetCtlInfo is a union of at most 256 bytes
    constexpr int32_t kEtherAddr = 2, kIpAddress = 14, kLinkUp = 4;
    static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x4c, 0x42, 0x33};
    if (code == kEtherAddr) std::memcpy(info, mac, sizeof(mac));
    if (code == kIpAddress) std::strcpy(static_cast<char*>(info), "192.168.1.50");
    if (code == kLinkUp) *static_cast<uint32_t*>(info) = 1;
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNetCtlGetNatInfo(void*) { return kErrorNotConnected; }

int PS4_SYSV_ABI sceNetCtlRegisterCallback(void*, void*, int32_t* id) {
    if (id) *id = 1;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNetCtlUnregisterCallback(int32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNetCtlCheckCallback() { return kernel::SCE_OK; }  // nothing ever changes

}  // namespace hle::netctl

// libSceNpUtility bandwidth test. LBP3 starts one and polls its status every frame; the
// stub never reported it finished, which left the game on the white checkpoint after
// the Bunkum Heroes intro (3000+ polls). It finishes at once with a broadband result.
namespace hle::nputil {

struct BandwidthTestResult {
    double uploadBps;
    double downloadBps;
    int32_t result;
    char padding[4];
};
constexpr int32_t kBandwidthTestFinished = 2;  // 0 none, 1 running, 2 finished
constexpr int32_t kBandwidthContext = 1;

int PS4_SYSV_ABI sceNpBandwidthTestInitStart(const void*) {
    std::fprintf(stderr, "[np] bandwidth test started (reports finished at once)\n");
    return kBandwidthContext;
}

int PS4_SYSV_ABI sceNpBandwidthTestGetStatus(int32_t, int32_t* status) {
    if (status) *status = kBandwidthTestFinished;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpBandwidthTestShutdown(int32_t, BandwidthTestResult* result) {
    if (result) *result = {20'000'000.0, 100'000'000.0, 0, {}};
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceNpBandwidthTestAbort(int32_t) { return kernel::SCE_OK; }

}  // namespace hle::nputil

namespace hle {

void RegisterNpOffline(core::Linker& linker) {
    using namespace np;
    using namespace netctl;
    using namespace nputil;
    const char* u = "libSceNpUtility";
    HLE_REG(linker, u, sceNpBandwidthTestInitStart);
    HLE_REG(linker, u, sceNpBandwidthTestGetStatus);
    HLE_REG(linker, u, sceNpBandwidthTestShutdown);
    HLE_REG(linker, u, sceNpBandwidthTestAbort);
    const char* m = "libSceNpManager";
    HLE_REG(linker, m, sceNpGetState);
    HLE_REG(linker, m, sceNpRegisterStateCallback);
    HLE_REG(linker, m, sceNpUnregisterStateCallback);
    HLE_REG(linker, m, sceNpCheckCallback);
    HLE_REG(linker, m, sceNpGetNpId);
    HLE_REG(linker, m, sceNpGetOnlineId);
    HLE_REG(linker, m, sceNpGetAccountId);
    HLE_REG(linker, m, sceNpGetAccountCountry);
    HLE_REG(linker, m, sceNpGetAccountLanguage);
    HLE_REG(linker, m, sceNpGetAccountDateOfBirth);
    HLE_REG(linker, m, sceNpGetParentalControlInfo);
    HLE_REG(linker, m, sceNpCheckNpAvailability);
    HLE_REG(linker, m, sceNpCheckPlus);
    HLE_REG(linker, m, sceNpNotifyPlusFeature);
    HLE_REG(linker, m, sceNpCreateRequest);
    HLE_REG(linker, m, sceNpCreateAsyncRequest);
    HLE_REG(linker, m, sceNpDeleteRequest);
    HLE_REG(linker, m, sceNpAbortRequest);
    HLE_REG(linker, m, sceNpPollAsync);
    HLE_REG(linker, m, sceNpSetContentRestriction);
    HLE_REG(linker, m, sceNpSetNpTitleId);
    HLE_REG(linker, m, sceNpInGameMessageInitialize);
    HLE_REG(linker, m, sceNpInGameMessageTerminate);
    HLE_REG(linker, m, sceNpInGameMessageCreateHandle);
    HLE_REG(linker, m, sceNpInGameMessageDeleteHandle);
    HLE_REG(linker, m, sceNpInGameMessagePrepare);
    HLE_REG(linker, m, sceNpInGameMessageSendData);
    HLE_REG(linker, "libSceNpAuth", sceNpAuthCreateRequest);
    HLE_REG(linker, "libSceNpAuth", sceNpAuthDeleteRequest);
    HLE_REG(linker, "libSceNpAuth", sceNpAuthGetAuthorizationCode);
    HLE_REG(linker, "libSceNpCommon", sceNpCmpOnlineId);
    const char* n = "libSceNetCtl";
    HLE_REG(linker, n, sceNetCtlGetState);
    HLE_REG(linker, n, sceNetCtlGetInfo);
    HLE_REG(linker, n, sceNetCtlGetNatInfo);
    HLE_REG(linker, n, sceNetCtlRegisterCallback);
    HLE_REG(linker, n, sceNetCtlUnregisterCallback);
    HLE_REG(linker, n, sceNetCtlCheckCallback);
}

}  // namespace hle
