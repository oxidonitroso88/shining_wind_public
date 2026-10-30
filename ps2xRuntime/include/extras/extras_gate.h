#ifndef PC_EXTRAS_GATE_H
#define PC_EXTRAS_GATE_H

class PS2Runtime;

// PC Extras - what the game must not see while Extras is in front. The gate has independent reasons so that closing the menu never lifts the
// block of a running event session.
//   * the game's pad input is replaced by a neutral, still "connected" pad (padinput::PadRouter::setGameInputBlocked),
//   * the game's audio is not sent to the device (ps2x::iop::setAudioMuted),
//   * the emulation stops at the next VBlank (EeScheduler::setUserPause) and resumes with its timers re-based, so nothing advances or catches up.
namespace pc_extras::gate
{
    enum Reason : unsigned { Menu = 1u, Session = 2u };

    void bind(PS2Runtime *runtime); // the runtime whose scheduler is paused (nullptr: stand-alone Extras, nothing to pause)
    void set(Reason reason, bool on);
    bool active();                  // any reason set
    bool has(Reason reason);
}

#endif // PC_EXTRAS_GATE_H
