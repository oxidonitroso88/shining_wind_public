#ifndef PS2_SESSION_POLICY_H
#define PS2_SESSION_POLICY_H

#include <atomic>

// Process-wide "nothing persistent" policy for throw-away sessions (PC Extras Soul Tree viewer). When set, every write to the guest's
// memory card and to any host file reached through the guest's file system is refused at the lowest level (the guest sees an ordinary
// I/O error), and the runtime does not save its own configuration. It is set once at start-up and never cleared.
namespace ps2x::session
{
    inline std::atomic<bool> g_readOnly{false};

    inline void setReadOnly() { g_readOnly.store(true, std::memory_order_relaxed); }
    inline bool readOnly() { return g_readOnly.load(std::memory_order_relaxed); }
}

#endif // PS2_SESSION_POLICY_H
