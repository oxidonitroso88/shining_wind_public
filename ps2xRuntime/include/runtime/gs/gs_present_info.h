#pragma once

// Side channel from the GS presentation backend to the window code: the *logical* size of the picture that was just presented, when it differs from the
// pixel size of the frame. A GPU scanout at a higher internal resolution (PS2X_GS_SCALE) produces e.g. 1280x448 pixels for what is a 640x448 picture
// (a field-rendered game is doubled vertically on screen), so the window must keep the picture's aspect ratio, not the pixel grid's.
// (A separate header on purpose: gs_frontend.h / gs_types.h are included by the generated runner and changing them forces a rebuild of all of it.)

#include <atomic>
#include <cstdint>

namespace gs_present_info
{
    inline std::atomic<uint32_t> g_logicalWidth{0};
    inline std::atomic<uint32_t> g_logicalHeight{0};

    // 0, 0 = same as the frame
    inline void setLogicalSize(uint32_t width, uint32_t height)
    {
        g_logicalWidth.store(width, std::memory_order_relaxed);
        g_logicalHeight.store(height, std::memory_order_relaxed);
    }

    inline void logicalSize(uint32_t &width, uint32_t &height)
    {
        width = g_logicalWidth.load(std::memory_order_relaxed);
        height = g_logicalHeight.load(std::memory_order_relaxed);
    }

    // Window -> backend: size in pixels the picture occupies on screen (after the aspect-preserving fit), published every frame by the window loop.
    // The GPU presentation may shrink the frame to it before the read-back (PS2X_GS_GPU_DOWNSCALE=1). 0, 0 = unknown.
    inline std::atomic<uint32_t> g_targetWidth{0};
    inline std::atomic<uint32_t> g_targetHeight{0};

    inline void setTargetSize(uint32_t width, uint32_t height)
    {
        g_targetWidth.store(width, std::memory_order_relaxed);
        g_targetHeight.store(height, std::memory_order_relaxed);
    }

    inline void targetSize(uint32_t &width, uint32_t &height)
    {
        width = g_targetWidth.load(std::memory_order_relaxed);
        height = g_targetHeight.load(std::memory_order_relaxed);
    }
}
