#pragma once

#include "vk_context.h"
#include "shader_loader.h"

#include <cstdint>
#include <filesystem>
#include <vector>

#if __has_include(<shaderc/shaderc.hpp>)
#include <shaderc/shaderc.hpp>
#define MERIDIAN_HELPERS_HAS_SHADERC 1
#else
#define MERIDIAN_HELPERS_HAS_SHADERC 0
#endif

namespace meridian {

#if MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW

std::filesystem::path resolve_shader_path(const char* name);

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                          VkMemoryPropertyFlags required_properties);

VkResult create_uploaded_buffer(VkPhysicalDevice physical_device, VkDevice device, const void* data,
                                VkDeviceSize size, VkBufferUsageFlags usage,
                                UploadedBuffer& uploaded_buffer);

VkFormat find_depth_format(VkPhysicalDevice physical_device);

#if MERIDIAN_HELPERS_HAS_SHADERC
std::vector<uint32_t> compile_glsl_to_spirv(const std::string& source, shaderc_shader_kind kind,
                                            const char* name);
#endif

VkShaderModule create_shader_module(VkDevice device, const std::vector<uint32_t>& spirv);

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW

}  // namespace meridian
