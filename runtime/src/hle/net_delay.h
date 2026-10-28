// How long an unreachable server takes to fail. On a console the game's online
// services fail after DNS/connect timeouts, not instantly, and LBP3's connect
// flow depends on that: it records the player before the failure arrives and
// only reports "can't connect" once the failure changes it. Failing instantly
// leaves it waiting forever. LBP3_NET_FAIL_MS overrides the default.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace hle::net {

inline void UnreachableDelay(const char* what) {
    static const int ms = [] {
        const char* v = std::getenv("LBP3_NET_FAIL_MS");
        return v ? std::atoi(v) : 3000;
    }();
    static std::atomic<int> logged{0};
    if (logged++ < 20) std::fprintf(stderr, "[net] %s: server unreachable (after %d ms)\n", what, ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace hle::net
