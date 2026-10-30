#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_pad_two_players_tests();
void register_ps2_fpu_tests();
void register_ps2_mmi_mac_tests();
void register_ps2_audio_output_tests();
void register_ps2_overlay_tests();
void register_ps2_host_arena_tests();
void register_ps2_soul_session_tests();
void register_ps2_extras_tests();
void register_ps2_hd_pack_tests();
void register_ps2_gs_wide_frame_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void reset_ps2_test_function_table();

int main()
{
    // Unit tests drive the software rasteriser directly; do not create a Vulkan device for every GS instance.
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "CPU");
#else
    setenv("PS2X_GS_BACKEND", "CPU", 1);
#endif
    // The GS tests read VRAM right after drawing; the runtime defaults to the asynchronous rasteriser thread, so run them synchronously.
#ifdef _WIN32
    _putenv_s("PS2X_GS_SYNC", "1");
#else
    setenv("PS2X_GS_SYNC", "1", 1);
#endif
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_pad_input_tests();
    register_pad_two_players_tests();
    register_ps2_fpu_tests();
    register_ps2_mmi_mac_tests();
    register_ps2_audio_output_tests();
    register_ps2_overlay_tests();
    register_ps2_host_arena_tests();
    register_ps2_soul_session_tests();
    register_ps2_extras_tests();
    register_ps2_hd_pack_tests();
    register_ps2_gs_wide_frame_tests();
    register_ps2_runtime_io_tests();
    register_ps2_runtime_kernel_tests();
    register_ps2_runtime_interrupt_tests();
    register_ps2_memory_tests();
    register_ps2_vu1_tests();
    register_ps2_vu_tests();
    register_ps2_gs_tests();
    register_ps2_iop_tests();
    register_ps2_sif_rpc_tests();
    register_ps2_sif_dma_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
