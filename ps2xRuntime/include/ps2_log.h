#ifndef PS2_LOG_H
#define PS2_LOG_H

#include <algorithm>
#include <deque>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <unordered_map>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PS2_RUNTIME_LOGS
#define PS2_RUNTIME_LOGS 0
#endif

#ifndef AGRESSIVE_LOGS
#define AGRESSIVE_LOGS 0
#endif

#define RUNTIME_ERROR(x)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        std::ostringstream _ps2_runtime_error_stream;                                                                  \
        _ps2_runtime_error_stream << x;                                                                                \
        const std::string _ps2_runtime_error_text =                                                                    \
            _ps2_runtime_error_stream.str();                                                                           \
                                                                                                                       \
        std::cerr << _ps2_runtime_error_text;                                                                           \
        ps2_log::append_runtime_log_text(_ps2_runtime_error_text);                                                      \
    } while (0)

namespace ps2_log
{
struct RuntimeLogEntry
{
    uint64_t seq = 0;
    std::string text;
};

inline constexpr size_t kMaxRuntimeLogEntries = 4096;

inline std::mutex &runtime_log_mutex()
{
    static std::mutex m;
    return m;
}

inline std::deque<RuntimeLogEntry> &runtime_log_entries()
{
    static std::deque<RuntimeLogEntry> entries;
    return entries;
}

inline uint64_t &runtime_log_next_seq()
{
    static uint64_t seq = 1;
    return seq;
}

inline bool &runtime_log_paused()
{
    static bool paused = false;
    return paused;
}

inline void set_runtime_log_paused(bool paused)
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_paused() = paused;
}

inline bool is_runtime_log_paused()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    return runtime_log_paused();
}

inline void append_runtime_log_text(const std::string &text)
{
    if (text.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    if (runtime_log_paused())
    {
        return;
    }

    auto &entries = runtime_log_entries();
    RuntimeLogEntry entry{};
    entry.seq = runtime_log_next_seq()++;
    entry.text = text;
    entries.push_back(std::move(entry));

    while (entries.size() > kMaxRuntimeLogEntries)
    {
        entries.pop_front();
    }
}

inline std::vector<RuntimeLogEntry> snapshot_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    const auto &entries = runtime_log_entries();
    return std::vector<RuntimeLogEntry>(entries.begin(), entries.end());
}

inline void clear_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_entries().clear();
}
}

#if PS2_RUNTIME_LOGS || AGRESSIVE_LOGS
#define RUNTIME_LOG(x)                                                                                                  \
    do                                                                                                                  \
    {                                                                                                                   \
        std::ostringstream _ps2_runtime_log_stream;                                                                     \
        _ps2_runtime_log_stream << x;                                                                                   \
        const std::string _ps2_runtime_log_text = _ps2_runtime_log_stream.str();                                        \
        if (_ps2_runtime_log_text.empty())                                                                            \
        {                                                                                                               \
            std::cout.flush();                                                                                          \
        }                                                                                                               \
        else                                                                                                            \
        {                                                                                                               \
            std::cout << _ps2_runtime_log_text;                                                                         \
            ps2_log::append_runtime_log_text(_ps2_runtime_log_text);                                                    \
        }                                                                                                               \
    } while (0)
#else
#define RUNTIME_LOG(x) do {} while(0)
#endif

#if AGRESSIVE_LOGS

namespace ps2_log
{
inline std::string log_path()
{
    static std::string path;
    if (path.empty())
    {
        path = (std::filesystem::current_path() / "ps2_log.txt").string();
    }
    return path;
}
inline std::ostream &log_stream()
{
    static std::ofstream f(log_path(), std::ios::out);
    return f.is_open() ? f : std::cerr;
}
inline int &depth()
{
    static thread_local int d = 0;
    return d;
}
// The function trace is extremely expensive (one flushed write per call); it is only produced when
// PS2X_FUNCTION_LOG is set to something other than "0".
// Read at the entry and exit of EVERY recompiled guest function (PS_LOG_ENTRY). As function-local statics each read went through the
// thread-safe-initialisation guard; as inline variables initialised once at start-up a read is a single load (it was ~35 % of the host
// time of the game's idle thread, docs/ee-thread-performance-2026-10-14.md).
inline const bool g_functionLogEnabled = []() {
    const char *v = std::getenv("PS2X_FUNCTION_LOG");
    return v != nullptr && v[0] != 0 && v[0] != '0';
}();
inline const bool g_functionProfileEnabled = []() {
    const char *v = std::getenv("PS2X_FUNC_PROFILE");
    return v != nullptr && v[0] != 0 && v[0] != '0';
}();
inline bool function_log_enabled()
{
    return g_functionLogEnabled;
}
// PS2X_FUNC_PROFILE=1: exclusive/inclusive host time per recompiled guest function (uses the entry/exit guards that
// every generated function already has). Printed every ~10 s. Diagnostic only; costs ~50 ns per call.
inline bool function_profile_enabled()
{
    return g_functionProfileEnabled;
}
struct ProfileFrame
{
    const char *name;
    uint64_t start;
    uint64_t child;
};
struct ProfileStat
{
    uint64_t calls = 0;
    uint64_t incl = 0;
    uint64_t excl = 0;
};
struct ProfileState
{
    std::vector<ProfileFrame> stack;
    std::unordered_map<const char *, ProfileStat> stats;
    uint64_t counter = 0;
    uint64_t tsc0 = 0;
    std::chrono::steady_clock::time_point t0;
    std::chrono::steady_clock::time_point lastPrint;
};
inline ProfileState &profile_state()
{
    static ProfileState state;
    return state;
}
inline uint64_t profile_now()
{
#if defined(_MSC_VER)
    return static_cast<uint64_t>(__rdtsc());
#else
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
inline void profile_print(ProfileState &st)
{
    const auto now = std::chrono::steady_clock::now();
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - st.t0).count());
    const double ticks = static_cast<double>(profile_now() - st.tsc0);
    const double nsPerTick = ticks > 0.0 ? ns / ticks : 1.0;
    std::vector<std::pair<const char *, ProfileStat>> rows(st.stats.begin(), st.stats.end());
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.excl > b.second.excl; });
    double total = 0.0;
    for (const auto &r : rows)
        total += static_cast<double>(r.second.excl) * nsPerTick;
    std::cerr << "[funcprof] guest-function time " << (total / 1e6) << " ms over " << (ns / 1e6) << " ms wall" << std::endl;
    // PS2X_FUNC_WATCH="0x138538,0x138e50": also print the cumulative call count of every function whose name contains one of
    // these tokens, however cold it is (diagnostic only).
    if (const char *watch = std::getenv("PS2X_FUNC_WATCH"))
    {
        const std::string spec(watch);
        for (const auto &r : rows)
        {
            size_t at = 0;
            while (at < spec.size())
            {
                size_t end = spec.find(',', at);
                if (end == std::string::npos)
                    end = spec.size();
                const std::string token = spec.substr(at, end - at);
                at = end + 1;
                if (!token.empty() && std::string(r.first).find(token) != std::string::npos)
                {
                    std::cerr << "[funcwatch] " << r.first << " calls=" << r.second.calls << std::endl;
                    break;
                }
            }
        }
    }
    for (size_t i = 0; i < rows.size() && i < 45; ++i)
        std::cerr << "[funcprof] " << rows[i].first << " excl=" << (static_cast<double>(rows[i].second.excl) * nsPerTick / 1e6) << "ms incl="
                  << (static_cast<double>(rows[i].second.incl) * nsPerTick / 1e6) << "ms calls=" << rows[i].second.calls << std::endl;
}
inline void profile_enter(const char *name)
{
    ProfileState &st = profile_state();
    if (st.stack.empty() && st.stats.empty())
    {
        st.tsc0 = profile_now();
        st.t0 = st.lastPrint = std::chrono::steady_clock::now();
    }
    st.stack.push_back({name, profile_now(), 0});
}
inline void profile_exit(const char *name)
{
    ProfileState &st = profile_state();
    if (st.stack.empty())
        return;
    const ProfileFrame f = st.stack.back();
    st.stack.pop_back();
    const uint64_t dur = profile_now() - f.start;
    ProfileStat &stat = st.stats[f.name];
    ++stat.calls;
    stat.incl += dur;
    stat.excl += dur > f.child ? dur - f.child : 0;
    if (!st.stack.empty())
        st.stack.back().child += dur;
    (void)name;
    if ((++st.counter & 0xFFFFu) == 0u)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - st.lastPrint > std::chrono::seconds(10))
        {
            st.lastPrint = now;
            profile_print(st);
        }
    }
}
inline void log_entry(const char *name)
{
    if (function_profile_enabled())
        profile_enter(name);
    if (!function_log_enabled())
        return;
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << ">> " << name << " enter\n";
    log_stream().flush();
    depth()++;
}
inline void log_exit(const char *name)
{
    if (function_profile_enabled())
        profile_exit(name);
    if (!function_log_enabled())
        return;
    depth()--;
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << "<< " << name << " exit\n";
    log_stream().flush();
}
inline void print_saved_location()
{
    std::cout << "[PS2 LOG] Logs saved at " << log_path() << std::endl;
}
}

#define PS_LOG_ENTRY(name) \
    ps2_log::log_entry(name); \
    struct _ps2_log_guard_ { const char *_n; _ps2_log_guard_(const char *n) : _n(n) {} \
        ~_ps2_log_guard_() { ps2_log::log_exit(_n); } } _ps2_log_guard_(name)
#define PS2_IF_AGRESSIVE_LOGS(code) \
    do                              \
    {                               \
        code;                       \
    } while (0)

#else

namespace ps2_log
{
inline std::string log_path()
{
    return (std::filesystem::current_path() / "ps2_log.txt").string();
}
inline void print_saved_location() {}
}
#define PS_LOG_ENTRY(name) ((void)0)
#define PS2_IF_AGRESSIVE_LOGS(code) ((void)0)

#endif

#endif
