#pragma once

#include "date/tz.h"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace revlm
{

using sys_seconds = std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>;

inline sys_seconds local_date_to_utc(int y, unsigned m, unsigned d, const std::string &tz)
{
    const auto zoned = date::make_zoned(tz, date::local_days{ date::year{ y } / date::month{ m } / date::day{ d } });
    return date::floor<std::chrono::seconds>(zoned.get_sys_time());
}

inline std::string format_local(sys_seconds tp, const std::string &tz, std::string_view fmt)
{
    return date::format(std::string{ fmt }, date::make_zoned(tz, tp));
}

inline std::string hour_bucket(sys_seconds tp, const std::string &tz)
{
    return format_local(tp, tz, "%Y-%m-%dT%H:00:00");
}

inline std::string day_bucket(sys_seconds tp, const std::string &tz)
{
    return format_local(tp, tz, "%Y-%m-%d");
}

inline std::string to_mysql_datetime(sys_seconds tp)
{
    return date::format("%Y-%m-%d %H:%M:%S", tp);
}

inline std::string to_iso8601z(sys_seconds tp)
{
    return date::format("%Y-%m-%dT%H:%M:%SZ", tp);
}

inline sys_seconds parse_mysql_datetime(std::string_view raw)
{
    sys_seconds tp{};
    std::istringstream in{ std::string{ raw } };
    in >> date::parse("%Y-%m-%d %H:%M:%S", tp);
    if (in.fail()) {
        throw std::invalid_argument("invalid mysql datetime");
    }
    return tp;
}

inline sys_seconds unix_to_sys(std::time_t seconds)
{
    return sys_seconds{ std::chrono::seconds{ seconds } };
}

inline std::time_t sys_to_unix(sys_seconds tp)
{
    return static_cast<std::time_t>(tp.time_since_epoch().count());
}

inline void next_date(int &year, unsigned &month, unsigned &day)
{
    const date::year_month_day ymd =
        date::sys_days{ date::year{ year } / date::month{ month } / date::day{ day } } + date::days{ 1 };
    year = static_cast<int>(ymd.year());
    month = static_cast<unsigned>(ymd.month());
    day = static_cast<unsigned>(ymd.day());
}

inline bool zone_exists(const std::string &tz)
{
    try {
        (void)date::locate_zone(tz);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace revlm
