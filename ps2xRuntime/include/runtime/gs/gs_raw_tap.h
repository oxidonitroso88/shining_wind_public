#pragma once

#include <cstdint>

// Optional tap on the exact commands the GS frontend is fed (GIF packets, direct register writes, native image uploads).
// An alternative rasteriser that consumes raw GS commands (parallel-gs) registers itself here; the frontend keeps parsing as
// usual (debug history, display-source heuristics) and the software rasteriser stays the default. Not included by
// ps2_runtime.h on purpose: adding it must not force a rebuild of the generated runner.
class GSRawTap
{
public:
    virtual ~GSRawTap() = default;
    // One or more complete GIF packets (tags + payload), as handed to GS::processGIFPacket.
    virtual void OnGifPackets(const uint8_t *data, uint32_t sizeBytes) = 0;
    // Direct register write (GS::writeRegister): addr is the GS register address (0x00..0x7f).
    virtual void OnRegister(uint8_t addr, uint64_t value) = 0;
    // Host->local image upload expressed as BITBLTBUF/TRXPOS/TRXREG/TRXDIR + raw image bytes (GS::uploadImageNative).
    virtual void OnImageUpload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir, const uint8_t *data, uint32_t sizeBytes) = 0;
};

GSRawTap *gsRawTap() noexcept;
void gsSetRawTap(GSRawTap *tap) noexcept;
