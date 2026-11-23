/*
 * SPDX-License-Identifier: MIT
 */

#include "platform/clock_sync.h"

#include "logger/logger.h"
#include "platform/process.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

namespace platform::clock_sync {

bool should_set(long long now, long long offered, bool trusted) {
    if (offered < kSaneEpoch || offered > 4102444800LL) return false;  // not a believable time
    if (std::llabs(offered - now) <= kSlackS) return false;
    return trusted || now < kSaneEpoch;
}

long long parse_http_date(const std::string& text) {
    // RFC 7231 IMF-fixdate: "Sun, 22 Nov 2026 02:19:22 GMT". Parsed by hand: strptime's
    // month names follow the locale.
    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const size_t comma = text.find(',');
    if (comma == std::string::npos) return 0;
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char month[4] = {};
    if (std::sscanf(text.c_str() + comma + 1, " %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6) {
        return 0;
    }
    const char* at = std::strstr(kMonths, month);
    if (!at || (at - kMonths) % 3 != 0 || year < 2000 || day < 1 || day > 31) return 0;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = static_cast<int>((at - kMonths) / 3);
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return static_cast<long long>(timegm(&tm));
}

bool offer(const std::string& command, long long offered, bool trusted, const char* source) {
    if (command.empty() || offered <= 0) return false;
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!should_set(now, offered, trusted)) return false;
    static std::atomic<long long> last_try_ms{0};
    const long long mono = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    long long last = last_try_ms.load();
    if (last != 0 && mono - last < 60000) return false;
    if (!last_try_ms.compare_exchange_strong(last, mono)) return false;
    LOG_INFO("clock: {} says it is {} (PET says {}): setting the clock", source, offered, now);
    auto argv = proc::split_command(command);
    argv.push_back(std::to_string(offered));
    std::thread([argv]() {
        std::string out;
        const int rc = proc::run_capture(argv, out, 10000);
        if (rc != 0) LOG_INFO("clock: set failed ({}): {}", rc, out);
    }).detach();
    return true;
}

} // namespace platform::clock_sync
