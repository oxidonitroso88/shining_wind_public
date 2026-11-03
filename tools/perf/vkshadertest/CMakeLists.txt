cmake_minimum_required(VERSION 3.16)
project(vkshadertest CXX)
# Vulkan headers only (the loader is opened with LoadLibrary): point VULKAN_HEADERS_DIR to ".../Granite/third_party/khronos/vulkan-headers/include"
# (it is inside the parallel-gs checkout) or to the include folder of a Vulkan SDK.
set(VULKAN_HEADERS_DIR "" CACHE PATH "Folder that contains vulkan/vulkan.h")
# static CRT: the .exe runs on a PC without the Visual C++ runtime (it only needs vulkan-1.dll, i.e. the GPU driver)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_executable(vkshadertest vkshadertest.cpp)
target_include_directories(vkshadertest PRIVATE "${VULKAN_HEADERS_DIR}")

# vkshaderrun: creates the compute pipeline of sampler_feedback.comp and runs it on synthetic input (differential test between SPIR-V builds / GPUs)
add_executable(vkshaderrun vkshaderrun.cpp)
target_include_directories(vkshaderrun PRIVATE "${VULKAN_HEADERS_DIR}")
