#ifndef PS2_CALL_TRACE_H
#define PS2_CALL_TRACE_H

class PS2Runtime;

namespace ps2_call_trace
{
    // PS2X_TRACE_CALLS="0xADDR,0xADDR,..." (+ PS2X_TRACE_MAX=n): wraps those recompiled functions with a logging trampoline.
    void installFromEnvironment(PS2Runtime &runtime);
}

#endif // PS2_CALL_TRACE_H
