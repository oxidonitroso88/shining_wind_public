#ifndef PS2_SCRIPT_TRACE_H
#define PS2_SCRIPT_TRACE_H

class PS2Runtime;

namespace ps2_script_trace
{
    // Diagnostic (off by default): traces the calls the game's script VM makes to its natives (table of function pointers at 0x3CFD90, interpreter at 0x1E98D0).
    // Each native receives the VM context in $a0 (+0x08 code base, +0x14 string pool base, +0x1C program offset) and reads its own operands from the bytecode
    // stream, so a call is logged as: native index, address, the operand words it consumed and, for operands that point into the string pool, the string.
    //   PS2X_SCRIPT_TRACE=<file>            output (stderr when "1")
    //   PS2X_SCRIPT_TRACE_VB=<from>-<to>    only inside this VBlank window (default: all)
    //   PS2X_SCRIPT_TRACE_MAX=<n>           stop after n lines (default 300000)
    void installFromEnvironment(PS2Runtime &runtime);
}

#endif // PS2_SCRIPT_TRACE_H
