#include "vk_bootstrap.h"
#include "vk_context.h"
#include "vk_helpers.h"
#include "gpu_profiler.h"
#include "math_utils.h"
#include "async_reader.h"
#include "runtime_model.h"
#include "shader_loader.h"
#include "streaming_scheduler.h"
#include "visibility_format.h"

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#if defined(__APPLE__) && __has_include(<vulkan/vulkan_metal.h>)
#include <vulkan/vulkan_metal.h>
#endif
#define MERIDIAN_HAS_VULKAN 1
#else
#define MERIDIAN_HAS_VULKAN 0
#endif

#if __has_include(<GLFW/glfw3.h>)
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#define MERIDIAN_HAS_GLFW 1
#else
#define MERIDIAN_HAS_GLFW 0
#endif

#if MERIDIAN_HAS_VULKAN && !defined(VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR)
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

#if MERIDIAN_HAS_VULKAN && !defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#endif

#if MERIDIAN_HAS_VULKAN && !defined(VK_EXT_METAL_SURFACE_EXTENSION_NAME)
#define VK_EXT_METAL_SURFACE_EXTENSION_NAME "VK_EXT_metal_surface"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

#if __has_include(<shaderc/shaderc.hpp>)
#include <shaderc/shaderc.hpp>
#define MERIDIAN_HAS_SHADERC 1
#else
#define MERIDIAN_HAS_SHADERC 0
#endif

namespace meridian {

std::filesystem::path resolve_shader_path(const char* name) {
    // Try relative to executable first, then relative to CWD
    auto exe_dir = std::filesystem::current_path();
    auto candidates = {
        exe_dir / ".." / "shaders" / name,  // build/.. = prototype root
        exe_dir / "shaders" / name,
        std::filesystem::path("shaders") / name,
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    throw std::runtime_error(std::string("shader not found: ") + name);
}

namespace {

#if MERIDIAN_HAS_VULKAN && MERIDIAN_HAS_GLFW

// Structs and types now provided by vk_context.h and math_utils.h

void configure_macos_moltenvk_environment() {
#if defined(__APPLE__)
    if (std::getenv("VK_ICD_FILENAMES") == nullptr) {
        constexpr const char* candidates[] = {
            "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json",
            "/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json",
        };
        for (const char* candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                setenv("VK_ICD_FILENAMES", candidate, 0);
                break;
            }
        }
    }

    if (std::getenv("VK_LAYER_PATH") == nullptr) {
        constexpr const char* candidates[] = {
            "/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d",
            "/usr/local/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d",
        };
        for (const char* candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                setenv("VK_LAYER_PATH", candidate, 0);
                break;
            }
        }
    }
#endif
}

bool supports_extension(const std::vector<VkExtensionProperties>& properties, const char* extension_name) {
    return std::any_of(properties.begin(), properties.end(), [&](const VkExtensionProperties& property) {
        return std::strcmp(property.extensionName, extension_name) == 0;
    });
}

}  // close anonymous namespace for shared helper definitions

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                          VkMemoryPropertyFlags required_properties) {
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (uint32_t memory_index = 0; memory_index < memory_properties.memoryTypeCount; ++memory_index) {
        const bool type_supported = (type_bits & (1u << memory_index)) != 0;
        const bool properties_supported =
            (memory_properties.memoryTypes[memory_index].propertyFlags & required_properties) ==
            required_properties;
        if (type_supported && properties_supported) {
            return memory_index;
        }
    }

    return kInvalidQueueFamily;
}

VkResult create_uploaded_buffer(VkPhysicalDevice physical_device, VkDevice device, const void* data,
                                VkDeviceSize size, VkBufferUsageFlags usage,
                                UploadedBuffer& uploaded_buffer) {
    if (size == 0) {
        return VK_SUCCESS;
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &uploaded_buffer.buffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements memory_requirements{};
    vkGetBufferMemoryRequirements(device, uploaded_buffer.buffer, &memory_requirements);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex =
        find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocate_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &uploaded_buffer.memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    void* mapped = nullptr;
    result = vkMapMemory(device, uploaded_buffer.memory, 0, size, 0, &mapped);
    if (result != VK_SUCCESS) {
        return result;
    }
    if (data != nullptr) {
        std::memcpy(mapped, data, static_cast<size_t>(size));
    } else {
        std::memset(mapped, 0, static_cast<size_t>(size));
    }
    vkUnmapMemory(device, uploaded_buffer.memory);

    result = vkBindBufferMemory(device, uploaded_buffer.buffer, uploaded_buffer.memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    uploaded_buffer.size = size;
    return VK_SUCCESS;
}

// Uploads `data` to a DEVICE_LOCAL buffer via a one-shot HOST_VISIBLE staging
// buffer + vkCmdCopyBuffer on `queue`. Used for large, immutable assets like
// cluster geometry payload where GPU-local residency outperforms keeping the
// data CPU-mapped. Falls back to a HOST_VISIBLE buffer if the implementation
// does not surface a pure DEVICE_LOCAL memory type (no functional change,
// just skips the copy).
VkResult create_device_local_buffer_staged(VkPhysicalDevice physical_device, VkDevice device,
                                           VkQueue queue, uint32_t queue_family,
                                           const void* data, VkDeviceSize size,
                                           VkBufferUsageFlags usage,
                                           UploadedBuffer& out_buffer) {
    if (size == 0) return VK_SUCCESS;

    // If the platform has no DEVICE_LOCAL-without-HOST-VISIBLE memory type,
    // the staging dance is pure overhead -- just fall back to the existing
    // HOST_COHERENT create path.
    {
        VkBufferCreateInfo probe_info{};
        probe_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        probe_info.size = size;
        probe_info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        probe_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer probe_buf = VK_NULL_HANDLE;
        if (vkCreateBuffer(device, &probe_info, nullptr, &probe_buf) == VK_SUCCESS) {
            VkMemoryRequirements mreq{};
            vkGetBufferMemoryRequirements(device, probe_buf, &mreq);
            vkDestroyBuffer(device, probe_buf, nullptr);
            const uint32_t dev_only = find_memory_type(physical_device, mreq.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            const uint32_t host_any = find_memory_type(physical_device, mreq.memoryTypeBits,
                                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            if (dev_only == kInvalidQueueFamily || dev_only == host_any) {
                // Unified-memory path (typical for Apple): no staging win.
                return create_uploaded_buffer(physical_device, device, data, size, usage, out_buffer);
            }
        }
    }

    // Staging buffer (HOST_VISIBLE, TRANSFER_SRC).
    UploadedBuffer staging{};
    VkResult r = create_uploaded_buffer(physical_device, device, data, size,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging);
    if (r != VK_SUCCESS) return r;

    // Destination buffer (DEVICE_LOCAL + usage + TRANSFER_DST).
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = size;
    buf_info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(device, &buf_info, nullptr, &out_buffer.buffer);
    if (r != VK_SUCCESS) { destroy_uploaded_buffer(device, staging); return r; }

    VkMemoryRequirements mreq{};
    vkGetBufferMemoryRequirements(device, out_buffer.buffer, &mreq);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mreq.size;
    alloc.memoryTypeIndex = find_memory_type(physical_device, mreq.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == kInvalidQueueFamily) {
        destroy_uploaded_buffer(device, staging);
        vkDestroyBuffer(device, out_buffer.buffer, nullptr);
        out_buffer.buffer = VK_NULL_HANDLE;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    r = vkAllocateMemory(device, &alloc, nullptr, &out_buffer.memory);
    if (r != VK_SUCCESS) { destroy_uploaded_buffer(device, staging); return r; }
    r = vkBindBufferMemory(device, out_buffer.buffer, out_buffer.memory, 0);
    if (r != VK_SUCCESS) { destroy_uploaded_buffer(device, staging); return r; }
    out_buffer.size = size;

    // One-shot transient command pool for the copy.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    r = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    if (r != VK_SUCCESS) { destroy_uploaded_buffer(device, staging); return r; }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cb_info{};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cb_info, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, out_buffer.buffer, 1, &copy);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkDestroyCommandPool(device, pool, nullptr);
    destroy_uploaded_buffer(device, staging);
    return VK_SUCCESS;
}

namespace {  // reopen anonymous namespace for internal helpers

VkResult update_uploaded_buffer(VkDevice device, const void* data, VkDeviceSize size,
                                UploadedBuffer& uploaded_buffer) {
    if (size == 0 || uploaded_buffer.memory == VK_NULL_HANDLE) {
        return VK_SUCCESS;
    }
    if (size > uploaded_buffer.size) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    void* mapped = nullptr;
    VkResult result = vkMapMemory(device, uploaded_buffer.memory, 0, size, 0, &mapped);
    if (result != VK_SUCCESS) {
        return result;
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(device, uploaded_buffer.memory);
    return VK_SUCCESS;
}

}  // close anonymous namespace for header-declared function definitions

void destroy_uploaded_buffer(VkDevice device, UploadedBuffer& uploaded_buffer) {
    if (uploaded_buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, uploaded_buffer.buffer, nullptr);
    }
    if (uploaded_buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, uploaded_buffer.memory, nullptr);
    }
    uploaded_buffer = {};
}

void destroy_uploaded_scene_buffers(VkDevice device, UploadedSceneBuffers& buffers) {
    destroy_uploaded_buffer(device, buffers.lod_payload);
    destroy_uploaded_buffer(device, buffers.base_payload);
    destroy_uploaded_buffer(device, buffers.page_residency);
    destroy_uploaded_buffer(device, buffers.page_dependencies);
    destroy_uploaded_buffer(device, buffers.pages);
    destroy_uploaded_buffer(device, buffers.node_lod_links);
    destroy_uploaded_buffer(device, buffers.lod_clusters);
    destroy_uploaded_buffer(device, buffers.lod_groups);
    destroy_uploaded_buffer(device, buffers.clusters);
    destroy_uploaded_buffer(device, buffers.hierarchy_nodes);
    destroy_uploaded_buffer(device, buffers.instances);
    destroy_uploaded_buffer(device, buffers.header);
}

namespace {  // reopen anonymous namespace

CameraFrameData build_camera_frame_data(const VGeoResource& resource, const VkExtent2D& extent,
                                        float& camera_distance) {
    const Vec3f center = {
        (resource.bounds.min.x + resource.bounds.max.x) * 0.5f,
        (resource.bounds.min.y + resource.bounds.max.y) * 0.5f,
        (resource.bounds.min.z + resource.bounds.max.z) * 0.5f,
    };
    const Vec3f extents = {
        resource.bounds.max.x - resource.bounds.min.x,
        resource.bounds.max.y - resource.bounds.min.y,
        resource.bounds.max.z - resource.bounds.min.z,
    };
    const float radius = std::max({extents.x, extents.y, extents.z, 1.0f});
    camera_distance = radius * 1.5f;

    const Vec3f eye = {center.x + radius * 0.4f, center.y + radius * 0.5f,
                       center.z + camera_distance};
    const float aspect_ratio =
        std::max(1.0f, static_cast<float>(extent.width)) / std::max(1.0f, static_cast<float>(extent.height));
    const Mat4f view = look_at_matrix(eye, center, {0.0f, 1.0f, 0.0f});
    const Mat4f projection = perspective_matrix(55.0f * 3.1415926535f / 180.0f, aspect_ratio,
                                                std::max(0.01f, radius * 0.01f), radius * 8.0f);

    CameraFrameData frame_data{};
    frame_data.view_projection = multiply_matrix(projection, view);
    frame_data.camera_position = eye;
    return frame_data;
}

}  // close anonymous namespace

void destroy_compute_cull_context(VkDevice device, ComputeCullContext& context) {
    destroy_uploaded_buffer(device, context.visible_instances);
    destroy_uploaded_buffer(device, context.counter);
    if (context.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    }
    if (context.descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    }
    if (context.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, context.pipeline, nullptr);
    }
    if (context.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    }
    context = {};
}

// create_compute_cull_context moved to vk_compute_cull.cpp

void destroy_hzb_context(VkDevice device, HzbContext& context) {
    for (VkImageView view : context.mip_views) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    }
    context.mip_views.clear();
    context.mip_descriptor_sets.clear();
    if (context.sampler != VK_NULL_HANDLE) vkDestroySampler(device, context.sampler, nullptr);
    if (context.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    if (context.descriptor_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    if (context.depth_copy_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, context.depth_copy_set_layout, nullptr);
    if (context.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, context.pipeline, nullptr);
    if (context.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    if (context.depth_copy_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, context.depth_copy_pipeline, nullptr);
    if (context.depth_copy_pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, context.depth_copy_pipeline_layout, nullptr);
    if (context.image != VK_NULL_HANDLE) vkDestroyImage(device, context.image, nullptr);
    if (context.memory != VK_NULL_HANDLE) vkFreeMemory(device, context.memory, nullptr);
    context = {};
}

void destroy_compute_selection_context(VkDevice device, ComputeSelectionContext& context) {
    destroy_uploaded_buffer(device, context.draw_list);
    destroy_uploaded_buffer(device, context.draw_count);
    if (context.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    }
    if (context.descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    }
    if (context.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, context.pipeline, nullptr);
    }
    if (context.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    }
    context = {};
}
// create_compute_selection_context moved to vk_compute_selection.cpp
// create_hzb_context moved to vk_hzb.cpp


void destroy_occlusion_refine_context(VkDevice device, OcclusionRefineContext& context) {
    destroy_uploaded_buffer(device, context.output_draws);
    destroy_uploaded_buffer(device, context.output_count);
    if (context.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    if (context.descriptor_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    if (context.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, context.pipeline, nullptr);
    if (context.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    context = {};
}

// create_occlusion_refine_context moved to vk_occlusion.cpp


void destroy_shadow_context(VkDevice device, ShadowContext& context) {
    for (VkFramebuffer& fb : context.framebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
    }
    if (context.render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, context.render_pass, nullptr);
    for (VkImageView& v : context.cascade_views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device, v, nullptr);
    }
    if (context.depth_array_view != VK_NULL_HANDLE) vkDestroyImageView(device, context.depth_array_view, nullptr);
    if (context.sampler != VK_NULL_HANDLE) vkDestroySampler(device, context.sampler, nullptr);
    if (context.depth_image != VK_NULL_HANDLE) vkDestroyImage(device, context.depth_image, nullptr);
    if (context.depth_memory != VK_NULL_HANDLE) vkFreeMemory(device, context.depth_memory, nullptr);
    for (UploadedBuffer& b : context.cascade_draw_lists) destroy_uploaded_buffer(device, b);
    for (UploadedBuffer& b : context.cascade_draw_counts) destroy_uploaded_buffer(device, b);
    if (context.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    if (context.descriptor_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    if (context.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, context.pipeline, nullptr);
    if (context.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    context = {};
}

// create_shadow_context moved to vk_shadow.cpp

namespace {  // reopen anonymous namespace for internal helpers

void update_debug_selection_report(const TraversalSelection& selection,
                                   const UploadableScene& scene,
                                   VkBootstrapReport& report) {
    report.debug_selected_node_count = static_cast<uint32_t>(selection.selected_node_indices.size());
    report.debug_rendered_cluster_count = static_cast<uint32_t>(selection.selected_cluster_indices.size());
    report.debug_rendered_lod_cluster_count = static_cast<uint32_t>(selection.selected_lod_cluster_indices.size());
    report.replay_selected_node_count = static_cast<uint32_t>(selection.selected_node_indices.size());
    report.replay_selected_cluster_count = static_cast<uint32_t>(selection.selected_cluster_indices.size());
    report.replay_selected_lod_cluster_count =
        static_cast<uint32_t>(selection.selected_lod_cluster_indices.size());
    report.replay_selected_page_count = static_cast<uint32_t>(selection.selected_page_indices.size());

    report.debug_triangle_count = 0;
    report.debug_vertex_count = 0;
    for (const uint32_t cluster_index : selection.selected_cluster_indices) {
        const GpuClusterRecord& cluster = scene.clusters[cluster_index];
        report.debug_triangle_count += cluster.local_triangle_count;
        report.debug_vertex_count += cluster.local_vertex_count;
    }
    for (const uint32_t lod_cluster_index : selection.selected_lod_cluster_indices) {
        const GpuLodClusterRecord& cluster = scene.lod_clusters[lod_cluster_index];
        report.debug_triangle_count += cluster.local_triangle_count;
        report.debug_vertex_count += cluster.local_vertex_count;
    }
}

}  // close anonymous namespace


#if MERIDIAN_HAS_SHADERC
std::vector<uint32_t> compile_glsl_to_spirv(const std::string& source, shaderc_shader_kind kind,
                                            const char* name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    const shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(source, kind, name, options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw BuilderError(std::string("shader compilation failed for ") + name + ": " +
                           result.GetErrorMessage());
    }
    return {result.cbegin(), result.cend()};
}
#endif

VkShaderModule create_shader_module(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = spirv.size() * sizeof(uint32_t);
    create_info.pCode = spirv.data();

    VkShaderModule shader_module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &create_info, nullptr, &shader_module);
    if (result != VK_SUCCESS) {
        throw BuilderError("vkCreateShaderModule failed");
    }
    return shader_module;
}



void destroy_debug_render_context(VkDevice device, DebugRenderContext& context) {
    destroy_uploaded_buffer(device, context.visibility_readback_buffer);
    destroy_uploaded_buffer(device, context.frame_ubo);
    if (context.placeholder_depth_view != VK_NULL_HANDLE) vkDestroyImageView(device, context.placeholder_depth_view, nullptr);
    if (context.placeholder_depth_image != VK_NULL_HANDLE) vkDestroyImage(device, context.placeholder_depth_image, nullptr);
    if (context.placeholder_depth_memory != VK_NULL_HANDLE) vkFreeMemory(device, context.placeholder_depth_memory, nullptr);
    if (context.placeholder_sampler != VK_NULL_HANDLE) vkDestroySampler(device, context.placeholder_sampler, nullptr);
    if (context.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    }
    if (context.descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    }
    for (VkFramebuffer framebuffer : context.framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    context.framebuffers.clear();
    if (context.depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, context.depth_view, nullptr);
    }
    if (context.depth_image != VK_NULL_HANDLE) {
        vkDestroyImage(device, context.depth_image, nullptr);
    }
    if (context.depth_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, context.depth_memory, nullptr);
    }
    if (context.visibility_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, context.visibility_view, nullptr);
    }
    if (context.visibility_image != VK_NULL_HANDLE) {
        vkDestroyImage(device, context.visibility_image, nullptr);
    }
    if (context.visibility_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, context.visibility_memory, nullptr);
    }
    if (context.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, context.pipeline, nullptr);
    }
    if (context.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    }
    if (context.render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, context.render_pass, nullptr);
    }
    context = {};
}

namespace {  // reopen anonymous namespace

template <typename T>
VkResult upload_vector_buffer(VkPhysicalDevice physical_device, VkDevice device,
                              const std::vector<T>& values, VkBufferUsageFlags usage,
                              UploadedBuffer& uploaded_buffer);

template <typename T>
VkResult update_vector_buffer(VkDevice device, const std::vector<T>& values,
                              UploadedBuffer& uploaded_buffer);

template <typename T>
VkResult upload_vector_buffer(VkPhysicalDevice physical_device, VkDevice device,
                              const std::vector<T>& values, VkBufferUsageFlags usage,
                              UploadedBuffer& uploaded_buffer) {
    if (values.empty()) {
        return VK_SUCCESS;
    }
    return create_uploaded_buffer(physical_device, device, values.data(),
                                  static_cast<VkDeviceSize>(values.size() * sizeof(T)), usage,
                                  uploaded_buffer);
}

template <typename T>
VkResult update_vector_buffer(VkDevice device, const std::vector<T>& values,
                              UploadedBuffer& uploaded_buffer) {
    if (values.empty()) {
        return VK_SUCCESS;
    }
    return update_uploaded_buffer(device, values.data(),
                                  static_cast<VkDeviceSize>(values.size() * sizeof(T)),
                                  uploaded_buffer);
}

VkResult upload_scene_buffers(VkPhysicalDevice physical_device, VkDevice device,
                              VkQueue upload_queue, uint32_t upload_queue_family,
                              const UploadableScene& scene, UploadedSceneBuffers& buffers,
                              VkBootstrapReport& report) {
    const VkBufferUsageFlags metadata_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkResult result = create_uploaded_buffer(physical_device, device, &scene.header,
                                             sizeof(scene.header), metadata_usage, buffers.header);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.instances, metadata_usage,
                                  buffers.instances);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.hierarchy_nodes, metadata_usage,
                                  buffers.hierarchy_nodes);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.clusters, metadata_usage,
                                  buffers.clusters);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.lod_groups, metadata_usage,
                                  buffers.lod_groups);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.lod_clusters, metadata_usage,
                                  buffers.lod_clusters);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.node_lod_links, metadata_usage,
                                  buffers.node_lod_links);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.pages, metadata_usage,
                                  buffers.pages);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.page_dependencies, metadata_usage,
                                  buffers.page_dependencies);
    if (result != VK_SUCCESS) return result;
    result = upload_vector_buffer(physical_device, device, scene.page_residency, metadata_usage,
                                  buffers.page_residency);
    if (result != VK_SUCCESS) return result;
    // Payload buffers are large and immutable. Push them to DEVICE_LOCAL via
    // a staged copy when the platform has a dedicated device-only heap.
    if (!scene.base_payload.empty()) {
        result = create_device_local_buffer_staged(
            physical_device, device, upload_queue, upload_queue_family,
            scene.base_payload.data(),
            static_cast<VkDeviceSize>(scene.base_payload.size()),
            metadata_usage, buffers.base_payload);
    }
    if (result != VK_SUCCESS) return result;
    if (!scene.lod_payload.empty()) {
        result = create_device_local_buffer_staged(
            physical_device, device, upload_queue, upload_queue_family,
            scene.lod_payload.data(),
            static_cast<VkDeviceSize>(scene.lod_payload.size()),
            metadata_usage, buffers.lod_payload);
    }
    if (result != VK_SUCCESS) return result;

    const UploadedBuffer* all_buffers[] = {
        &buffers.header,          &buffers.instances,     &buffers.hierarchy_nodes,
        &buffers.clusters,        &buffers.lod_groups,    &buffers.lod_clusters,
        &buffers.node_lod_links,  &buffers.pages,         &buffers.page_dependencies,
        &buffers.page_residency,  &buffers.base_payload,  &buffers.lod_payload,
    };

    report.uploaded_buffer_count = 0;
    report.uploaded_buffer_bytes = 0;
    for (const UploadedBuffer* buffer : all_buffers) {
        if (buffer->buffer != VK_NULL_HANDLE) {
            report.uploaded_buffer_count += 1;
            report.uploaded_buffer_bytes += static_cast<uint64_t>(buffer->size);
        }
    }
    report.scene_buffers_uploaded = true;
    return VK_SUCCESS;
}

uint32_t count_resident_pages(const ResidencyModel& model) {
    uint32_t resident_count = 0;
    for (const PageResidencyEntry& entry : model.pages) {
        if (entry.state == PageResidencyState::resident ||
            entry.state == PageResidencyState::eviction_candidate) {
            resident_count += 1;
        }
    }
    return resident_count;
}

uint32_t complete_loading_pages(ResidencyModel& model, uint32_t frame_index) {
    uint32_t completed_count = 0;
    for (PageResidencyEntry& entry : model.pages) {
        if (entry.state == PageResidencyState::loading) {
            entry.state = PageResidencyState::resident;
            entry.last_touched_frame = frame_index;
            completed_count += 1;
        }
    }
    return completed_count;
}

void snapshot_page_residency(UploadableScene& scene, const ResidencyModel& model) {
    scene.page_residency.resize(model.pages.size());
    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        scene.page_residency[page_index].state = static_cast<uint32_t>(model.pages[page_index].state);
        scene.page_residency[page_index].last_touched_frame = model.pages[page_index].last_touched_frame;
        scene.page_residency[page_index].request_priority = model.pages[page_index].request_priority;
        scene.page_residency[page_index].flags = 0;
    }
}

void analyze_visibility_readback(VkDevice device, const SwapchainContext& swapchain,
                                 const DebugRenderContext& debug_render,
                                 const TraversalSelection& selection,
                                 VkBootstrapReport& report) {
    report.visibility_valid_pixels = 0;
    report.visibility_unique_base_geometry = 0;
    report.visibility_unique_lod_geometry = 0;
    report.visibility_invalid_ids = 0;
    report.visibility_visible_selected_base_geometry = 0;
    report.visibility_visible_selected_lod_geometry = 0;
    report.visibility_invisible_selected_base_geometry = 0;
    report.visibility_invisible_selected_lod_geometry = 0;
    report.visibility_selection_subset = false;

    if (debug_render.visibility_readback_buffer.memory == VK_NULL_HANDLE) {
        return;
    }

    void* mapped = nullptr;
    const VkResult result = vkMapMemory(device, debug_render.visibility_readback_buffer.memory, 0,
                                        debug_render.visibility_readback_buffer.size, 0, &mapped);
    if (result != VK_SUCCESS) {
        return;
    }

    const uint32_t pixel_count = swapchain.extent.width * swapchain.extent.height;
    const uint32_t* words = static_cast<const uint32_t*>(mapped);
    std::set<uint32_t> unique_base_ids;
    std::set<uint32_t> unique_lod_ids;
    for (uint32_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index) {
        const VisibilityPixel pixel{words[pixel_index * 2], words[pixel_index * 2 + 1]};
        if (!visibility_valid(pixel)) {
            continue;
        }
        report.visibility_valid_pixels += 1;
        const GeometryKind kind = decode_visibility_geometry_kind(pixel);
        const uint32_t geometry_index = decode_visibility_geometry_index(pixel);
        if (kind == GeometryKind::base_cluster) {
            unique_base_ids.insert(geometry_index);
        } else {
            unique_lod_ids.insert(geometry_index);
        }
    }

    report.visibility_unique_base_geometry = static_cast<uint32_t>(unique_base_ids.size());
    report.visibility_unique_lod_geometry = static_cast<uint32_t>(unique_lod_ids.size());
    for (const uint32_t cluster_index : selection.selected_cluster_indices) {
        if (unique_base_ids.find(cluster_index) != unique_base_ids.end()) {
            report.visibility_visible_selected_base_geometry += 1;
        } else {
            report.visibility_invisible_selected_base_geometry += 1;
        }
    }
    for (const uint32_t lod_cluster_index : selection.selected_lod_cluster_indices) {
        if (unique_lod_ids.find(lod_cluster_index) != unique_lod_ids.end()) {
            report.visibility_visible_selected_lod_geometry += 1;
        } else {
            report.visibility_invisible_selected_lod_geometry += 1;
        }
    }

    bool subset_ok = true;
    for (const uint32_t geometry_index : unique_base_ids) {
        if (std::find(selection.selected_cluster_indices.begin(), selection.selected_cluster_indices.end(),
                      geometry_index) == selection.selected_cluster_indices.end()) {
            subset_ok = false;
            break;
        }
    }
    if (subset_ok) {
        for (const uint32_t geometry_index : unique_lod_ids) {
            if (std::find(selection.selected_lod_cluster_indices.begin(),
                          selection.selected_lod_cluster_indices.end(), geometry_index) ==
                selection.selected_lod_cluster_indices.end()) {
                subset_ok = false;
                break;
            }
        }
    }
    report.visibility_selection_subset = subset_ok && report.visibility_invalid_ids == 0;
    report.visibility_readback_ready = true;
    vkUnmapMemory(device, debug_render.visibility_readback_buffer.memory);
}

std::vector<const char*> collect_instance_extensions() {
    uint32_t available_extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(available_extension_count);
    if (available_extension_count > 0) {
        vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count,
                                               available_extensions.data());
    }

    uint32_t glfw_extension_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
    std::vector<const char*> extensions;
    if (glfw_extensions != nullptr && glfw_extension_count > 0) {
        extensions.assign(glfw_extensions, glfw_extensions + glfw_extension_count);
    } else {
#if defined(__APPLE__)
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        if (supports_extension(available_extensions, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        } else {
            throw BuilderError("required macOS metal surface extension is not available");
        }
#else
        throw BuilderError("GLFW did not report required Vulkan instance extensions");
#endif
    }

    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    return extensions;
}

std::vector<const char*> collect_validation_layers(bool enable_validation) {
    std::vector<const char*> layers;
    if (!enable_validation) {
        return layers;
    }

    uint32_t layer_count = 0;
    if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) != VK_SUCCESS || layer_count == 0) {
        return layers;
    }

    std::vector<VkLayerProperties> available_layers(layer_count);
    if (vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data()) != VK_SUCCESS) {
        return layers;
    }

    for (const VkLayerProperties& layer : available_layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            break;
        }
    }

    return layers;
}

VkResult create_window_surface(VkInstance instance, GLFWwindow* window, VkSurfaceKHR* surface) {
#if defined(__APPLE__)
    NSWindow* cocoa_window = glfwGetCocoaWindow(window);
    if (cocoa_window == nil) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    NSView* cocoa_view = [cocoa_window contentView];
    if (cocoa_view == nil) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    [cocoa_view setWantsLayer:YES];
    CAMetalLayer* metal_layer = [CAMetalLayer layer];
    [cocoa_view setLayer:metal_layer];

    VkMetalSurfaceCreateInfoEXT create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    create_info.pLayer = metal_layer;
    return vkCreateMetalSurfaceEXT(instance, &create_info, nullptr, surface);
#else
    return glfwCreateWindowSurface(instance, window, nullptr, surface);
#endif
}

QueueFamilySelection select_queue_families(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    QueueFamilySelection selection;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                             queue_families.data());

    for (uint32_t family_index = 0; family_index < queue_family_count; ++family_index) {
        const VkQueueFamilyProperties& family = queue_families[family_index];
        if ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && selection.graphics_family == kInvalidQueueFamily) {
            selection.graphics_family = family_index;
        }

        VkBool32 supports_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, family_index, surface, &supports_present);
        if (supports_present == VK_TRUE && selection.present_family == kInvalidQueueFamily) {
            selection.present_family = family_index;
        }

        if (selection.complete() && selection.graphics_family == selection.present_family) {
            break;
        }
    }

    return selection;
}

bool has_swapchain_support(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
    uint32_t format_count = 0;
    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count, nullptr);
    return format_count > 0 && present_mode_count > 0;
}

DeviceSelection select_device(VkInstance instance, VkSurfaceKHR surface, VkBootstrapReport& report) {
    uint32_t physical_device_count = 0;
    vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    if (physical_device_count > 0) {
        vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data());
    }

    DeviceSelection selection;
    for (VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        report.physical_devices.push_back(properties.deviceName);

        QueueFamilySelection queues = select_queue_families(physical_device, surface);
        if (!queues.complete()) {
            continue;
        }

        uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (extension_count > 0) {
            vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count,
                                                 extensions.data());
        }

        if (!supports_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        if (!has_swapchain_support(physical_device, surface)) {
            continue;
        }

        selection.physical_device = physical_device;
        selection.queues = queues;
        selection.enable_portability_subset =
            supports_extension(extensions, "VK_KHR_portability_subset");
        selection.has_draw_indirect_count =
            supports_extension(extensions, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
        report.selected_device = properties.deviceName;
        report.graphics_queue_family = queues.graphics_family;
        report.present_queue_family = queues.present_family;
        return selection;
    }

    return selection;
}

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& present_modes) {
    for (const VkPresentModeKHR present_mode : present_modes) {
        if (present_mode == VK_PRESENT_MODE_FIFO_KHR) {
            return present_mode;
        }
    }
    return present_modes.front();
}

VkExtent2D choose_swapchain_extent(GLFWwindow* window, const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    while (framebuffer_width == 0 || framebuffer_height == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    }

    VkExtent2D extent{};
    extent.width = std::clamp(static_cast<uint32_t>(framebuffer_width), capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(static_cast<uint32_t>(framebuffer_height), capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

}  // close anonymous namespace

void destroy_swapchain(VkDevice device, SwapchainContext& swapchain) {
    for (VkImageView image_view : swapchain.image_views) {
        vkDestroyImageView(device, image_view, nullptr);
    }
    swapchain.image_views.clear();
    swapchain.images.clear();
    if (swapchain.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain.swapchain, nullptr);
        swapchain.swapchain = VK_NULL_HANDLE;
    }
}

namespace {  // reopen anonymous namespace

VkResult create_swapchain(VkPhysicalDevice physical_device, VkDevice device, VkSurfaceKHR surface,
                          GLFWwindow* window, const QueueFamilySelection& queues,
                          SwapchainContext& swapchain) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data());

    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_mode_count,
                                              present_modes.data());

    swapchain.surface_format = choose_surface_format(formats);
    swapchain.present_mode = choose_present_mode(present_modes);
    swapchain.extent = choose_swapchain_extent(window, capabilities);

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    const uint32_t queue_family_indices[] = {queues.graphics_family, queues.present_family};
    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = swapchain.surface_format.format;
    create_info.imageColorSpace = swapchain.surface_format.colorSpace;
    create_info.imageExtent = swapchain.extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = swapchain.present_mode;
    create_info.clipped = VK_TRUE;

    if (queues.graphics_family != queues.present_family) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkResult result = vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain.swapchain);
    if (result != VK_SUCCESS) {
        return result;
    }

    uint32_t swapchain_image_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain.swapchain, &swapchain_image_count, nullptr);
    swapchain.images.resize(swapchain_image_count);
    vkGetSwapchainImagesKHR(device, swapchain.swapchain, &swapchain_image_count,
                            swapchain.images.data());

    swapchain.image_views.resize(swapchain.images.size());
    for (size_t image_index = 0; image_index < swapchain.images.size(); ++image_index) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain.images[image_index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain.surface_format.format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        result = vkCreateImageView(device, &view_info, nullptr, &swapchain.image_views[image_index]);
        if (result != VK_SUCCESS) {
            destroy_swapchain(device, swapchain);
            return result;
        }
    }

    return VK_SUCCESS;
}

VkResult create_frame_context(VkDevice device, const QueueFamilySelection& queues, FrameContext& frame) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queues.graphics_family;
    VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &frame.command_pool);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = frame.command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &allocate_info, &frame.command_buffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    result = vkCreateSemaphore(device, &semaphore_info, nullptr, &frame.image_available);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = vkCreateSemaphore(device, &semaphore_info, nullptr, &frame.render_finished);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(device, &fence_info, nullptr, &frame.in_flight);
}


}  // close anonymous namespace for find_depth_format extraction

VkFormat find_depth_format(VkPhysicalDevice physical_device) {
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (VkFormat format : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

// create_depth_resources, create_visibility_resources, create_debug_render_context moved to vk_render.cpp

void destroy_frame_context(VkDevice device, FrameContext& frame) {
    if (frame.in_flight != VK_NULL_HANDLE) {
        vkDestroyFence(device, frame.in_flight, nullptr);
    }
    if (frame.render_finished != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, frame.render_finished, nullptr);
    }
    if (frame.image_available != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, frame.image_available, nullptr);
    }
    if (frame.command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, frame.command_pool, nullptr);
    }
}

namespace {  // reopen anonymous namespace

VkResult record_debug_command_buffer(FrameContext& frame, const DebugRenderContext& debug_render,
                                     const ComputeCullContext& compute_cull,
                                     const ComputeSelectionContext& compute_selection,
                                     const HzbContext& hzb,
                                     const OcclusionRefineContext& occlusion_refine,
                                     const ShadowContext& shadow,
                                     const SwapchainContext& swapchain,
                                     const CameraFrameData& camera_frame,
                                     const FrustumPlanes& frustum,
                                     float error_threshold,
                                     const TraversalSelection& selection,
                                     const UploadableScene& scene,
                                     uint32_t frame_index,
                                     uint32_t image_index,
                                     bool has_draw_indirect_count,
                                     const GpuProfiler& profiler) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VkResult result = vkBeginCommandBuffer(frame.command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        return result;
    }

    // GPU profiler: reset queries and write initial timestamp
    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.command_buffer, profiler.query_pool, 0, profiler.query_count);
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 0); // cull start
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 12); // total start
    }

    // Compute instance culling pass
    if (compute_cull.pipeline != VK_NULL_HANDLE && compute_cull.descriptor_set != VK_NULL_HANDLE) {
        // Reset counter to zero
        vkCmdFillBuffer(frame.command_buffer, compute_cull.counter.buffer, 0, sizeof(uint32_t), 0);

        VkMemoryBarrier fill_barrier{};
        fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fill_barrier,
                             0, nullptr, 0, nullptr);

        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_cull.pipeline);
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                compute_cull.pipeline_layout, 0, 1, &compute_cull.descriptor_set,
                                0, nullptr);

        CullPushConstants cull_push{};
        std::memcpy(cull_push.frustum_planes, frustum.planes, sizeof(frustum.planes));
        cull_push.instance_count = compute_cull.max_instances;
        vkCmdPushConstants(frame.command_buffer, compute_cull.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPushConstants), &cull_push);

        const uint32_t group_count = (compute_cull.max_instances + 63) / 64;
        vkCmdDispatch(frame.command_buffer, group_count, 1, 1);

        VkMemoryBarrier compute_barrier{};
        compute_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        compute_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        compute_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &compute_barrier, 0, nullptr, 0, nullptr);
    }

    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 1); // cull end
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 2); // sel start
    }

    // Selection runs on CPU (simulate_traversal) and its output is uploaded into
    // compute_selection.draw_list / .draw_count before command buffer recording.
    // The GPU selection compute shader is retained but not dispatched; kept for
    // future use once parallel traversal replaces serial DFS.
    if (compute_selection.draw_list.buffer != VK_NULL_HANDLE) {
        VkMemoryBarrier host_write_barrier{};
        host_write_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host_write_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host_write_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                           VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1, &host_write_barrier, 0, nullptr, 0, nullptr);
    }

    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 3); // sel end
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 4); // occ start
    }

    // Occlusion refinement pass (uses previous frame's HZB; skip frame 0)
    if (frame_index > 0 && occlusion_refine.pipeline != VK_NULL_HANDLE &&
        occlusion_refine.descriptor_set != VK_NULL_HANDLE) {
        vkCmdFillBuffer(frame.command_buffer, occlusion_refine.output_count.buffer, 0,
                        sizeof(uint32_t), 0);

        VkMemoryBarrier fill_bar{};
        fill_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fill_bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fill_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fill_bar,
                             0, nullptr, 0, nullptr);

        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          occlusion_refine.pipeline);
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                occlusion_refine.pipeline_layout, 0, 1,
                                &occlusion_refine.descriptor_set, 0, nullptr);

        OcclusionPushConstants occ_push{};
        std::memcpy(occ_push.view_projection, camera_frame.view_projection.m,
                    sizeof(occ_push.view_projection));
        occ_push.hzb_width = hzb.width;
        occ_push.hzb_height = hzb.height;
        vkCmdPushConstants(frame.command_buffer, occlusion_refine.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(OcclusionPushConstants), &occ_push);

        const uint32_t occ_groups = (compute_selection.max_draws + 63) / 64;
        vkCmdDispatch(frame.command_buffer, std::max(occ_groups, 1u), 1, 1);

        VkMemoryBarrier occ_bar{};
        occ_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        occ_bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        occ_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1, &occ_bar, 0, nullptr, 0, nullptr);
    }

    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 5); // occ end
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 6); // shadow start
    }

    // Shadow pass: render scene from light perspective, once per cascade.
    // Each cascade has its own draw list (CPU-filled against that cascade's
    // orthographic frustum) and its own framebuffer/layer into the shared
    // 2D array depth image, so a cluster only gets submitted to the cascades
    // whose volume it actually overlaps.
    if (shadow.pipeline != VK_NULL_HANDLE) {
        for (uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade) {
            if (shadow.cascade_descriptor_sets[cascade] == VK_NULL_HANDLE) continue;

            VkClearValue shadow_clear{};
            shadow_clear.depthStencil.depth = 1.0f;

            VkRenderPassBeginInfo shadow_rp_info{};
            shadow_rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            shadow_rp_info.renderPass = shadow.render_pass;
            shadow_rp_info.framebuffer = shadow.framebuffers[cascade];
            shadow_rp_info.renderArea.extent = {shadow.resolution, shadow.resolution};
            shadow_rp_info.clearValueCount = 1;
            shadow_rp_info.pClearValues = &shadow_clear;

            vkCmdBeginRenderPass(frame.command_buffer, &shadow_rp_info, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow.pipeline);
            vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    shadow.pipeline_layout, 0, 1,
                                    &shadow.cascade_descriptor_sets[cascade], 0, nullptr);
            vkCmdPushConstants(frame.command_buffer, shadow.pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &cascade);

            const UploadedBuffer& dl = shadow.cascade_draw_lists[cascade];
            const UploadedBuffer& dc = shadow.cascade_draw_counts[cascade];
            if (dl.buffer != VK_NULL_HANDLE && dc.buffer != VK_NULL_HANDLE) {
                if (has_draw_indirect_count) {
                    vkCmdDrawIndirectCount(frame.command_buffer,
                                           dl.buffer, 0,
                                           dc.buffer, 0,
                                           shadow.max_draws_per_cascade,
                                           sizeof(GpuDrawEntry));
                } else {
                    vkCmdDrawIndirect(frame.command_buffer,
                                      dl.buffer, 0,
                                      shadow.max_draws_per_cascade,
                                      sizeof(GpuDrawEntry));
                }
            }
            vkCmdEndRenderPass(frame.command_buffer);
        }
    }

    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 7); // shadow end
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 8); // main start
    }

    VkClearValue clear_values[3] = {};
    clear_values[0].color.float32[0] = 0.05f;
    clear_values[0].color.float32[1] = 0.07f;
    clear_values[0].color.float32[2] = 0.10f;
    clear_values[0].color.float32[3] = 1.0f;
    clear_values[1].depthStencil.depth = 1.0f;
    clear_values[2].color.uint32[0] = 0u;

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = debug_render.render_pass;
    render_pass_info.framebuffer = debug_render.framebuffers[image_index];
    render_pass_info.renderArea.extent = swapchain.extent;
    render_pass_info.clearValueCount = 3;
    render_pass_info.pClearValues = clear_values;

    vkCmdBeginRenderPass(frame.command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_render.pipeline);
    if (debug_render.descriptor_set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                debug_render.pipeline_layout, 0, 1, &debug_render.descriptor_set,
                                0, nullptr);
        if (compute_selection.draw_list.buffer != VK_NULL_HANDLE &&
            compute_selection.draw_count.buffer != VK_NULL_HANDLE) {
            const bool use_occlusion_output = frame_index > 0 &&
                occlusion_refine.output_draws.buffer != VK_NULL_HANDLE &&
                occlusion_refine.output_count.buffer != VK_NULL_HANDLE;
            VkBuffer draw_buffer = use_occlusion_output
                ? occlusion_refine.output_draws.buffer
                : compute_selection.draw_list.buffer;
            VkBuffer count_buffer = use_occlusion_output
                ? occlusion_refine.output_count.buffer
                : compute_selection.draw_count.buffer;
            uint32_t max_draws = use_occlusion_output
                ? occlusion_refine.max_draws
                : compute_selection.max_draws;
            if (has_draw_indirect_count) {
                vkCmdDrawIndirectCount(frame.command_buffer,
                                       draw_buffer, 0,
                                       count_buffer, 0,
                                       max_draws,
                                       sizeof(GpuDrawEntry));
            } else {
                vkCmdDrawIndirect(frame.command_buffer,
                                  draw_buffer, 0,
                                  max_draws,
                                  sizeof(GpuDrawEntry));
            }
        }
    }
    vkCmdEndRenderPass(frame.command_buffer);

    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 9); // main end
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            profiler.query_pool, 10); // hzb start
    }

    // HZB build: use the downsample shader to seed mip 0 from depth, then cascade
    if (hzb.pipeline != VK_NULL_HANDLE && hzb.mip_count > 1 && hzb.depth_copy_pipeline != VK_NULL_HANDLE) {
        // Transition depth to shader-readable, HZB mip 0 to general for storage write
        VkImageMemoryBarrier depth_to_read{};
        depth_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depth_to_read.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth_to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depth_to_read.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth_to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_to_read.image = debug_render.depth_image;
        depth_to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_to_read.subresourceRange.levelCount = 1;
        depth_to_read.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier hzb_all_to_general{};
        hzb_all_to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hzb_all_to_general.srcAccessMask = 0;
        hzb_all_to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hzb_all_to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        hzb_all_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        hzb_all_to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hzb_all_to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hzb_all_to_general.image = hzb.image;
        hzb_all_to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hzb_all_to_general.subresourceRange.levelCount = hzb.mip_count;
        hzb_all_to_general.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier pre_barriers[2] = {depth_to_read, hzb_all_to_general};
        vkCmdPipelineBarrier(frame.command_buffer,
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             2, pre_barriers);

        // Dispatch depth-copy shader to write depth into HZB mip 0
        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          hzb.depth_copy_pipeline);
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                hzb.depth_copy_pipeline_layout, 0, 1,
                                &hzb.depth_copy_descriptor_set, 0, nullptr);
        vkCmdDispatch(frame.command_buffer, (hzb.width + 7) / 8, (hzb.height + 7) / 8, 1);

        // Barrier: mip 0 written -> readable for downsample
        VkImageMemoryBarrier mip0_to_read{};
        mip0_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mip0_to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mip0_to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        mip0_to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        mip0_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mip0_to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mip0_to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mip0_to_read.image = hzb.image;
        mip0_to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mip0_to_read.subresourceRange.baseMipLevel = 0;
        mip0_to_read.subresourceRange.levelCount = 1;
        mip0_to_read.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &mip0_to_read);

        // Downsample each mip level
        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, hzb.pipeline);
        uint32_t src_w = hzb.width, src_h = hzb.height;
        for (uint32_t mip = 0; mip + 1 < hzb.mip_count; ++mip) {
            vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    hzb.pipeline_layout, 0, 1, &hzb.mip_descriptor_sets[mip],
                                    0, nullptr);

            uint32_t push_data[2] = {src_w, src_h};
            vkCmdPushConstants(frame.command_buffer, hzb.pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, push_data);

            const uint32_t dst_w = std::max(src_w / 2, 1u);
            const uint32_t dst_h = std::max(src_h / 2, 1u);
            vkCmdDispatch(frame.command_buffer, (dst_w + 7) / 8, (dst_h + 7) / 8, 1);

            // Barrier: written mip -> readable for next iteration
            if (mip + 2 < hzb.mip_count) {
                VkImageMemoryBarrier mip_barrier{};
                mip_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                mip_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mip_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                mip_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                mip_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                mip_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mip_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mip_barrier.image = hzb.image;
                mip_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                mip_barrier.subresourceRange.baseMipLevel = mip + 1;
                mip_barrier.subresourceRange.levelCount = 1;
                mip_barrier.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                     1, &mip_barrier);
            }
            src_w = dst_w;
            src_h = dst_h;
        }

        // Transition entire HZB to shader-readable for occlusion testing
        VkImageMemoryBarrier hzb_to_read{};
        hzb_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hzb_to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hzb_to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        hzb_to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        hzb_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        hzb_to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hzb_to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hzb_to_read.image = hzb.image;
        hzb_to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        hzb_to_read.subresourceRange.levelCount = hzb.mip_count;
        hzb_to_read.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &hzb_to_read);
    }

    // (Occlusion refinement moved before shadow/main passes above)

    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 11); // hzb end
        vkCmdWriteTimestamp(frame.command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            profiler.query_pool, 13); // total end
    }

    VkImageMemoryBarrier image_barrier{};
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = debug_render.visibility_image;
    image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image_barrier.subresourceRange.baseMipLevel = 0;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.baseArrayLayer = 0;
    image_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &image_barrier);

    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.mipLevel = 0;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageExtent.width = swapchain.extent.width;
    copy_region.imageExtent.height = swapchain.extent.height;
    copy_region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(frame.command_buffer, debug_render.visibility_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           debug_render.visibility_readback_buffer.buffer, 1, &copy_region);

    VkBufferMemoryBarrier buffer_barrier{};
    buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = debug_render.visibility_readback_buffer.buffer;
    buffer_barrier.size = debug_render.visibility_readback_buffer.size;
    vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer_barrier, 0,
                         nullptr);
    return vkEndCommandBuffer(frame.command_buffer);
}

#endif

}  // namespace

VkBootstrapReport build_vk_bootstrap_report(const VGeoResource& resource,
                                            const VkBootstrapConfig& config) {
    VkBootstrapReport report;
    report.uploadable_scene = build_uploadable_scene(resource);
    report.compiled_with_vulkan = MERIDIAN_HAS_VULKAN != 0;

#if !(MERIDIAN_HAS_VULKAN && MERIDIAN_HAS_GLFW)
    report.status =
        "Vulkan or GLFW headers are not available in this environment; bootstrap cannot create a windowed runtime";
    return report;
#else
    configure_macos_moltenvk_environment();

    if (glfwInit() != GLFW_TRUE) {
        report.status = "glfwInit failed";
        return report;
    }

    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    SwapchainContext swapchain;
    FrameContext frame;
    UploadedSceneBuffers scene_buffers;
    DebugRenderContext debug_render;
    ComputeCullContext compute_cull;
    ComputeSelectionContext compute_selection;
    HzbContext hzb;
    OcclusionRefineContext occlusion_refine;
    ShadowContext shadow;
    TraversalSelection last_submitted_selection;
    uint32_t gpu_draw_count = 0;
    bool has_draw_indirect_count = false;
    GpuProfiler gpu_profiler;

    const auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            destroy_gpu_profiler(device, gpu_profiler);
            destroy_shadow_context(device, shadow);
            destroy_occlusion_refine_context(device, occlusion_refine);
            destroy_hzb_context(device, hzb);
            destroy_compute_selection_context(device, compute_selection);
            destroy_compute_cull_context(device, compute_cull);
            destroy_debug_render_context(device, debug_render);
            destroy_uploaded_scene_buffers(device, scene_buffers);
            destroy_frame_context(device, frame);
            destroy_swapchain(device, swapchain);
            vkDestroyDevice(device, nullptr);
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        glfwTerminate();
    };

    try {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, config.visible_window ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        std::vector<const char*> instance_extensions = collect_instance_extensions();
        std::vector<const char*> validation_layers = collect_validation_layers(config.enable_validation);

        VkApplicationInfo application_info{};
        application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application_info.pApplicationName = "Project Meridian Bootstrap";
        application_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        application_info.pEngineName = "Meridian";
        application_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        application_info.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application_info;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
        instance_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
        instance_info.ppEnabledLayerNames =
            validation_layers.empty() ? nullptr : validation_layers.data();
        instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

        VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "vkCreateInstance failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }
        report.instance_created = true;

        window = glfwCreateWindow(static_cast<int>(config.window_width),
                                  static_cast<int>(config.window_height), "Meridian Bootstrap", nullptr,
                                  nullptr);
        if (window == nullptr) {
            report.status = "glfwCreateWindow failed";
            cleanup();
            return report;
        }
        report.window_created = true;

        result = create_window_surface(instance, window, &surface);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_window_surface failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }
        report.surface_created = true;

        const DeviceSelection selection = select_device(instance, surface, report);
        if (selection.physical_device == VK_NULL_HANDLE) {
            report.status = "no compatible physical device found for graphics, present, and swapchain support";
            cleanup();
            return report;
        }

        const float queue_priority = 1.0f;
        std::set<uint32_t> unique_families = {selection.queues.graphics_family,
                                              selection.queues.present_family};
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        queue_infos.reserve(unique_families.size());
        for (const uint32_t family_index : unique_families) {
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = family_index;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &queue_priority;
            queue_infos.push_back(queue_info);
        }

        std::vector<const char*> device_extensions;
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        if (selection.enable_portability_subset) {
            device_extensions.push_back("VK_KHR_portability_subset");
        }
        if (selection.has_draw_indirect_count) {
            device_extensions.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
        }

        VkPhysicalDeviceFeatures device_features{};
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        device_info.pEnabledFeatures = &device_features;

        result = vkCreateDevice(selection.physical_device, &device_info, nullptr, &device);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "vkCreateDevice failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }
        report.device_created = true;
        has_draw_indirect_count = selection.has_draw_indirect_count;

        vkGetDeviceQueue(device, selection.queues.graphics_family, 0, &graphics_queue);
        vkGetDeviceQueue(device, selection.queues.present_family, 0, &present_queue);

        // Create GPU profiler (7 timer pairs: cull, sel, occ, shadow, main, hzb, total)
        {
            VkResult prof_result = create_gpu_profiler(selection.physical_device, device, 7, gpu_profiler);
            if (prof_result == VK_SUCCESS) {
                gpu_profiler.names[0] = "cull";
                gpu_profiler.names[1] = "sel";
                gpu_profiler.names[2] = "occ";
                gpu_profiler.names[3] = "shadow";
                gpu_profiler.names[4] = "main";
                gpu_profiler.names[5] = "hzb";
                gpu_profiler.names[6] = "total";
            } else {
                std::fprintf(stderr, "MERIDIAN_GPU: timestamp queries not supported (code %d), profiling disabled\n",
                             static_cast<int>(prof_result));
            }
        }

        result = upload_scene_buffers(selection.physical_device, device,
                                      graphics_queue, selection.queues.graphics_family,
                                      report.uploadable_scene,
                                      scene_buffers, report);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "upload_scene_buffers failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        result = create_compute_cull_context(selection.physical_device, device, scene_buffers,
                                              static_cast<uint32_t>(report.uploadable_scene.instances.size()),
                                              compute_cull);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_compute_cull_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        const uint32_t total_clusters =
            static_cast<uint32_t>(report.uploadable_scene.clusters.size() +
                                  report.uploadable_scene.lod_clusters.size());
        result = create_compute_selection_context(selection.physical_device, device, scene_buffers,
                                                   compute_cull, total_clusters, compute_selection);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_compute_selection_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        result = create_swapchain(selection.physical_device, device, surface, window, selection.queues,
                                  swapchain);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_swapchain failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }
        report.swapchain_created = true;
        report.swapchain_image_count = static_cast<uint32_t>(swapchain.images.size());
        report.swapchain_width = swapchain.extent.width;
        report.swapchain_height = swapchain.extent.height;

        CameraFrameData camera_frame =
            build_camera_frame_data(resource, swapchain.extent, report.debug_camera_distance);

        // Interactive camera setup
        InteractiveCamera interactive_cam;
        if (config.interactive) {
            const Vec3f center = {
                (resource.bounds.min.x + resource.bounds.max.x) * 0.5f,
                (resource.bounds.min.y + resource.bounds.max.y) * 0.5f,
                (resource.bounds.min.z + resource.bounds.max.z) * 0.5f,
            };
            const float radius = std::max({
                resource.bounds.max.x - resource.bounds.min.x,
                resource.bounds.max.y - resource.bounds.min.y,
                resource.bounds.max.z - resource.bounds.min.z, 1.0f});
            interactive_cam.position = {center.x + radius * 0.55f, center.y + radius * 0.9f,
                                        center.z + radius * 2.4f};
            // Compute yaw/pitch to face the center of the model
            const Vec3f to_center = {center.x - interactive_cam.position.x,
                                     center.y - interactive_cam.position.y,
                                     center.z - interactive_cam.position.z};
            const float horiz_dist = std::sqrt(to_center.x * to_center.x + to_center.z * to_center.z);
            interactive_cam.yaw = std::atan2(to_center.x, -to_center.z);
            interactive_cam.pitch = std::atan2(to_center.y, horiz_dist);
            interactive_cam.move_speed = radius * 0.8f;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(window, &interactive_cam.last_cursor_x, &interactive_cam.last_cursor_y);
            interactive_cam.cursor_captured = true;
        }
        double last_frame_time = glfwGetTime();
        uint32_t fps_frame_count = 0;
        double fps_timer = last_frame_time;
        std::vector<double> frame_times_ms;

        ResidencyModel residency_model = create_residency_model(resource);
        // Demand-streaming: start pages unloaded so the scheduler drives the
        // loads from frame 0. Default path leaves every page resident for the
        // existing "all-in-memory" benchmark behavior.
        std::vector<uint32_t> page_load_start_frame(residency_model.pages.size(), 0xffffffffu);
        // Per-page absolute file offset + size, filled when async I/O is on.
        struct PageFileRange { uint64_t offset = 0; uint32_t size = 0; };
        std::vector<PageFileRange> page_file_ranges(residency_model.pages.size());
        AsyncReader async_reader;
        std::filesystem::path temp_vgeo_path;
        bool async_io_active = false;
        StreamingScheduler streaming_scheduler;
        if (config.demand_streaming) {
            for (PageResidencyEntry& entry : residency_model.pages) {
                entry.state = PageResidencyState::unloaded;
                entry.last_touched_frame = 0xffffffffu;
            }
            // Root-page autodetect: run a coarse traversal with all pages
            // marked resident and a very large error threshold to discover
            // which pages the hierarchy actually needs for its minimum-detail
            // render. This is the correct seed set regardless of storage
            // layout (previously we seeded the first N pages by index, which
            // happened to work only because the DFS flatten pass tends to
            // land low-detail clusters at the front of the linear layout).
            const std::vector<uint8_t> all_resident_mask(residency_model.pages.size(), 1);
            const TraversalSelection coarse =
                simulate_traversal(resource, /*error_threshold=*/1e30f, all_resident_mask);
            const uint32_t seed_cap = std::min<uint32_t>(
                config.streaming_seed_pages,
                static_cast<uint32_t>(residency_model.pages.size()));
            uint32_t seeded = 0;
            for (uint32_t p : coarse.selected_page_indices) {
                if (p >= residency_model.pages.size()) continue;
                if (residency_model.pages[p].state == PageResidencyState::resident) continue;
                residency_model.pages[p].state = PageResidencyState::resident;
                residency_model.pages[p].last_touched_frame = 0;
                if (++seeded >= seed_cap) break;
            }
            // Fall back to linear seed only if the coarse traversal produced
            // fewer pages than the seed cap (unlikely but defensive against
            // scenes where the root has zero LOD links and an empty base
            // span -- e.g. a completely uninitialised hierarchy).
            for (uint32_t p = 0; seeded < seed_cap && p < residency_model.pages.size(); ++p) {
                if (residency_model.pages[p].state == PageResidencyState::resident) continue;
                residency_model.pages[p].state = PageResidencyState::resident;
                residency_model.pages[p].last_touched_frame = 0;
                ++seeded;
            }
            StreamingConfig sc;
            sc.max_resident_pages = config.resident_budget == 0xffffffffu
                                        ? static_cast<uint32_t>(residency_model.pages.size())
                                        : config.resident_budget;
            sc.max_loads_per_frame = config.streaming_max_loads_per_frame;
            sc.eviction_grace_frames = config.eviction_grace_frames;
            streaming_scheduler = create_streaming_scheduler(resource, sc);

            // Real async disk I/O path: serialize the resource to a temp
            // .vgeo and open it for pread() on a worker thread. Each page
            // load turns into an actual disk read. If anything fails here
            // we fall back to the latency-window simulation (async_io_active
            // stays false).
            try {
                const std::string unique =
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                temp_vgeo_path = std::filesystem::temp_directory_path() /
                                 (std::string("meridian-stream-") + unique + ".vgeo");
                write_resource(resource, temp_vgeo_path);
                const VGeoPayloadOffsets payload_offsets = compute_payload_offsets(resource);
                for (uint32_t p = 0; p < resource.pages.size(); ++p) {
                    const PageRecord& page = resource.pages[p];
                    const uint64_t base = (page.lod_cluster_count != 0)
                                              ? payload_offsets.lod_geometry_payload_offset
                                              : payload_offsets.cluster_geometry_payload_offset;
                    page_file_ranges[p] = {base + page.byte_offset, page.uncompressed_byte_size};
                }
                async_io_active = async_reader.open(temp_vgeo_path);
                if (!async_io_active) {
                    std::fprintf(stderr,
                                 "MERIDIAN_STREAM: async_reader.open failed, "
                                 "falling back to latency simulation\n");
                }
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "MERIDIAN_STREAM: temp .vgeo write failed (%s), "
                             "falling back to latency simulation\n", e.what());
                async_io_active = false;
            }
        }
        snapshot_page_residency(report.uploadable_scene, residency_model);
        result = update_vector_buffer(device, report.uploadable_scene.page_residency,
                                      scene_buffers.page_residency);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "initial page residency upload failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        const std::vector<uint8_t> initial_resident_pages = build_resident_page_mask(residency_model);
        const TraversalSelection initial_selection =
            simulate_traversal(resource, config.debug_error_threshold, initial_resident_pages);
        report.runtime_missing_page_count =
            static_cast<uint32_t>(initial_selection.missing_page_indices.size());
        report.runtime_prefetch_page_count =
            static_cast<uint32_t>(initial_selection.prefetch_page_indices.size());
        report.runtime_resident_page_count = count_resident_pages(residency_model);
        report.replay_runtime_parity =
            report.debug_selected_node_count == report.replay_selected_node_count &&
            report.debug_rendered_cluster_count == report.replay_selected_cluster_count &&
            report.debug_rendered_lod_cluster_count == report.replay_selected_lod_cluster_count;

        result = create_debug_render_context(selection.physical_device, device, swapchain,
                                             scene_buffers, compute_selection.draw_list,
                                             debug_render);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_debug_render_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }
        report.debug_pipeline_created = true;
        report.debug_geometry_uploaded = debug_render.descriptor_set != VK_NULL_HANDLE;
        report.visibility_attachment_created = debug_render.visibility_view != VK_NULL_HANDLE;

        result = create_hzb_context(selection.physical_device, device,
                                    swapchain.extent.width, swapchain.extent.height,
                                    debug_render.depth_view, debug_render.depth_format, hzb);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_hzb_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        result = create_occlusion_refine_context(selection.physical_device, device,
                                                  compute_selection, scene_buffers, hzb,
                                                  total_clusters, occlusion_refine);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_occlusion_refine_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        result = create_shadow_context(selection.physical_device, device, scene_buffers,
                                       debug_render.frame_ubo, compute_selection.max_draws,
                                       resource, 2048, shadow);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_shadow_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        // Bind cascaded shadow map (2D array) to main pass descriptor set binding 3
        if (shadow.depth_array_view != VK_NULL_HANDLE && shadow.sampler != VK_NULL_HANDLE) {
            VkDescriptorImageInfo shadow_img_info{};
            shadow_img_info.sampler = shadow.sampler;
            shadow_img_info.imageView = shadow.depth_array_view;
            shadow_img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet shadow_write{};
            shadow_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            shadow_write.dstSet = debug_render.descriptor_set;
            shadow_write.dstBinding = 3;
            shadow_write.descriptorCount = 1;
            shadow_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            shadow_write.pImageInfo = &shadow_img_info;
            vkUpdateDescriptorSets(device, 1, &shadow_write, 0, nullptr);
        }

        update_debug_selection_report(initial_selection, report.uploadable_scene, report);
        report.replay_runtime_parity =
            report.debug_selected_node_count == report.replay_selected_node_count &&
            report.debug_rendered_cluster_count == report.replay_selected_cluster_count &&
            report.debug_rendered_lod_cluster_count == report.replay_selected_lod_cluster_count;

        result = create_frame_context(device, selection.queues, frame);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_frame_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        for (uint32_t frame_index = 1;
             config.interactive ? (glfwWindowShouldClose(window) != GLFW_TRUE)
                                : (frame_index < config.present_frame_count);
             ++frame_index) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window) == GLFW_TRUE) {
                break;
            }

            // Interactive camera update
            if (config.interactive) {
                const double now = glfwGetTime();
                const float dt = static_cast<float>(now - last_frame_time);
                last_frame_time = now;

                // Mouse look
                double cx, cy;
                glfwGetCursorPos(window, &cx, &cy);
                if (interactive_cam.cursor_captured) {
                    const float dx = static_cast<float>(cx - interactive_cam.last_cursor_x);
                    const float dy = static_cast<float>(cy - interactive_cam.last_cursor_y);
                    interactive_cam.yaw += dx * interactive_cam.mouse_sensitivity;
                    interactive_cam.pitch -= dy * interactive_cam.mouse_sensitivity;
                    interactive_cam.pitch = std::max(-1.5f, std::min(1.5f, interactive_cam.pitch));
                }
                interactive_cam.last_cursor_x = cx;
                interactive_cam.last_cursor_y = cy;

                // WASD movement
                const Vec3f fwd = camera_forward(interactive_cam);
                const Vec3f rgt = camera_right(interactive_cam);
                const float spd = interactive_cam.move_speed * dt;
                if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
                    interactive_cam.position.x += fwd.x * spd;
                    interactive_cam.position.y += fwd.y * spd;
                    interactive_cam.position.z += fwd.z * spd;
                }
                if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
                    interactive_cam.position.x -= fwd.x * spd;
                    interactive_cam.position.y -= fwd.y * spd;
                    interactive_cam.position.z -= fwd.z * spd;
                }
                if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
                    interactive_cam.position.x -= rgt.x * spd;
                    interactive_cam.position.z -= rgt.z * spd;
                }
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                    interactive_cam.position.x += rgt.x * spd;
                    interactive_cam.position.z += rgt.z * spd;
                }
                if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
                    interactive_cam.position.y -= spd;
                }
                if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
                    interactive_cam.position.y += spd;
                }
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }

                // Recompute camera VP
                const Vec3f target = {interactive_cam.position.x + fwd.x,
                                      interactive_cam.position.y + fwd.y,
                                      interactive_cam.position.z + fwd.z};
                const float aspect = std::max(1.0f, static_cast<float>(swapchain.extent.width)) /
                                     std::max(1.0f, static_cast<float>(swapchain.extent.height));
                const Mat4f view = look_at_matrix(interactive_cam.position, target, {0.0f, 1.0f, 0.0f});
                const float radius = std::max({
                    resource.bounds.max.x - resource.bounds.min.x,
                    resource.bounds.max.y - resource.bounds.min.y,
                    resource.bounds.max.z - resource.bounds.min.z, 1.0f});
                const Mat4f proj = perspective_matrix(55.0f * 3.14159265f / 180.0f, aspect,
                                                      std::max(0.01f, radius * 0.01f), radius * 8.0f);
                camera_frame.view_projection = multiply_matrix(proj, view);
                camera_frame.camera_position = interactive_cam.position;

                fps_frame_count++;
                if (now - fps_timer >= 1.0) {
                    char title[256];
                    const uint32_t total_pages = static_cast<uint32_t>(residency_model.pages.size());
                    const uint32_t res_pages = count_resident_pages(residency_model);
                    std::snprintf(title, sizeof(title),
                                  "Meridian - %u FPS - %u draws - pages %u/%u",
                                  fps_frame_count, gpu_draw_count, res_pages, total_pages);
                    glfwSetWindowTitle(window, title);
                    fps_frame_count = 0;
                    fps_timer = now;
                }
            }

            // Frame timing (common path)
            {
                const double frame_now = glfwGetTime();
                if (frame_index > 2) {
                    frame_times_ms.push_back((frame_now - last_frame_time) * 1000.0);
                }
                last_frame_time = frame_now;
            }

            // Async-load completion. Two paths:
            //   * Real async I/O (async_io_active): drain completions from
            //     the worker thread that just finished pread()ing the page's
            //     byte range from the temp .vgeo. Completion time reflects
            //     actual disk latency + worker scheduling.
            //   * Simulated (fallback): a page that entered the loading state
            //     streaming_load_latency_frames ago now becomes resident.
            //   * Non-streaming default: complete_loading_pages transitions
            //     every loading page to resident instantly.
            std::vector<uint32_t> completed_this_frame;
            if (config.demand_streaming) {
                if (async_io_active) {
                    const auto reads = async_reader.drain_completions();
                    for (const auto& r : reads) {
                        if (!r.success) continue;
                        if (r.page_index >= residency_model.pages.size()) continue;
                        if (residency_model.pages[r.page_index].state !=
                            PageResidencyState::loading) continue;
                        completed_this_frame.push_back(r.page_index);
                        page_load_start_frame[r.page_index] = 0xffffffffu;
                    }
                } else {
                    for (uint32_t p = 0; p < residency_model.pages.size(); ++p) {
                        if (residency_model.pages[p].state == PageResidencyState::loading &&
                            page_load_start_frame[p] != 0xffffffffu &&
                            frame_index - page_load_start_frame[p] >=
                                config.streaming_load_latency_frames) {
                            completed_this_frame.push_back(p);
                            page_load_start_frame[p] = 0xffffffffu;
                        }
                    }
                }
                report.runtime_completed_page_count =
                    static_cast<uint32_t>(completed_this_frame.size());
            } else {
                report.runtime_completed_page_count =
                    complete_loading_pages(residency_model, frame_index);
            }

            using clock_t = std::chrono::steady_clock;
            auto t_traverse_start = clock_t::now();
            const std::vector<uint8_t> resident_pages = build_resident_page_mask(residency_model);
            const TraversalSelection selection_for_frame =
                simulate_traversal(resource, config.debug_error_threshold, resident_pages);
            auto t_traverse_end = clock_t::now();
            static double acc_traverse_ms = 0.0;
            static double acc_build_ms = 0.0;
            static double acc_upload_ms = 0.0;
            static double acc_residency_ms = 0.0;
            static double acc_cmdrec_ms = 0.0;
            static double acc_submit_ms = 0.0;
            static uint32_t cpu_prof_samples = 0;
            acc_traverse_ms +=
                std::chrono::duration<double, std::milli>(t_traverse_end - t_traverse_start).count();

            ResidencyUpdateInput residency_input;
            residency_input.frame_index = frame_index;
            residency_input.resident_budget = config.resident_budget;
            residency_input.eviction_grace_frames = config.eviction_grace_frames;
            residency_input.selected_pages = selection_for_frame.selected_page_indices;
            if (config.demand_streaming) {
                // Run the scheduler against the current selection to get a
                // throttled load queue (cap max_loads_per_frame) and evict
                // queue (oldest zero-priority pages when over budget). The
                // scheduler's own ResidencyUpdateInput return value sets the
                // frame/budget fields; override missing/prefetch with the
                // throttled queue so step_residency requests only those.
                ResidencyUpdateInput sched_in = update_streaming_scheduler(
                    streaming_scheduler, residency_model, selection_for_frame, frame_index);
                residency_input.missing_pages = streaming_scheduler.load_queue;
                residency_input.prefetch_pages.clear(); // load_queue already covers prefetch priority
                residency_input.completed_pages = std::move(completed_this_frame);
                // Explicit eviction: step_residency does not take an evict
                // list, so transition the scheduler-selected pages directly.
                for (uint32_t p : streaming_scheduler.evict_queue) {
                    if (p < residency_model.pages.size()) {
                        residency_model.pages[p].state = PageResidencyState::unloaded;
                    }
                }
                (void)sched_in; // we use the scheduler state directly.
            } else {
                residency_input.missing_pages = selection_for_frame.missing_page_indices;
                residency_input.prefetch_pages = selection_for_frame.prefetch_page_indices;
            }
            auto t_residency_start = clock_t::now();
            const ResidencyUpdateResult residency_update =
                step_residency(residency_model, residency_input);
            if (config.demand_streaming) {
                // Any page that step_residency advanced to `loading` this
                // frame needs its load-start timestamp recorded (for the
                // simulation fallback) AND its real pread submitted to the
                // worker thread (for the async I/O path). The scheduler's
                // throttle guarantees we won't spam the worker.
                for (uint32_t p : residency_update.loading_pages) {
                    if (p < page_load_start_frame.size()) {
                        page_load_start_frame[p] = frame_index;
                    }
                    if (async_io_active && p < page_file_ranges.size() &&
                        page_file_ranges[p].size > 0) {
                        AsyncReadJob job{};
                        job.offset = page_file_ranges[p].offset;
                        job.size = page_file_ranges[p].size;
                        job.page_index = p;
                        async_reader.submit(job);
                    }
                }
            }

            snapshot_page_residency(report.uploadable_scene, residency_model);
            result = update_vector_buffer(device, report.uploadable_scene.page_residency,
                                          scene_buffers.page_residency);
            auto t_residency_end = clock_t::now();
            acc_residency_ms +=
                std::chrono::duration<double, std::milli>(t_residency_end - t_residency_start).count();
            if (result != VK_SUCCESS) {
                std::ostringstream message;
                message << "page residency update failed with code " << result;
                report.status = message.str();
                cleanup();
                return report;
            }

            update_debug_selection_report(selection_for_frame, report.uploadable_scene, report);
            report.runtime_missing_page_count =
                static_cast<uint32_t>(selection_for_frame.missing_page_indices.size());
            report.runtime_prefetch_page_count =
                static_cast<uint32_t>(selection_for_frame.prefetch_page_indices.size());
            report.runtime_requested_page_count =
                static_cast<uint32_t>(residency_update.requested_pages.size());
            report.runtime_loading_page_count =
                static_cast<uint32_t>(residency_update.loading_pages.size());
            report.runtime_resident_page_count = count_resident_pages(residency_model);

            report.replay_runtime_parity =
                report.debug_selected_node_count == report.replay_selected_node_count &&
                report.debug_rendered_cluster_count == report.replay_selected_cluster_count &&
                report.debug_rendered_lod_cluster_count == report.replay_selected_lod_cluster_count;

            auto t_fence_start = clock_t::now();
            vkWaitForFences(device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
            auto t_fence_end = clock_t::now();
            static double acc_fence_ms = 0.0;
            static double acc_present_ms = 0.0;
            acc_fence_ms +=
                std::chrono::duration<double, std::milli>(t_fence_end - t_fence_start).count();
            if (report.presented_frame_count > 0) {
                analyze_visibility_readback(device, swapchain, debug_render, last_submitted_selection,
                                           report);
                // Read back GPU draw count for debug stats (draws are consumed on GPU via indirect)
                const UploadedBuffer& readback_count_buf = compute_selection.draw_count;
                if (readback_count_buf.buffer != VK_NULL_HANDLE) {
                    void* count_mapped = nullptr;
                    if (vkMapMemory(device, readback_count_buf.memory, 0,
                                    sizeof(uint32_t), 0, &count_mapped) == VK_SUCCESS) {
                        gpu_draw_count = *static_cast<const uint32_t*>(count_mapped);
                        vkUnmapMemory(device, readback_count_buf.memory);
                    }
                }

                // GPU profiler readback: print timing every 60 frames in interactive mode,
                // or every frame in non-interactive mode
                if (gpu_profiler.query_pool != VK_NULL_HANDLE &&
                    (!config.interactive || (frame_index % 60) == 0)) {
                    auto timers = read_gpu_timers(device, gpu_profiler);
                    if (!timers.empty()) {
                        std::fprintf(stderr, "MERIDIAN_GPU:");
                        for (const auto& t : timers) {
                            std::fprintf(stderr, " %s=%.2fms", t.name.c_str(), t.ms);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            vkResetFences(device, 1, &frame.in_flight);
            vkResetCommandPool(device, frame.command_pool, 0);

            uint32_t image_index = 0;
            result = vkAcquireNextImageKHR(device, swapchain.swapchain, UINT64_MAX,
                                           frame.image_available, VK_NULL_HANDLE, &image_index);
            if (result != VK_SUCCESS) {
                std::ostringstream message;
                message << "vkAcquireNextImageKHR failed with code " << result;
                report.status = message.str();
                cleanup();
                return report;
            }

            // Compute cascaded shadow light view-projection matrices for the
            // current camera. Using the same camera projection parameters
            // (fov, aspect, near, far) that feed view_projection above.
            const Vec3f norm_light = normalize_vec3({0.4f, 0.7f, 0.5f});
            {
                const float aspect = std::max(1.0f,
                    static_cast<float>(swapchain.extent.width)) /
                    std::max(1.0f, static_cast<float>(swapchain.extent.height));
                const float fov = 55.0f * 3.14159265f / 180.0f;
                const float radius = std::max({
                    resource.bounds.max.x - resource.bounds.min.x,
                    resource.bounds.max.y - resource.bounds.min.y,
                    resource.bounds.max.z - resource.bounds.min.z, 1.0f});
                const float near_p = std::max(0.01f, radius * 0.01f);
                // Clamp the far plane for CSM so the cascades pack usefully
                // around the camera rather than stretching to the full
                // 8x-radius projection far, which would drown cascade 2 in
                // empty space for indoor scenes.
                const float far_p = radius * 3.0f;
                const Vec3f fwd = config.interactive
                                      ? camera_forward(interactive_cam)
                                      : normalize_vec3(subtract_vec3(
                                            {
                                                (resource.bounds.min.x + resource.bounds.max.x) * 0.5f,
                                                (resource.bounds.min.y + resource.bounds.max.y) * 0.5f,
                                                (resource.bounds.min.z + resource.bounds.max.z) * 0.5f,
                                            },
                                            camera_frame.camera_position));
                // World-space right/up derived from forward + world up.
                const Vec3f world_up = {0.0f, 1.0f, 0.0f};
                const Vec3f rgt = normalize_vec3(cross_vec3(fwd, world_up));
                const Vec3f up_corr = cross_vec3(rgt, fwd);
                shadow.cascades = compute_cascade_light_setup(
                    camera_frame.camera_position, fwd, rgt, up_corr,
                    fov, aspect, near_p, far_p, norm_light,
                    /*caster_extent=*/radius * 1.5f,
                    /*lambda=*/0.7f);
            }

            // Upload per-frame UBO (camera VP + 3 cascade light VPs + splits).
            FrameUBO frame_ubo_data{};
            std::memcpy(frame_ubo_data.view_projection, camera_frame.view_projection.m,
                        sizeof(frame_ubo_data.view_projection));
            for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
                std::memcpy(frame_ubo_data.light_vp[c], shadow.cascades.light_vp[c].m,
                            sizeof(frame_ubo_data.light_vp[c]));
                frame_ubo_data.cascade_splits[c] = shadow.cascades.splits[c];
            }
            frame_ubo_data.cascade_splits[3] = 0.0f;
            frame_ubo_data.light_dir[0] = norm_light.x;
            frame_ubo_data.light_dir[1] = norm_light.y;
            frame_ubo_data.light_dir[2] = norm_light.z;
            frame_ubo_data.light_dir[3] = 0.0f;
            update_uploaded_buffer(device, &frame_ubo_data, sizeof(FrameUBO),
                                   debug_render.frame_ubo);

            // Convert CPU TraversalSelection to GpuDrawEntry list and upload to the
            // same buffers the GPU selection shader used to populate. This replaces
            // the serial DFS compute dispatch (was ~18ms on 1M-tri city on M4) with
            // CPU traversal + HOST_COHERENT write (~1-3ms total).
            //
            // Filters applied here:
            //   1. Frustum AABB test (base + LOD) -- mirrors instance_cull but at
            //      cluster granularity. Assumes cluster bounds are world-space
            //      (single-instance / identity transform scenes).
            //   2. Normal-cone backface cull (base only, LOD clusters don't carry cones).
            const FrustumPlanes frustum =
                extract_frustum_planes(camera_frame.view_projection);
            auto aabb_outside_frustum = [&](const float bmin[4], const float bmax[4]) -> bool {
                for (int p = 0; p < 6; ++p) {
                    const float nx = frustum.planes[p][0];
                    const float ny = frustum.planes[p][1];
                    const float nz = frustum.planes[p][2];
                    const float d  = frustum.planes[p][3];
                    const float px = nx > 0.0f ? bmax[0] : bmin[0];
                    const float py = ny > 0.0f ? bmax[1] : bmin[1];
                    const float pz = nz > 0.0f ? bmax[2] : bmin[2];
                    if (nx * px + ny * py + nz * pz + d < 0.0f) return true;
                }
                return false;
            };
            // Per-cascade frustum planes, extracted from each cascade's
            // light view-projection. Used below to filter the main draw list
            // into per-cascade draw lists so the shadow pass only draws
            // clusters that actually overlap the cascade's volume.
            FrustumPlanes cascade_frusta[kShadowCascadeCount];
            for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
                cascade_frusta[c] = extract_frustum_planes(shadow.cascades.light_vp[c]);
            }
            auto aabb_outside_planes = [](const FrustumPlanes& fp,
                                          const float bmin[4],
                                          const float bmax[4]) -> bool {
                for (int p = 0; p < 6; ++p) {
                    const float nx = fp.planes[p][0];
                    const float ny = fp.planes[p][1];
                    const float nz = fp.planes[p][2];
                    const float d  = fp.planes[p][3];
                    const float px = nx > 0.0f ? bmax[0] : bmin[0];
                    const float py = ny > 0.0f ? bmax[1] : bmin[1];
                    const float pz = nz > 0.0f ? bmax[2] : bmin[2];
                    if (nx * px + ny * py + nz * pz + d < 0.0f) return true;
                }
                return false;
            };
            {
                auto t_build_start = clock_t::now();
                std::vector<GpuDrawEntry> cpu_draws;
                std::vector<GpuDrawEntry> cascade_draws[kShadowCascadeCount];
                cpu_draws.reserve(selection_for_frame.selected_cluster_indices.size() +
                                  selection_for_frame.selected_lod_cluster_indices.size());
                for (auto& v : cascade_draws) v.reserve(cpu_draws.capacity() / 2);
                const Vec3f cam = camera_frame.camera_position;
                auto push_to_cascades = [&](const GpuDrawEntry& entry,
                                             const float bmin[4], const float bmax[4]) {
                    for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
                        if (aabb_outside_planes(cascade_frusta[c], bmin, bmax)) continue;
                        GpuDrawEntry copy = entry;
                        // draw_first_instance is used as gl_InstanceIndex by
                        // the shadow.vert vertex pulling path; keep it in
                        // sync with the cascade-local slot so indexing stays
                        // contiguous within each cascade buffer.
                        copy.draw_first_instance = static_cast<uint32_t>(cascade_draws[c].size());
                        cascade_draws[c].push_back(copy);
                    }
                };
                for (const uint32_t ci : selection_for_frame.selected_cluster_indices) {
                    const GpuClusterRecord& c = report.uploadable_scene.clusters[ci];
                    // Cascade filtering uses the raw AABB only -- backface
                    // culling against the camera doesn't apply to shadow
                    // casters (a cluster facing away from the camera can
                    // still cast a shadow into the camera's view).
                    GpuDrawEntry cascade_entry{};
                    cascade_entry.draw_vertex_count = c.local_triangle_count * 3u;
                    cascade_entry.draw_instance_count = 1u;
                    cascade_entry.draw_first_vertex = 0u;
                    cascade_entry.cluster_index = ci;
                    cascade_entry.geometry_kind = 0u;
                    cascade_entry.payload_offset = c.payload_offset;
                    cascade_entry.local_vertex_count = c.local_vertex_count;
                    push_to_cascades(cascade_entry, c.bounds_min.data(), c.bounds_max.data());

                    // Main-pass entry has camera-frustum + normal-cone culls.
                    if (aabb_outside_frustum(c.bounds_min.data(), c.bounds_max.data())) continue;
                    // Normal-cone backface cull (mirrors shader is_base_cluster_backfacing).
                    // Cone packing: xyz = cone axis, w = cone cutoff. Cutoff >= 1.0 means
                    // meshoptimizer could not compute a useful cone; do not cull.
                    const float cone_cutoff = c.normal_cone[3];
                    if (cone_cutoff < 1.0f) {
                        const float cx = (c.bounds_min[0] + c.bounds_max[0]) * 0.5f;
                        const float cy = (c.bounds_min[1] + c.bounds_max[1]) * 0.5f;
                        const float cz = (c.bounds_min[2] + c.bounds_max[2]) * 0.5f;
                        float vx = cx - cam.x;
                        float vy = cy - cam.y;
                        float vz = cz - cam.z;
                        const float len = std::sqrt(vx * vx + vy * vy + vz * vz);
                        if (len > 1e-6f) {
                            const float inv = 1.0f / len;
                            vx *= inv; vy *= inv; vz *= inv;
                        }
                        const float d = vx * c.normal_cone[0] +
                                        vy * c.normal_cone[1] +
                                        vz * c.normal_cone[2];
                        if (d < -cone_cutoff) continue;
                    }
                    GpuDrawEntry e = cascade_entry;
                    e.draw_first_instance = static_cast<uint32_t>(cpu_draws.size());
                    cpu_draws.push_back(e);
                }
                for (const uint32_t ci : selection_for_frame.selected_lod_cluster_indices) {
                    const GpuLodClusterRecord& c = report.uploadable_scene.lod_clusters[ci];
                    GpuDrawEntry cascade_entry{};
                    cascade_entry.draw_vertex_count = c.local_triangle_count * 3u;
                    cascade_entry.draw_instance_count = 1u;
                    cascade_entry.draw_first_vertex = 0u;
                    cascade_entry.cluster_index = ci;
                    cascade_entry.geometry_kind = 1u;
                    cascade_entry.payload_offset = c.payload_offset;
                    cascade_entry.local_vertex_count = c.local_vertex_count;
                    push_to_cascades(cascade_entry, c.bounds_min.data(), c.bounds_max.data());

                    if (aabb_outside_frustum(c.bounds_min.data(), c.bounds_max.data())) continue;
                    GpuDrawEntry e = cascade_entry;
                    e.draw_first_instance = static_cast<uint32_t>(cpu_draws.size());
                    cpu_draws.push_back(e);
                }
                const uint32_t cpu_draw_count = static_cast<uint32_t>(cpu_draws.size());
                auto t_build_end = clock_t::now();
                acc_build_ms +=
                    std::chrono::duration<double, std::milli>(t_build_end - t_build_start).count();
                auto t_upload_start = clock_t::now();
                if (cpu_draw_count > 0 &&
                    compute_selection.draw_list.buffer != VK_NULL_HANDLE) {
                    update_uploaded_buffer(device, cpu_draws.data(),
                                           static_cast<VkDeviceSize>(cpu_draw_count) *
                                               sizeof(GpuDrawEntry),
                                           compute_selection.draw_list);
                }
                // Upload each cascade's draw list + count into its own
                // HOST_COHERENT buffer; the shadow pass reads them per pass.
                for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
                    const uint32_t ccount = static_cast<uint32_t>(cascade_draws[c].size());
                    if (ccount > 0 && shadow.cascade_draw_lists[c].buffer != VK_NULL_HANDLE) {
                        update_uploaded_buffer(device, cascade_draws[c].data(),
                                               static_cast<VkDeviceSize>(ccount) *
                                                   sizeof(GpuDrawEntry),
                                               shadow.cascade_draw_lists[c]);
                    }
                    if (shadow.cascade_draw_counts[c].buffer != VK_NULL_HANDLE) {
                        update_uploaded_buffer(device, &ccount, sizeof(uint32_t),
                                               shadow.cascade_draw_counts[c]);
                    }
                }
                if (compute_selection.draw_count.buffer != VK_NULL_HANDLE) {
                    update_uploaded_buffer(device, &cpu_draw_count, sizeof(uint32_t),
                                           compute_selection.draw_count);
                }
                auto t_upload_end = clock_t::now();
                acc_upload_ms +=
                    std::chrono::duration<double, std::milli>(t_upload_end - t_upload_start).count();
            }

            // frustum was already extracted above for cluster-level CPU culling
            auto t_cmdrec_start = clock_t::now();
            result = record_debug_command_buffer(frame, debug_render, compute_cull, compute_selection,
                                                 hzb, occlusion_refine, shadow, swapchain,
                                                 camera_frame, frustum, config.debug_error_threshold,
                                                 selection_for_frame, report.uploadable_scene,
                                                 frame_index, image_index,
                                                 has_draw_indirect_count, gpu_profiler);
            auto t_cmdrec_end = clock_t::now();
            acc_cmdrec_ms +=
                std::chrono::duration<double, std::milli>(t_cmdrec_end - t_cmdrec_start).count();
            if (result != VK_SUCCESS) {
                std::ostringstream message;
                message << "record_debug_command_buffer failed with code " << result;
                report.status = message.str();
                cleanup();
                return report;
            }

            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = &frame.image_available;
            submit_info.pWaitDstStageMask = &wait_stage;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &frame.command_buffer;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &frame.render_finished;

            auto t_submit_start = clock_t::now();
            result = vkQueueSubmit(graphics_queue, 1, &submit_info, frame.in_flight);
            auto t_submit_end = clock_t::now();
            acc_submit_ms +=
                std::chrono::duration<double, std::milli>(t_submit_end - t_submit_start).count();
            cpu_prof_samples++;
            if (cpu_prof_samples % 60 == 0) {
                std::fprintf(stderr,
                    "MERIDIAN_CPU: traverse=%.2f residency=%.2f build=%.2f upload=%.2f cmdrec=%.2f submit=%.2f fence=%.2f present=%.2f (ms/frame, n=%u)\n",
                    acc_traverse_ms / cpu_prof_samples,
                    acc_residency_ms / cpu_prof_samples,
                    acc_build_ms / cpu_prof_samples,
                    acc_upload_ms / cpu_prof_samples,
                    acc_cmdrec_ms / cpu_prof_samples,
                    acc_submit_ms / cpu_prof_samples,
                    acc_fence_ms / cpu_prof_samples,
                    acc_present_ms / cpu_prof_samples,
                    cpu_prof_samples);
            }
            if (result != VK_SUCCESS) {
                std::ostringstream message;
                message << "vkQueueSubmit failed with code " << result;
                report.status = message.str();
                cleanup();
                return report;
            }

            VkPresentInfoKHR present_info{};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &frame.render_finished;
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain.swapchain;
            present_info.pImageIndices = &image_index;

            auto t_present_start = clock_t::now();
            result = vkQueuePresentKHR(present_queue, &present_info);
            auto t_present_end = clock_t::now();
            acc_present_ms +=
                std::chrono::duration<double, std::milli>(t_present_end - t_present_start).count();
            if (result != VK_SUCCESS) {
                std::ostringstream message;
                message << "vkQueuePresentKHR failed with code " << result;
                report.status = message.str();
                cleanup();
                return report;
            }

            last_submitted_selection = selection_for_frame;
            report.presented_frame_count += 1;
            report.debug_draw_submitted = true;
        }

        vkDeviceWaitIdle(device);
        // Tear down the async reader and remove the temp .vgeo before the
        // visibility readback / cleanup path runs. Closing here instead of
        // in `cleanup` keeps the reader local to the streaming block where
        // it lives.
        if (async_io_active) {
            async_reader.close();
            async_io_active = false;
            std::error_code ec;
            std::filesystem::remove(temp_vgeo_path, ec);
        }
        if (report.presented_frame_count > 0) {
            analyze_visibility_readback(device, swapchain, debug_render, last_submitted_selection,
                                       report);
        }
        report.present_loop_completed = report.presented_frame_count == config.present_frame_count;

        // Frame timing report
        if (!frame_times_ms.empty()) {
            std::sort(frame_times_ms.begin(), frame_times_ms.end());
            const double median = frame_times_ms[frame_times_ms.size() / 2];
            const double p99 = frame_times_ms[static_cast<size_t>(frame_times_ms.size() * 0.99)];
            const double avg = std::accumulate(frame_times_ms.begin(), frame_times_ms.end(), 0.0)
                               / static_cast<double>(frame_times_ms.size());
            std::fprintf(stderr, "MERIDIAN_BENCHMARK: median_ms=%.2f p99_ms=%.2f avg_ms=%.2f avg_fps=%.1f samples=%zu\n",
                         median, p99, avg, 1000.0 / avg, frame_times_ms.size());
        }
        if (compute_cull.counter.buffer != VK_NULL_HANDLE) {
            void* mapped = nullptr;
            if (vkMapMemory(device, compute_cull.counter.memory, 0, sizeof(uint32_t), 0, &mapped) == VK_SUCCESS) {
                report.compute_cull_visible_instances = *static_cast<const uint32_t*>(mapped);
                vkUnmapMemory(device, compute_cull.counter.memory);
            }
        }
        if (compute_selection.draw_count.buffer != VK_NULL_HANDLE) {
            void* mapped = nullptr;
            if (vkMapMemory(device, compute_selection.draw_count.memory, 0, sizeof(uint32_t), 0, &mapped) == VK_SUCCESS) {
                report.compute_selection_draw_count = *static_cast<const uint32_t*>(mapped);
                vkUnmapMemory(device, compute_selection.draw_count.memory);
            }
        }
        if (occlusion_refine.output_count.buffer != VK_NULL_HANDLE) {
            void* mapped = nullptr;
            if (vkMapMemory(device, occlusion_refine.output_count.memory, 0, sizeof(uint32_t), 0, &mapped) == VK_SUCCESS) {
                report.compute_occlusion_surviving_draws = *static_cast<const uint32_t*>(mapped);
                vkUnmapMemory(device, occlusion_refine.output_count.memory);
            }
        }
        // Screenshot capture
        if (!config.screenshot_path.empty() && report.presented_frame_count > 0 &&
            !swapchain.images.empty() && frame.command_pool != VK_NULL_HANDLE) {
            const uint32_t w = swapchain.extent.width;
            const uint32_t h = swapchain.extent.height;
            const VkDeviceSize pixel_size = 4; // BGRA
            const VkDeviceSize buf_size = w * h * pixel_size;

            UploadedBuffer readback{};
            if (create_uploaded_buffer(selection.physical_device, device, nullptr, buf_size,
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback) == VK_SUCCESS) {
                vkResetCommandPool(device, frame.command_pool, 0);
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(frame.command_buffer, &begin);

                // Transition swapchain image to transfer src
                VkImageMemoryBarrier to_src{};
                to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_src.image = swapchain.images.back();
                to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                     1, &to_src);

                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer(frame.command_buffer, swapchain.images.back(),
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);

                vkEndCommandBuffer(frame.command_buffer);
                VkSubmitInfo sub{};
                sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                sub.commandBufferCount = 1;
                sub.pCommandBuffers = &frame.command_buffer;
                vkQueueSubmit(graphics_queue, 1, &sub, VK_NULL_HANDLE);
                vkQueueWaitIdle(graphics_queue);

                void* mapped = nullptr;
                if (vkMapMemory(device, readback.memory, 0, buf_size, 0, &mapped) == VK_SUCCESS) {
                    const uint8_t* pixels = static_cast<const uint8_t*>(mapped);
                    std::ofstream ppm(config.screenshot_path, std::ios::binary);
                    if (ppm) {
                        ppm << "P6\n" << w << " " << h << "\n255\n";
                        for (uint32_t i = 0; i < w * h; ++i) {
                            // BGRA -> RGB
                            ppm.put(static_cast<char>(pixels[i * 4 + 2]));
                            ppm.put(static_cast<char>(pixels[i * 4 + 1]));
                            ppm.put(static_cast<char>(pixels[i * 4 + 0]));
                        }
                    }
                    vkUnmapMemory(device, readback.memory);
                }
                destroy_uploaded_buffer(device, readback);
            }
        }

        report.status = report.present_loop_completed ? "window, surface, device, and swapchain created successfully"
                                                     : "bootstrap completed but present loop ended early";
        cleanup();
        return report;
    } catch (const std::exception& error) {
        report.status = error.what();
        cleanup();
        return report;
    }
#endif
}

}  // namespace meridian
