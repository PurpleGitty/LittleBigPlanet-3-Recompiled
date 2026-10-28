// libSceNet and the POSIX socket calls, as on a console with the cable
// unplugged: anything that would leave the machine fails with "network
// unreachable", but loopback works. LBP3 needs that: the Pod runs as a local
// game session whose host and client talk over UDP to 127.0.0.1:3658.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hle/kernel_errors.h"
#include "hle/net_delay.h"
#include "hle/register.h"

namespace hle::net {
namespace {

// FreeBSD errno values.
constexpr int kEWouldBlock = 35;
constexpr int kENetUnreach = 51;
constexpr int kETimedOut = 60;

// sceNet errors are 0x80410100 | errno.
constexpr int NetErr(int e) { return static_cast<int>(0x80410100u | static_cast<uint32_t>(e)); }

constexpr int kAfInet = 2;
constexpr int32_t kFirstId = 1000;
constexpr auto kEpollMaxSleep = std::chrono::milliseconds(100);

std::atomic<int32_t> g_nextId{kFirstId};

int* NetErrno() {
    thread_local int err = 0;
    return &err;
}

// POSIX style: -1 with errno set (both the kernel's and sceNet's copies).
int PosixFail(int e) {
    *kernel::__error() = e;
    *NetErrno() = e;
    return -1;
}

int SceFail(int e) {
    *NetErrno() = e;
    return NetErr(e);
}

// "a.b.c.d:port" from a PS4 sockaddr_in (len, family, port BE, addr).
std::string Addr(const void* sa) {
    if (!sa) return "?";
    const auto* b = static_cast<const uint8_t*>(sa);
    char buf[64];
    // SceNetSockaddrIn: len, family, port (BE), addr, vport (BE, P2P sockets), zero[6]
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u vport %u (len %u)", b[4], b[5], b[6], b[7], (b[2] << 8) | b[3],
                  (b[8] << 8) | b[9], b[0]);
    return buf;
}

void NetLog(const char* what, int id, const std::string& extra) {
    static std::atomic<int> n{0};
    if (n++ < 80) std::fprintf(stderr, "[net] %s(%d) %s\n", what, id, extra.c_str());
}

// --- loopback datagrams -------------------------------------------------------------

struct Datagram {
    std::vector<uint8_t> data;
    uint16_t fromPort, fromVport;
};
struct Socket {
    uint16_t port = 0;   // bound local port, 0 = unbound
    uint16_t vport = 0;  // bound virtual port (P2P sockets, SceNetSockaddrIn.sin_vport)
    std::deque<Datagram> inbox;
};
struct EpollEntry {
    int socket;
    uint32_t events;
    uint64_t data;
};

std::mutex g_netMutex;
std::atomic<uint64_t> g_recvCalls{0}, g_recvData{0};  // for the traffic summary
std::map<int, Socket> g_sockets;
std::map<int, std::vector<EpollEntry>> g_epolls;
uint16_t g_nextEphemeral = 49152;
constexpr uint32_t kEpollIn = 0x1;
constexpr size_t kMaxQueued = 256;  // per socket, like a full receive buffer

// PS4 sockaddr_in: len, family, port (big endian), address.
uint16_t PortOf(const void* sa) {
    const auto* b = static_cast<const uint8_t*>(sa);
    return static_cast<uint16_t>((b[2] << 8) | b[3]);
}
uint16_t VportOf(const void* sa) {
    const auto* b = static_cast<const uint8_t*>(sa);
    return static_cast<uint16_t>((b[8] << 8) | b[9]);
}
bool IsLoopback(const void* sa) { return sa && static_cast<const uint8_t*>(sa)[4] == 127; }

// Socket descriptors are small and reused, like FreeBSD file descriptors: games
// put them in select() fd_sets (1024 bits). They used to share a counter with
// epolls, which LBP3's select() creates thousands of times a second, so later
// sockets got numbers past a million and FD_SET wrote far out of bounds.
constexpr int kFirstSocket = 64, kMaxSocket = 1023;

int NewSocket() {
    std::lock_guard lk(g_netMutex);
    for (int id = kFirstSocket; id <= kMaxSocket; ++id) {
        if (g_sockets.contains(id)) continue;
        g_sockets[id];
        return id;
    }
    return -1;
}

void BindSocket(int s, const void* addr) {
    std::lock_guard lk(g_netMutex);
    g_sockets[s].port = addr ? PortOf(addr) : 0;
    g_sockets[s].vport = addr ? VportOf(addr) : 0;
}

// Delivers to the socket bound to the destination port. Returns false if
// the destination isn't on this machine.
bool SendLocal(int s, const void* data, size_t len, const void* to) {
    if (!IsLoopback(to)) return false;
    std::lock_guard lk(g_netMutex);
    Socket& from = g_sockets[s];
    if (from.port == 0) from.port = g_nextEphemeral++;  // implicit bind, like a real stack
    const uint16_t port = PortOf(to);
    {
        // Traffic summary every 5 s: message count and size spread.
        static uint64_t msgs = 0, bytes = 0, minLen = ~0ull, maxLen = 0;
        static ULONGLONG last = 0;
        ++msgs;
        bytes += len;
        minLen = std::min<uint64_t>(minLen, len);
        maxLen = std::max<uint64_t>(maxLen, len);
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 5000) {
            size_t queued = 0;
            for (auto& [id, sock] : g_sockets) queued += sock.inbox.size();
            std::fprintf(stderr, "[net] loopback: %llu messages, %llu bytes (sizes %llu..%llu) in the last 5 s; "
                         "%llu receive calls, %llu got data, %zu queued\n",
                         static_cast<unsigned long long>(msgs), static_cast<unsigned long long>(bytes),
                         static_cast<unsigned long long>(minLen), static_cast<unsigned long long>(maxLen),
                         static_cast<unsigned long long>(g_recvCalls.exchange(0)),
                         static_cast<unsigned long long>(g_recvData.exchange(0)), queued);
            msgs = bytes = maxLen = 0;
            minLen = ~0ull;
            last = now;
        }
    }
    // The first 30 messages, and the first of every new size after that: a new kind of
    // request (e.g. at a level change) shows up as a size not seen before.
    static int logged = 0;
    static std::set<size_t> seenSizes;
    const bool newSize = seenSizes.insert(len).second && seenSizes.size() < 400;
    if (logged++ < 30 || newSize) {
        std::string hex;
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len && i < 160; ++i) {
            char h[4];
            std::snprintf(h, sizeof(h), "%02x", bytes[i]);
            hex += h;
            if (i % 4 == 3) hex += ' ';
        }
        std::string text;
        for (size_t i = 0; i < len && i < 160; ++i) text += (bytes[i] >= 32 && bytes[i] < 127) ? char(bytes[i]) : '.';
        std::fprintf(stderr, "[net] loopback send %zu bytes %u -> %s: %s |%s|\n", len, from.port, Addr(to).c_str(),
                     hex.c_str(), text.c_str());
    }
    for (auto& [id, sock] : g_sockets) {
        if (sock.port != port) continue;
        if (sock.inbox.size() < kMaxQueued) {
            const auto* p = static_cast<const uint8_t*>(data);
            sock.inbox.push_back({std::vector<uint8_t>(p, p + len), from.port, from.vport});
        }
        break;
    }
    return true;  // UDP: unanswered datagrams to a closed port are just dropped
}

// Pops one datagram; -1 if none.
int64_t RecvLocal(int s, void* buf, size_t len, void* from, uint32_t* fromLen) {
    std::lock_guard lk(g_netMutex);
    ++g_recvCalls;
    auto it = g_sockets.find(s);
    if (it == g_sockets.end() || it->second.inbox.empty()) return -1;
    ++g_recvData;
    Datagram d = std::move(it->second.inbox.front());
    it->second.inbox.pop_front();
    static int logged = 0;
    if (logged++ < 30)
        std::fprintf(stderr, "[net] loopback recv %zu bytes on %u from %u\n", d.data.size(), it->second.port,
                     d.fromPort);
    const size_t n = std::min(len, d.data.size());
    if (buf && n) std::memcpy(buf, d.data.data(), n);
    if (from && fromLen && *fromLen >= 16) {
        // The sender's virtual port matters: P2P peers are identified by addr:port:vport.
        uint8_t sa[16] = {16, kAfInet, static_cast<uint8_t>(d.fromPort >> 8), static_cast<uint8_t>(d.fromPort),
                          127, 0, 0, 1, static_cast<uint8_t>(d.fromVport >> 8), static_cast<uint8_t>(d.fromVport)};
        std::memcpy(from, sa, sizeof(sa));
        *fromLen = sizeof(sa);
    }
    return static_cast<int64_t>(n);
}

void CloseSocket(int s) {
    std::lock_guard lk(g_netMutex);
    g_sockets.erase(s);
    for (auto& [ep, entries] : g_epolls)
        std::erase_if(entries, [s](const EpollEntry& e) { return e.socket == s; });
}

}  // namespace

// --- POSIX ------------------------------------------------------------------------

int PS4_SYSV_ABI socket(int family, int type, int proto) {
    const int id = NewSocket();
    if (id < 0) return PosixFail(24);  // EMFILE
    NetLog("posix socket", id, "family " + std::to_string(family) + " type " + std::to_string(type) + " proto " +
                                   std::to_string(proto));
    return id;
}
int PS4_SYSV_ABI bind(int s, const void* addr, uint32_t) {
    NetLog("posix bind", s, Addr(addr));
    BindSocket(s, addr);
    return 0;
}
int64_t PS4_SYSV_ABI recvfrom(int s, void* buf, size_t len, int, void* from, uint32_t* fromLen) {
    const int64_t n = RecvLocal(s, buf, len, from, fromLen);
    return n >= 0 ? n : PosixFail(kEWouldBlock);
}
int64_t PS4_SYSV_ABI recv(int s, void* buf, size_t len, int flags) { return recvfrom(s, buf, len, flags, nullptr, nullptr); }
int64_t PS4_SYSV_ABI sendto(int s, const void* data, size_t len, int, const void* addr, uint32_t) {
    if (SendLocal(s, data, len, addr)) return static_cast<int64_t>(len);
    NetLog("posix sendto", s, std::to_string(len) + " bytes to " + Addr(addr) + " -> unreachable");
    return PosixFail(kENetUnreach);
}

// --- libSceNet ----------------------------------------------------------------------

int PS4_SYSV_ABI sceNetInit() { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNetTerm() { return kernel::SCE_OK; }
int* PS4_SYSV_ABI sceNetErrnoLoc() { return NetErrno(); }
int PS4_SYSV_ABI sceNetPoolCreate(const char*, int, int) { return g_nextId++; }
int PS4_SYSV_ABI sceNetPoolDestroy(int) { return kernel::SCE_OK; }

int PS4_SYSV_ABI sceNetSocket(const char* name, int family, int type, int proto) {
    const int id = NewSocket();
    NetLog("socket", id, std::string(name ? name : "") + " family " + std::to_string(family) + " type " +
                             std::to_string(type) + " proto " + std::to_string(proto));
    return id;
}
int PS4_SYSV_ABI sceNetSocketClose(int s) {
    CloseSocket(s);
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNetBind(int s, const void* addr, uint32_t) {
    NetLog("bind", s, Addr(addr));
    BindSocket(s, addr);
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNetListen(int s, int backlog) {
    NetLog("listen", s, std::to_string(backlog));
    return kernel::SCE_OK;
}
int PS4_SYSV_ABI sceNetSetsockopt(int, int, int, const void*, uint32_t) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNetConnect(int s, const void* addr, uint32_t) {
    NetLog("connect", s, Addr(addr) + " -> unreachable");
    return SceFail(kENetUnreach);
}
int PS4_SYSV_ABI sceNetAccept(int, void*, uint32_t*) { return SceFail(kEWouldBlock); }
int PS4_SYSV_ABI sceNetRecv(int s, void* buf, size_t len, int) {
    const int64_t n = RecvLocal(s, buf, len, nullptr, nullptr);
    return n >= 0 ? static_cast<int>(n) : SceFail(kEWouldBlock);
}
int PS4_SYSV_ABI sceNetSend(int, const void*, size_t, int) { return SceFail(kENetUnreach); }

int PS4_SYSV_ABI sceNetEpollCreate(const char*, int) {
    const int id = g_nextId++;
    std::lock_guard lk(g_netMutex);
    g_epolls[id];
    return id;
}
int PS4_SYSV_ABI sceNetEpollDestroy(int ep) {
    std::lock_guard lk(g_netMutex);
    g_epolls.erase(ep);
    return kernel::SCE_OK;
}

// SceNetEpollEvent: events, reserved, ident, user data.
struct EpollEvent {
    uint32_t events, reserved;
    uint64_t ident;
    uint64_t data;
};

int PS4_SYSV_ABI sceNetEpollControl(int ep, int op, int id, EpollEvent* ev) {
    constexpr int kAdd = 1, kMod = 2, kDel = 3;
    std::lock_guard lk(g_netMutex);
    auto& entries = g_epolls[ep];
    std::erase_if(entries, [id](const EpollEntry& e) { return e.socket == id; });
    if ((op == kAdd || op == kMod) && ev) entries.push_back({id, ev->events, ev->data});
    (void)kDel;
    return kernel::SCE_OK;
}

// Reports sockets with queued datagrams as readable. Long or infinite waits
// are capped so the caller keeps looping.
int PS4_SYSV_ABI sceNetEpollWait(int ep, EpollEvent* events, int maxEvents, int timeoutUs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          (timeoutUs < 0 ? kEpollMaxSleep
                                         : std::min<std::chrono::milliseconds>(
                                               std::chrono::milliseconds(timeoutUs / 1000), kEpollMaxSleep));
    for (;;) {
        int n = 0;
        {
            std::lock_guard lk(g_netMutex);
            for (const auto& e : g_epolls[ep]) {
                if (n >= maxEvents) break;
                auto it = g_sockets.find(e.socket);
                if (!(e.events & kEpollIn) || it == g_sockets.end() || it->second.inbox.empty()) continue;
                if (events) events[n] = {kEpollIn, 0, static_cast<uint64_t>(e.socket), e.data};
                ++n;
            }
        }
        if (n > 0 || std::chrono::steady_clock::now() >= deadline) return n;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

uint16_t PS4_SYSV_ABI sceNetHtons(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

int PS4_SYSV_ABI sceNetInetPton(int af, const char* src, void* dst) {
    unsigned a, b, c, d;
    if (af != kAfInet || !src || !dst || std::sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    const uint8_t bytes[4] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c),
                              static_cast<uint8_t>(d)};
    std::memcpy(dst, bytes, 4);
    return 1;
}

const char* PS4_SYSV_ABI sceNetInetNtop(int af, const void* src, char* dst, uint32_t size) {
    if (af != kAfInet || !src || !dst) return nullptr;
    const auto* b = static_cast<const uint8_t*>(src);
    std::snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}

int PS4_SYSV_ABI sceNetResolverCreate(const char*, int, int) { return g_nextId++; }
int PS4_SYSV_ABI sceNetResolverDestroy(int) { return kernel::SCE_OK; }
int PS4_SYSV_ABI sceNetResolverStartNtoa(int, const char* host, void*, int, int, int) {
    std::fprintf(stderr, "[net] resolve %s\n", host ? host : "?");
    UnreachableDelay("sceNetResolverStartNtoa");
    return SceFail(kETimedOut);
}

}  // namespace hle::net

namespace hle {

void RegisterNetOffline(core::Linker& linker) {
    using namespace net;
    const char* posix = "libScePosix";
    HLE_REG(linker, posix, socket);
    HLE_REG(linker, posix, bind);
    HLE_REG(linker, posix, recv);
    HLE_REG(linker, posix, recvfrom);
    HLE_REG(linker, posix, sendto);
    const char* lib = "libSceNet";
    HLE_REG(linker, lib, sceNetInit);
    HLE_REG(linker, lib, sceNetTerm);
    HLE_REG(linker, lib, sceNetErrnoLoc);
    HLE_REG(linker, lib, sceNetPoolCreate);
    HLE_REG(linker, lib, sceNetPoolDestroy);
    HLE_REG(linker, lib, sceNetSocket);
    HLE_REG(linker, lib, sceNetSocketClose);
    HLE_REG(linker, lib, sceNetBind);
    HLE_REG(linker, lib, sceNetListen);
    HLE_REG(linker, lib, sceNetSetsockopt);
    HLE_REG(linker, lib, sceNetConnect);
    HLE_REG(linker, lib, sceNetAccept);
    HLE_REG(linker, lib, sceNetRecv);
    HLE_REG(linker, lib, sceNetSend);
    HLE_REG(linker, lib, sceNetEpollCreate);
    HLE_REG(linker, lib, sceNetEpollDestroy);
    HLE_REG(linker, lib, sceNetEpollControl);
    HLE_REG(linker, lib, sceNetEpollWait);
    HLE_REG(linker, lib, sceNetHtons);
    HLE_REG(linker, lib, sceNetInetPton);
    HLE_REG(linker, lib, sceNetInetNtop);
    HLE_REG(linker, lib, sceNetResolverCreate);
    HLE_REG(linker, lib, sceNetResolverDestroy);
    HLE_REG(linker, lib, sceNetResolverStartNtoa);
}

}  // namespace hle
