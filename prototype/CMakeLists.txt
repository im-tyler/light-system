cmake_minimum_required(VERSION 3.20)

# Default build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

project(ProjectMeridianPrototype LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

# Export compile_commands.json for LSP/clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VULKAN QUIET IMPORTED_TARGET vulkan)
    pkg_check_modules(GLFW QUIET IMPORTED_TARGET glfw3)
    pkg_check_modules(SHADERC QUIET IMPORTED_TARGET shaderc)
endif()

add_library(vgeo_builder
    src/builder_import.cpp
    src/builder_cluster.cpp
    src/builder_traversal.cpp
    src/builder_validate.cpp
    src/builder_serialize.cpp
    src/replay_script.cpp
    src/vgeo_builder.cpp
    src/runtime_model.cpp
    src/streaming_scheduler.cpp
    src/async_reader.cpp
    src/resource_upload.cpp
    src/gpu_profiler.cpp
    src/vk_bootstrap.mm
    src/vk_compute_cull.cpp
    src/vk_compute_selection.cpp
    src/vk_hzb.cpp
    src/vk_occlusion.cpp
    src/vk_shadow.cpp
    src/vk_render.cpp
    thirdparty/meshoptimizer/src/allocator.cpp
    thirdparty/meshoptimizer/src/clusterizer.cpp
    thirdparty/meshoptimizer/src/indexgenerator.cpp
    thirdparty/meshoptimizer/src/indexcodec.cpp
    thirdparty/meshoptimizer/src/meshletutils.cpp
    thirdparty/meshoptimizer/src/partition.cpp
    thirdparty/meshoptimizer/src/simplifier.cpp
    thirdparty/meshoptimizer/src/spatialorder.cpp
    thirdparty/meshoptimizer/src/vertexcodec.cpp
    thirdparty/meshoptimizer/src/vertexfilter.cpp
)

target_include_directories(vgeo_builder
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/meshoptimizer/src
)

target_compile_features(vgeo_builder PUBLIC cxx_std_20)

# Suppress warnings for third-party meshoptimizer sources
set_source_files_properties(
    thirdparty/meshoptimizer/src/allocator.cpp
    thirdparty/meshoptimizer/src/clusterizer.cpp
    thirdparty/meshoptimizer/src/indexgenerator.cpp
    thirdparty/meshoptimizer/src/indexcodec.cpp
    thirdparty/meshoptimizer/src/meshletutils.cpp
    thirdparty/meshoptimizer/src/partition.cpp
    thirdparty/meshoptimizer/src/simplifier.cpp
    thirdparty/meshoptimizer/src/spatialorder.cpp
    thirdparty/meshoptimizer/src/vertexcodec.cpp
    thirdparty/meshoptimizer/src/vertexfilter.cpp
    PROPERTIES COMPILE_OPTIONS "-w"
)

# Compiler warnings (suppress noisy ones common in Vulkan code)
target_compile_options(vgeo_builder PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
        -Wno-missing-field-initializers
    >
)

# AddressSanitizer support
option(MERIDIAN_ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(MERIDIAN_ENABLE_ASAN)
    target_compile_options(vgeo_builder PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(vgeo_builder PUBLIC -fsanitize=address)
endif()

if(TARGET PkgConfig::VULKAN)
    target_link_libraries(vgeo_builder PRIVATE PkgConfig::VULKAN)
endif()

if(TARGET PkgConfig::GLFW)
    target_link_libraries(vgeo_builder PRIVATE PkgConfig::GLFW)
endif()

if(TARGET PkgConfig::SHADERC)
    target_link_libraries(vgeo_builder PRIVATE PkgConfig::SHADERC)
endif()

if(APPLE)
    find_library(OBJC_LIBRARY objc REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(vgeo_builder PRIVATE ${OBJC_LIBRARY} ${COCOA_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
endif()

add_executable(meridian_builder
    src/main.cpp
)

target_link_libraries(meridian_builder PRIVATE vgeo_builder)

add_executable(meridian_dump
    src/dump_main.cpp
)

target_link_libraries(meridian_dump PRIVATE vgeo_builder)

add_executable(meridian_trace
    src/trace_main.cpp
)

target_link_libraries(meridian_trace PRIVATE vgeo_builder)

add_executable(meridian_residency
    src/residency_main.cpp
)

target_link_libraries(meridian_residency PRIVATE vgeo_builder)

add_executable(meridian_vk_bootstrap
    src/bootstrap_main.cpp
)

target_link_libraries(meridian_vk_bootstrap PRIVATE vgeo_builder)

add_executable(meridian_replay
    src/replay_main.cpp
)

target_link_libraries(meridian_replay PRIVATE vgeo_builder)
