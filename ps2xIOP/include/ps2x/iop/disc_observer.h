#pragma once

// Observer of the virtual disc reads served to the guest (diagnostics for the texture provenance tracking, PS2X_TEXMAP).
// Every non-blocking CDVDFSV read reports which file(s) the sectors belong to and where the data landed in the EE RAM, so the EE side can follow a
// resource from the disc through decompression/copies to the texture the GS samples. Off (no observer) by default: nothing is computed.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::iop
{
    struct DiscSpan
    {
        std::string path;        // "ST/TITLE.RAX", "ST/CHARA/PLAYER/006A.BRA", "DISC/OVERLAY/OVL09.BIN" (upper case, no ";1")
        uint64_t fileOffset = 0; // offset of the first byte of this span inside the file
        uint32_t bytes = 0;      // length of the span
        uint32_t bufferOffset = 0; // offset of the span inside the destination buffer of the read
    };

    struct DiscRead
    {
        uint32_t lsn = 0;
        uint32_t sectors = 0;
        uint32_t destination = 0; // EE address the data was written to
        std::vector<DiscSpan> spans;
    };

    using DiscReadObserver = std::function<void(const DiscRead &)>;

    // Process-wide; set once before the guest runs. Passing an empty function removes it.
    void setDiscReadObserver(DiscReadObserver observer);
    bool hasDiscReadObserver();
}
