// PS4 code uses the System V calling convention; everything the game calls
// directly must be compiled with it. Requires clang (MSVC cannot do this).
#pragma once

#if defined(__clang__)
#define PS4_SYSV_ABI __attribute__((sysv_abi))
#else
#define PS4_SYSV_ABI
#endif
