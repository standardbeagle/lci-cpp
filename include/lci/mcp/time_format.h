#pragma once

#include <lci/core/portable.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace lci {
namespace mcp {

/// Formats a system_clock time_point as RFC3339Nano with local timezone offset
/// (matches Go's time.Time JSON marshal: "2026-05-13T17:12:53.528955893-05:00").
/// Zero-alloc on hot path: writes into a fixed 48-byte stack buffer then a
/// single std::string move out.
inline std::string format_rfc3339_nano_local(
    std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto secs = time_point_cast<seconds>(tp);
    auto ns = duration_cast<nanoseconds>(tp - secs).count();
    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    portable::localtime_local(t, tm);

    // Compute local zone offset from UTC.
    std::tm utm{};
    portable::gmtime_utc(t, utm);
    // Difference in seconds: treat both tms as if UTC for difftime.
    std::time_t lt = std::mktime(&tm);
    std::time_t ut = std::mktime(&utm);
    long offset = static_cast<long>(lt - ut);
    char sign = offset < 0 ? '-' : '+';
    long abs_off = offset < 0 ? -offset : offset;
    int oh = static_cast<int>(abs_off / 3600);
    int om = static_cast<int>((abs_off % 3600) / 60);

    char buf[48];
    int n = std::snprintf(buf, sizeof(buf),
                          "%04d-%02d-%02dT%02d:%02d:%02d.%09ld%c%02d:%02d",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<long>(ns), sign, oh, om);
    if (n <= 0) return std::string{};
    return std::string(buf, static_cast<size_t>(n));
}

}  // namespace mcp
}  // namespace lci
