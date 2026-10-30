#include "MiniTest.h"
#include <cstdlib>
#include <iostream>
void reset_ps2_test_function_table();
void register_ps2_sif_dma_tests();
void register_ps2_sif_rpc_tests();
void register_sif_packet_runtime_tests();
void register_ee_scheduler_irq_tests();
int main()
{
    // Unit tests drive the software rasteriser directly; do not create a Vulkan device for every GS instance.
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "CPU");
#else
    setenv("PS2X_GS_BACKEND", "CPU", 1);
#endif
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_ps2_sif_dma_tests();
    register_ps2_sif_rpc_tests();
    register_sif_packet_runtime_tests();
    register_ee_scheduler_irq_tests();
    const int result = MiniTest::Run();
    std::cout.flush(); std::cerr.flush();
    std::_Exit(result);
}
