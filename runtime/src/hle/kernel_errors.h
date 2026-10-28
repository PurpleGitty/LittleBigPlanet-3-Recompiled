// PS4 error codes. sce* functions return 0x80020000 + errno; POSIX-named
// functions return -1 and set errno (FreeBSD numbering).
#pragma once
#include <cstdint>

#include "hle/abi.h"

namespace hle::kernel {

enum Errno : int {
    kEPERM = 1, kENOENT = 2, kESRCH = 3, kEINTR = 4, kEIO = 5, kEBADF = 9,
    kEDEADLK = 11, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEBUSY = 16,
    kEEXIST = 17, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24,
    kENOSPC = 28, kEAGAIN = 35, kETIMEDOUT = 60, kENAMETOOLONG = 63,
    kENOTEMPTY = 66, kENOSYS = 78, kECANCELED = 85,
};

constexpr int SCE_OK = 0;
constexpr int SceErr(int e) { return static_cast<int>(0x80020000u + static_cast<uint32_t>(e)); }
// sce error -> errno, for POSIX wrappers around sce implementations.
constexpr int ToErrno(int sce) { return sce == SCE_OK ? 0 : static_cast<int>(static_cast<uint32_t>(sce) - 0x80020000u); }

int* PS4_SYSV_ABI __error();
// POSIX return convention: 0 on success, otherwise -1 with errno set.
inline int PosixResult(int sce) {
    if (sce == SCE_OK) return 0;
    *__error() = ToErrno(sce);
    return -1;
}

}  // namespace hle::kernel
