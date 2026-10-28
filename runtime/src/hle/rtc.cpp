// libSceRtc: wall-clock time. A "tick" is microseconds since 0001-01-01 UTC.
// The network clock is just the PC's clock, like shadPS4 does.
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hle/kernel_errors.h"
#include "hle/register.h"

namespace hle::rtc {
namespace {

constexpr int kErrorInvalidPointer = static_cast<int>(0x80B50002);
constexpr int kErrorInvalidYear = static_cast<int>(0x80010621);
constexpr int kErrorInvalidMonth = static_cast<int>(0x80010622);
constexpr int kErrorInvalidDay = static_cast<int>(0x80010623);
constexpr int kErrorInvalidHour = static_cast<int>(0x80010624);
constexpr int kErrorInvalidMinute = static_cast<int>(0x80010625);
constexpr int kErrorInvalidSecond = static_cast<int>(0x80010626);
constexpr int kErrorInvalidMicrosecond = static_cast<int>(0x80010627);

constexpr int64_t kUnixEpochTicks = 0xdcbffeff2bc000;  // 1970-01-01 in ticks
constexpr int64_t kTicksPerSecond = 1'000'000;
constexpr int64_t kTicksPerDay = 86'400 * kTicksPerSecond;
// Days from 0001-01-01 to 1970-01-01 in the proleptic Gregorian calendar.
constexpr int64_t kDaysToUnixEpoch = 719'162;

// Howard Hinnant's civil-date algorithms (days relative to 1970-01-01).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int DaysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && IsLeap(y) ? 29 : kDays[m - 1];
}

int64_t NowTicks() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t unix100ns = (static_cast<int64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    return kUnixEpochTicks + unix100ns / 10;
}

int LocalOffsetMinutes() {
    TIME_ZONE_INFORMATION tz;
    const DWORD r = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : 0);
    return -static_cast<int>(bias);  // minutes east of UTC
}

}  // namespace

struct Tick { uint64_t tick; };
struct DateTime { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; };

int PS4_SYSV_ABI sceRtcCheckValid(const DateTime* t) {
    if (!t) return kErrorInvalidPointer;
    if (t->year < 1 || t->year > 9999) return kErrorInvalidYear;
    if (t->month < 1 || t->month > 12) return kErrorInvalidMonth;
    if (t->day < 1 || t->day > DaysInMonth(t->year, t->month)) return kErrorInvalidDay;
    if (t->hour > 23) return kErrorInvalidHour;
    if (t->minute > 59) return kErrorInvalidMinute;
    if (t->second > 59) return kErrorInvalidSecond;
    if (t->microsecond > 999'999) return kErrorInvalidMicrosecond;
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceRtcGetTick(const DateTime* t, Tick* tick) {
    if (!t || !tick) return kErrorInvalidPointer;
    if (int r = sceRtcCheckValid(t)) return r;
    const int64_t days = DaysFromCivil(t->year, t->month, t->day) + kDaysToUnixEpoch;
    tick->tick = static_cast<uint64_t>(days * kTicksPerDay +
                                       ((t->hour * 60 + t->minute) * 60 + t->second) * kTicksPerSecond +
                                       t->microsecond);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceRtcSetTick(DateTime* t, const Tick* tick) {
    if (!t || !tick) return kErrorInvalidPointer;
    const int64_t ticks = static_cast<int64_t>(tick->tick);
    const int64_t days = ticks / kTicksPerDay;
    int64_t rest = ticks % kTicksPerDay;
    int64_t y;
    unsigned m, d;
    CivilFromDays(days - kDaysToUnixEpoch, y, m, d);
    t->year = static_cast<uint16_t>(y);
    t->month = static_cast<uint16_t>(m);
    t->day = static_cast<uint16_t>(d);
    t->hour = static_cast<uint16_t>(rest / (3600 * kTicksPerSecond));
    rest %= 3600 * kTicksPerSecond;
    t->minute = static_cast<uint16_t>(rest / (60 * kTicksPerSecond));
    rest %= 60 * kTicksPerSecond;
    t->second = static_cast<uint16_t>(rest / kTicksPerSecond);
    t->microsecond = static_cast<uint32_t>(rest % kTicksPerSecond);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceRtcSetTime_t(DateTime* t, int64_t time) {
    if (!t) return kErrorInvalidPointer;
    const Tick tick{static_cast<uint64_t>(kUnixEpochTicks + time * kTicksPerSecond)};
    return sceRtcSetTick(t, &tick);
}

int PS4_SYSV_ABI sceRtcGetCurrentTick(Tick* tick) {
    if (!tick) return kErrorInvalidPointer;
    tick->tick = static_cast<uint64_t>(NowTicks());
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceRtcGetCurrentNetworkTick(Tick* tick) { return sceRtcGetCurrentTick(tick); }

int PS4_SYSV_ABI sceRtcTickAddMinutes(Tick* out, const Tick* in, int64_t minutes) {
    if (!out || !in) return kErrorInvalidPointer;
    out->tick = in->tick + static_cast<uint64_t>(minutes * 60 * kTicksPerSecond);
    return kernel::SCE_OK;
}

int PS4_SYSV_ABI sceRtcConvertUtcToLocalTime(const Tick* utc, Tick* local) {
    if (!utc || !local) return kErrorInvalidPointer;
    return sceRtcTickAddMinutes(local, utc, LocalOffsetMinutes());
}

int PS4_SYSV_ABI sceRtcGetCurrentClockLocalTime(DateTime* t) {
    if (!t) return kErrorInvalidPointer;
    Tick now{static_cast<uint64_t>(NowTicks())}, local;
    sceRtcConvertUtcToLocalTime(&now, &local);
    return sceRtcSetTick(t, &local);
}

// 0 = Sunday.
int PS4_SYSV_ABI sceRtcGetDayOfWeek(int year, int month, int day) {
    if (year < 1 || year > 9999) return kErrorInvalidYear;
    if (month < 1 || month > 12) return kErrorInvalidMonth;
    if (day < 1 || day > DaysInMonth(year, month)) return kErrorInvalidDay;
    const int64_t days = DaysFromCivil(year, month, day);  // 1970-01-01 was a Thursday
    return static_cast<int>(((days % 7) + 11) % 7);
}

}  // namespace hle::rtc

namespace hle {

void RegisterRtc(core::Linker& linker) {
    using namespace rtc;
    const char* lib = "libSceRtc";
    HLE_REG(linker, lib, sceRtcCheckValid);
    HLE_REG(linker, lib, sceRtcGetTick);
    HLE_REG(linker, lib, sceRtcSetTick);
    HLE_REG(linker, lib, sceRtcSetTime_t);
    HLE_REG(linker, lib, sceRtcGetCurrentTick);
    HLE_REG(linker, lib, sceRtcGetCurrentNetworkTick);
    HLE_REG(linker, lib, sceRtcTickAddMinutes);
    HLE_REG(linker, lib, sceRtcConvertUtcToLocalTime);
    HLE_REG(linker, lib, sceRtcGetCurrentClockLocalTime);
    HLE_REG(linker, lib, sceRtcGetDayOfWeek);
}

}  // namespace hle
