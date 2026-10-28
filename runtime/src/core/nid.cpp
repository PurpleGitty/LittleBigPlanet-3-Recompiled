#include "core/nid.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstring>

namespace core {
namespace {

constexpr char kEnc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
constexpr uint8_t kSuffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};

}  // namespace

std::string Nid(std::string_view name) {
    std::string buf(name);
    buf.append(reinterpret_cast<const char*>(kSuffix), sizeof(kSuffix));
    uint8_t hash[20];
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(buf.data()),
               static_cast<ULONG>(buf.size()), hash, sizeof(hash));
    uint64_t v;
    std::memcpy(&v, hash, sizeof(v));
    // 64 bits -> 10 six-bit chars plus a final char holding the low 4 bits
    std::string out;
    for (int i = 0; i < 10; ++i) out += kEnc[(v >> (58 - 6 * i)) & 63];
    out += kEnc[(v & 0xF) << 2];
    return out;
}

uint16_t DecodeId(std::string_view encoded) {
    uint32_t v = 0;
    for (char c : encoded) v = (v << 6) | static_cast<uint32_t>(std::strchr(kEnc, c) - kEnc);
    return static_cast<uint16_t>(v);
}

}  // namespace core
