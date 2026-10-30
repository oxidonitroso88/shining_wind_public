#pragma once

#include "runtime/gs/gs_backend.h"

#include <memory>

// Experimental backend (PS2X_GS_PARALLEL=ON at configure time, PS2X_GS_BACKEND=PARALLEL_GS at run time): parallel-gs (Vulkan,
// LGPLv3+) rasterises the raw GS commands received through the GSRawTap; a non-drawing GSCpuBackend works on a host mirror of
// the VRAM for presentation, host reads and clears. Returns nullptr (after logging why) when Vulkan or the GPU is unusable.
std::unique_ptr<GSRasterBackend> CreateParallelGSBackend();
