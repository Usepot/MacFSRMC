cmake_minimum_required(VERSION 3.20)
project(macfsrmc_fsr2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(JNI REQUIRED)

set(FSR2_API "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr2/api")
add_library(macfsrmc_fsr2 SHARED
    src/fsr2_bridge.cpp
    src/macfsr_vulkan_shim.cpp
    "${FSR2_API}/ffx_fsr2.cpp"
    "${FSR2_API}/ffx_assert.cpp"
    "${FSR2_API}/vk/ffx_fsr2_vk.cpp"
    "${FSR2_API}/vk/shaders/ffx_fsr2_shaders_vk.cpp"
)

target_compile_definitions(macfsrmc_fsr2 PRIVATE FFX_GCC=1)
target_compile_options(macfsrmc_fsr2 PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/src/macfsr_portability.h"
    -Wall
    -Wextra
    -Wno-unused-parameter
)
target_include_directories(macfsrmc_fsr2 PRIVATE
    src
    generated
    "${FSR2_API}"
    "${FSR2_API}/vk"
    third_party/fsr2/generated/vk
    third_party/vulkan/include
    ${JNI_INCLUDE_DIRS}
)

if(APPLE)
    target_link_options(macfsrmc_fsr2 PRIVATE "-Wl,-undefined,error")
elseif(UNIX)
    target_link_libraries(macfsrmc_fsr2 PRIVATE dl)
endif()

set_target_properties(macfsrmc_fsr2 PROPERTIES
    OUTPUT_NAME "macfsrmc_fsr2"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

