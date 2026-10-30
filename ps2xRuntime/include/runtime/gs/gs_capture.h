#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

// Diagnostic/benchmark aid: PS2X_GS_CAPTURE="<file>;<startSeconds>;<presents>" wraps the raster backend and records, from
// <startSeconds> of wall time on, every state-changing backend call (primitives, CLUT loads, transfers, uploads, clears,
// direct VRAM writes) in call order together with a VRAM snapshot taken when the capture starts, until <presents> frames have
// been presented. The file is replayed offline by the ps2_gs_replay tool (timing + bit-exact VRAM hash), which makes
// rasteriser changes measurable and verifiable on real game workloads. Captured files contain game data: keep them local.
//
// A "GSCAP1" file is: magic[6], u32 vramSize, vram bytes, then records (u8 type + payload):
enum class GSCaptureRecord : uint8_t
{
    Submit = 1,        // raw GSPrimitiveBatch
    Clut = 2,          // GSTex0Reg + GSTexClutReg
    Transfer = 3,      // raw GSTransferCommand
    Upload = 4,        // u32 size + bytes
    TextureFlush = 5,
    Clear = 6,         // GSContext + u32 rgba
    WriteVram = 7,     // 6 x u32 (psm, base, bw, x, y, value)
    Present = 8,       // frame marker
    ConsumeLocal = 9,  // u32 maxBytes
    Reset = 10,
};

// Returns `inner` unchanged unless PS2X_GS_CAPTURE is set.
std::unique_ptr<GSRasterBackend> gsMaybeWrapWithCapture(std::unique_ptr<GSRasterBackend> inner);
