#include "MiniTest.h"

void register_code_generator_tests();
void register_ps2_recompiler_tests();

int main()
{
    register_code_generator_tests();
    register_ps2_recompiler_tests();
    return MiniTest::Run();
}
