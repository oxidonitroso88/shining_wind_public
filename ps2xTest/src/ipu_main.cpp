#include "MiniTest.h"
#include <cstdlib>
#include <iostream>

void register_ps2_ipu_tests();

int main()
{
    // Unit tests drive the software rasteriser directly; do not create a Vulkan device for every GS instance.
#ifdef _WIN32
    _putenv_s("PS2X_GS_BACKEND", "CPU");
#else
    setenv("PS2X_GS_BACKEND", "CPU", 1);
#endif
    register_ps2_ipu_tests();
    const int result = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(result);
}
