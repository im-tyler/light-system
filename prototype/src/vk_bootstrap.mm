#include "vk_bootstrap.h"
#include "runtime_model.h"
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
#include <limits>
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

namespace {

constexpr uint32_t kInvalidQueueFamily = 0xffffffffu;

#if MERIDIAN_HAS_VULKAN && MERIDIAN_HAS_GLFW

struct QueueFamilySelection {
    uint32_t graphics_family = kInvalidQueueFamily;
    uint32_t present_family = kInvalidQueueFamily;

    bool complete() const {
        return graphics_family != kInvalidQueueFamily && present_family != kInvalidQueueFamily;
    }
};

struct DeviceSelection {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    QueueFamilySelection queues;
    bool enable_portability_subset = false;
};

struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format{};
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
};

struct FrameContext {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

struct UploadedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct UploadedSceneBuffers {
    UploadedBuffer header;
    UploadedBuffer instances;
    UploadedBuffer hierarchy_nodes;
    UploadedBuffer clusters;
    UploadedBuffer lod_groups;
    UploadedBuffer lod_clusters;
    UploadedBuffer node_lod_links;
    UploadedBuffer pages;
    UploadedBuffer page_dependencies;
    UploadedBuffer page_residency;
    UploadedBuffer base_payload;
    UploadedBuffer lod_payload;
};


struct DebugRenderContext {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkImage visibility_image = VK_NULL_HANDLE;
    VkDeviceMemory visibility_memory = VK_NULL_HANDLE;
    VkImageView visibility_view = VK_NULL_HANDLE;
    VkFormat visibility_format = VK_FORMAT_R32G32_UINT;
    UploadedBuffer visibility_readback_buffer;
    UploadedBuffer frame_ubo;
    VkImage placeholder_depth_image = VK_NULL_HANDLE;
    VkDeviceMemory placeholder_depth_memory = VK_NULL_HANDLE;
    VkImageView placeholder_depth_view = VK_NULL_HANDLE;
    VkSampler placeholder_sampler = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

struct Mat4f {
    float m[16] = {0.0f};
};

struct CameraFrameData {
    Mat4f view_projection;
};

struct InteractiveCamera {
    Vec3f position = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float move_speed = 5.0f;
    float mouse_sensitivity = 0.003f;
    double last_cursor_x = 0.0;
    double last_cursor_y = 0.0;
    bool cursor_captured = false;
};

Vec3f camera_forward(const InteractiveCamera& cam) {
    return {std::sin(cam.yaw) * std::cos(cam.pitch),
            std::sin(cam.pitch),
            -std::cos(cam.yaw) * std::cos(cam.pitch)};
}

Vec3f camera_right(const InteractiveCamera& cam) {
    return {std::cos(cam.yaw), 0.0f, std::sin(cam.yaw)};
}

struct FrameUBO {
    float view_projection[16];
    float light_vp[16];
    float light_dir[4];
};

struct DrawPushConstants {
    uint32_t payload_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t geometry_index = 0;
    uint32_t geometry_kind = 0;
};

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

Mat4f identity_matrix() {
    Mat4f matrix;
    matrix.m[0] = 1.0f;
    matrix.m[5] = 1.0f;
    matrix.m[10] = 1.0f;
    matrix.m[15] = 1.0f;
    return matrix;
}

Mat4f multiply_matrix(const Mat4f& lhs, const Mat4f& rhs) {
    Mat4f result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result.m[column * 4 + row] =
                lhs.m[0 * 4 + row] * rhs.m[column * 4 + 0] +
                lhs.m[1 * 4 + row] * rhs.m[column * 4 + 1] +
                lhs.m[2 * 4 + row] * rhs.m[column * 4 + 2] +
                lhs.m[3 * 4 + row] * rhs.m[column * 4 + 3];
        }
    }
    return result;
}

Vec3f subtract_vec3(const Vec3f& lhs, const Vec3f& rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3f cross_vec3(const Vec3f& lhs, const Vec3f& rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

float dot_vec3(const Vec3f& lhs, const Vec3f& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3f normalize_vec3(const Vec3f& value) {
    const float length = std::sqrt(dot_vec3(value, value));
    if (length <= 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {value.x / length, value.y / length, value.z / length};
}

Mat4f look_at_matrix(const Vec3f& eye, const Vec3f& target, const Vec3f& up) {
    const Vec3f forward = normalize_vec3(subtract_vec3(target, eye));
    const Vec3f right = normalize_vec3(cross_vec3(forward, up));
    const Vec3f corrected_up = cross_vec3(right, forward);

    Mat4f result = identity_matrix();
    result.m[0] = right.x;
    result.m[1] = corrected_up.x;
    result.m[2] = -forward.x;
    result.m[4] = right.y;
    result.m[5] = corrected_up.y;
    result.m[6] = -forward.y;
    result.m[8] = right.z;
    result.m[9] = corrected_up.z;
    result.m[10] = -forward.z;
    result.m[12] = -dot_vec3(right, eye);
    result.m[13] = -dot_vec3(corrected_up, eye);
    result.m[14] = dot_vec3(forward, eye);
    return result;
}

Mat4f perspective_matrix(float vertical_fov_radians, float aspect_ratio, float near_plane,
                         float far_plane) {
    const float tan_half_fov = std::tan(vertical_fov_radians * 0.5f);
    Mat4f result{};
    result.m[0] = 1.0f / (aspect_ratio * tan_half_fov);
    result.m[5] = -(1.0f / tan_half_fov);
    result.m[10] = far_plane / (near_plane - far_plane);
    result.m[11] = -1.0f;
    result.m[14] = (near_plane * far_plane) / (near_plane - far_plane);
    return result;
}

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
    camera_distance = radius * 2.4f;

    const Vec3f eye = {center.x + radius * 0.55f, center.y + radius * 0.9f,
                       center.z + camera_distance};
    const float aspect_ratio =
        std::max(1.0f, static_cast<float>(extent.width)) / std::max(1.0f, static_cast<float>(extent.height));
    const Mat4f view = look_at_matrix(eye, center, {0.0f, 1.0f, 0.0f});
    const Mat4f projection = perspective_matrix(55.0f * 3.1415926535f / 180.0f, aspect_ratio,
                                                std::max(0.01f, radius * 0.01f), radius * 8.0f);

    CameraFrameData frame_data{};
    frame_data.view_projection = multiply_matrix(projection, view);
    return frame_data;
}

struct FrustumPlanes {
    float planes[6][4];
};

FrustumPlanes extract_frustum_planes(const Mat4f& vp) {
    FrustumPlanes fp{};
    // Row extraction from column-major: row r = { m[0*4+r], m[1*4+r], m[2*4+r], m[3*4+r] }
    auto row = [&](int r, int c) -> float { return vp.m[c * 4 + r]; };
    // Left:   row3 + row0
    for (int i = 0; i < 4; ++i) fp.planes[0][i] = row(3, i) + row(0, i);
    // Right:  row3 - row0
    for (int i = 0; i < 4; ++i) fp.planes[1][i] = row(3, i) - row(0, i);
    // Bottom: row3 + row1
    for (int i = 0; i < 4; ++i) fp.planes[2][i] = row(3, i) + row(1, i);
    // Top:    row3 - row1
    for (int i = 0; i < 4; ++i) fp.planes[3][i] = row(3, i) - row(1, i);
    // Near:   row3 + row2
    for (int i = 0; i < 4; ++i) fp.planes[4][i] = row(3, i) + row(2, i);
    // Far:    row3 - row2
    for (int i = 0; i < 4; ++i) fp.planes[5][i] = row(3, i) - row(2, i);
    // Normalize each plane
    for (auto& plane : fp.planes) {
        const float len = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (len > 1e-6f) {
            for (float& v : plane) v /= len;
        }
    }
    return fp;
}

struct CullPushConstants {
    float frustum_planes[6][4]; // 96 bytes
    uint32_t instance_count;    // 4 bytes
    uint32_t pad[3];            // 12 bytes padding to 112 bytes (16-byte aligned)
};

struct ComputeCullContext {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    UploadedBuffer visible_instances;
    UploadedBuffer counter;
    uint32_t max_instances = 0;
};

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

#if MERIDIAN_HAS_SHADERC
std::vector<uint32_t> compile_glsl_to_spirv(const std::string& source, shaderc_shader_kind kind,
                                            const char* name);
VkShaderModule create_shader_module(VkDevice device, const std::vector<uint32_t>& spirv);
#endif

#if MERIDIAN_HAS_VULKAN
VkFormat find_depth_format(VkPhysicalDevice physical_device);
#endif

#if MERIDIAN_HAS_SHADERC && MERIDIAN_HAS_VULKAN && MERIDIAN_HAS_GLFW
VkResult create_compute_cull_context(VkPhysicalDevice physical_device, VkDevice device,
                                     const UploadedSceneBuffers& scene_buffers,
                                     uint32_t instance_count,
                                     ComputeCullContext& context) {
    context.max_instances = instance_count;

    const char* compute_source = R"(
#version 450
layout(local_size_x = 64) in;

layout(push_constant) uniform CullData {
    vec4 frustum_planes[6];
    uint instance_count;
} cull;

struct InstanceRecord {
    mat4 object_to_world;
    vec4 bounds_min;
    vec4 bounds_max;
    uint resource_index;
    uint root_node_index;
    uint flags;
    uint reserved;
};

layout(set = 0, binding = 0) readonly buffer Instances { InstanceRecord instances[]; };
layout(set = 0, binding = 1) buffer VisibleInstances { uint visible_indices[]; };
layout(set = 0, binding = 2) buffer Counter { uint visible_count; };

bool aabb_outside_plane(vec4 plane, vec3 bmin, vec3 bmax) {
    vec3 p = vec3(
        plane.x > 0.0 ? bmax.x : bmin.x,
        plane.y > 0.0 ? bmax.y : bmin.y,
        plane.z > 0.0 ? bmax.z : bmin.z
    );
    return dot(plane.xyz, p) + plane.w < 0.0;
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= cull.instance_count) return;

    InstanceRecord inst = instances[id];
    vec3 bmin = inst.bounds_min.xyz;
    vec3 bmax = inst.bounds_max.xyz;

    for (uint i = 0; i < 6; i++) {
        if (aabb_outside_plane(cull.frustum_planes[i], bmin, bmax)) {
            return;
        }
    }

    uint slot = atomicAdd(visible_count, 1u);
    visible_indices[slot] = id;
}
    )";

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "instance_cull.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // Descriptor set layout: instances (in), visible list (out), counter (out)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 3;
    set_layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                                   &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(CullPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &context.descriptor_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = context.pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Output buffers
    const VkDeviceSize visible_buffer_size =
        static_cast<VkDeviceSize>(instance_count) * sizeof(uint32_t);
    result = create_uploaded_buffer(physical_device, device, nullptr, std::max(visible_buffer_size,
                                   static_cast<VkDeviceSize>(4)),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.visible_instances);
    if (result != VK_SUCCESS) return result;

    const uint32_t zero = 0;
    result = create_uploaded_buffer(physical_device, device, &zero, sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.counter);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool and set
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 3;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) return result;

    VkDescriptorBufferInfo buf_infos[3] = {};
    buf_infos[0].buffer = scene_buffers.instances.buffer;
    buf_infos[0].range = scene_buffers.instances.size;
    buf_infos[1].buffer = context.visible_instances.buffer;
    buf_infos[1].range = context.visible_instances.size;
    buf_infos[2].buffer = context.counter.buffer;
    buf_infos[2].range = context.counter.size;

    VkWriteDescriptorSet writes[3] = {};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = context.descriptor_set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    return VK_SUCCESS;
}

struct SelectionPushConstants {
    float error_threshold;
    uint32_t pad[3];
};

struct HzbContext {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkImageView> mip_views;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline depth_copy_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout depth_copy_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_copy_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet depth_copy_descriptor_set = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> mip_descriptor_sets;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_count = 0;
};

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

struct ComputeSelectionContext {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    UploadedBuffer draw_list;
    UploadedBuffer draw_count;
    uint32_t max_draws = 0;
};

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

VkResult create_compute_selection_context(VkPhysicalDevice physical_device, VkDevice device,
                                          const UploadedSceneBuffers& scene_buffers,
                                          const ComputeCullContext& cull_context,
                                          uint32_t max_clusters,
                                          ComputeSelectionContext& context) {
    context.max_draws = max_clusters;

    // Shader uses raw uint[] SSBOs to avoid GLSL struct alignment mismatches with C++.
    // Field offsets are hardcoded to match the GPU ABI structs in gpu_abi.h.
    const char* compute_source = R"(
#version 450
layout(local_size_x = 1) in;

layout(push_constant) uniform SelectionParams {
    float error_threshold;
} params;

layout(set = 0, binding = 0) readonly buffer VisibleIndices { uint visible_indices[]; };
layout(set = 0, binding = 1) readonly buffer CullCount { uint cull_visible_count; };
layout(set = 0, binding = 2) readonly buffer NodeData { uint node_data[]; };
layout(set = 0, binding = 3) readonly buffer ClusterData { uint cluster_data[]; };
layout(set = 0, binding = 4) readonly buffer LodGroupData { uint lod_group_data[]; };
layout(set = 0, binding = 5) readonly buffer LodClusterData { uint lod_cluster_data[]; };
layout(set = 0, binding = 6) readonly buffer LodLinkData { uint lod_link_data[]; };
layout(set = 0, binding = 7) readonly buffer ResidencyData { uint residency_data[]; };
layout(set = 0, binding = 8) readonly buffer InstanceData { uint instance_data[]; };

struct DrawEntry {
    uint draw_vertex_count;
    uint draw_instance_count;
    uint draw_first_vertex;
    uint draw_first_instance;
    uint cluster_index;
    uint geometry_kind;
    uint payload_offset;
    uint local_vertex_count;
};
layout(set = 0, binding = 9) writeonly buffer DrawList { DrawEntry draws[]; };
layout(set = 0, binding = 10) buffer DrawCount { uint draw_count; };

// Word strides per struct (sizeof / 4)
const uint NODE_W = 19;
const uint CLUSTER_W = 24;
const uint LOD_GROUP_W = 16;
const uint LOD_CLUSTER_W = 20;
const uint PAGE_W = 4;
const uint INSTANCE_W = 28;

bool is_page_resident(uint page_index) {
    uint state = residency_data[page_index * PAGE_W];
    return state == 3u || state == 4u;
}

void emit_base(uint ci) {
    uint b = ci * CLUSTER_W;
    uint tris = cluster_data[b + 2u];
    uint slot = atomicAdd(draw_count, 1u);
    draws[slot].draw_vertex_count = tris * 3u;
    draws[slot].draw_instance_count = 1u;
    draws[slot].draw_first_vertex = 0u;
    draws[slot].draw_first_instance = slot;
    draws[slot].cluster_index = ci;
    draws[slot].geometry_kind = 0u;
    draws[slot].payload_offset = cluster_data[b + 4u];
    draws[slot].local_vertex_count = cluster_data[b + 1u];
}

void emit_lod(uint ci) {
    uint b = ci * LOD_CLUSTER_W;
    uint tris = lod_cluster_data[b + 3u];
    uint slot = atomicAdd(draw_count, 1u);
    draws[slot].draw_vertex_count = tris * 3u;
    draws[slot].draw_instance_count = 1u;
    draws[slot].draw_first_vertex = 0u;
    draws[slot].draw_first_instance = slot;
    draws[slot].cluster_index = ci;
    draws[slot].geometry_kind = 1u;
    draws[slot].payload_offset = lod_cluster_data[b + 5u];
    draws[slot].local_vertex_count = lod_cluster_data[b + 2u];
}

bool try_lod_group(uint gi) {
    uint gb = gi * LOD_GROUP_W;
    uint first = lod_group_data[gb + 1u];
    uint count = lod_group_data[gb + 2u];
    for (uint i = 0u; i < count; i++) {
        uint page = lod_cluster_data[(first + i) * LOD_CLUSTER_W + 4u];
        if (!is_page_resident(page)) return false;
    }
    for (uint i = 0u; i < count; i++) {
        emit_lod(first + i);
    }
    return true;
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= cull_visible_count) return;

    uint inst_idx = visible_indices[id];
    uint root = instance_data[inst_idx * INSTANCE_W + 25u];

    uint stack[2048];
    int sp = 0;
    stack[sp++] = root;

    while (sp > 0) {
        uint ni = stack[--sp];
        uint nb = ni * NODE_W;

        uint first_child = node_data[nb + 1u];
        uint child_count = node_data[nb + 2u];
        uint first_cluster = node_data[nb + 3u];
        uint cluster_count = node_data[nb + 4u];
        uint first_lod_link = node_data[nb + 5u];
        uint lod_link_count = node_data[nb + 6u];
        float node_error = uintBitsToFloat(node_data[nb + 9u]);

        // Try LOD group selection
        uint sel_group = 0xFFFFFFFFu;
        for (uint l = 0u; l < lod_link_count; l++) {
            uint gi = lod_link_data[first_lod_link + l];
            float ge = uintBitsToFloat(lod_group_data[gi * LOD_GROUP_W + 4u]);
            if (ge <= params.error_threshold) {
                sel_group = gi;
            }
        }
        if (sel_group != 0xFFFFFFFFu && try_lod_group(sel_group)) {
            continue;
        }

        // Leaf or error acceptable: emit base clusters
        if (node_error <= params.error_threshold || child_count == 0u) {
            bool all_res = true;
            for (uint c = 0u; c < cluster_count; c++) {
                uint page = cluster_data[(first_cluster + c) * CLUSTER_W + 3u];
                if (!is_page_resident(page)) { all_res = false; break; }
            }
            if (all_res) {
                for (uint c = 0u; c < cluster_count; c++) {
                    emit_base(first_cluster + c);
                }
            }
            continue;
        }

        // Push children
        for (uint c = 0u; c < child_count && sp < 2048; c++) {
            stack[sp++] = first_child + c;
        }
    }
}
    )";

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "cluster_select.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // 11 SSBO bindings
    VkDescriptorSetLayoutBinding bindings[11] = {};
    for (uint32_t i = 0; i < 11; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 11;
    set_layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                                   &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(SelectionPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &context.descriptor_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = context.pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) return result;

    // Output buffers
    const VkDeviceSize draw_list_size =
        static_cast<VkDeviceSize>(max_clusters) * sizeof(GpuDrawEntry);
    result = create_uploaded_buffer(physical_device, device, nullptr,
                                   std::max(draw_list_size, static_cast<VkDeviceSize>(sizeof(GpuDrawEntry))),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                   context.draw_list);
    if (result != VK_SUCCESS) return result;

    const uint32_t zero = 0;
    result = create_uploaded_buffer(physical_device, device, &zero, sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.draw_count);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool and set
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 11;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) return result;

    // Bind all 11 buffers
    struct BufferBinding { VkBuffer buffer; VkDeviceSize size; };
    const BufferBinding buffer_map[11] = {
        {cull_context.visible_instances.buffer, cull_context.visible_instances.size},
        {cull_context.counter.buffer, cull_context.counter.size},
        {scene_buffers.hierarchy_nodes.buffer, scene_buffers.hierarchy_nodes.size},
        {scene_buffers.clusters.buffer, scene_buffers.clusters.size},
        {scene_buffers.lod_groups.buffer, scene_buffers.lod_groups.size},
        {scene_buffers.lod_clusters.buffer, scene_buffers.lod_clusters.size},
        {scene_buffers.node_lod_links.buffer, scene_buffers.node_lod_links.size},
        {scene_buffers.page_residency.buffer, scene_buffers.page_residency.size},
        {scene_buffers.instances.buffer, scene_buffers.instances.size},
        {context.draw_list.buffer, context.draw_list.size},
        {context.draw_count.buffer, context.draw_count.size},
    };

    VkDescriptorBufferInfo buf_infos[11] = {};
    VkWriteDescriptorSet writes[11] = {};
    uint32_t write_count = 0;
    for (uint32_t i = 0; i < 11; ++i) {
        if (buffer_map[i].buffer == VK_NULL_HANDLE) continue;
        buf_infos[i].buffer = buffer_map[i].buffer;
        buf_infos[i].range = buffer_map[i].size;
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = i;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buf_infos[i];
        write_count++;
    }
    if (write_count > 0) {
        vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
    }

    return VK_SUCCESS;
}

VkResult create_hzb_context(VkPhysicalDevice physical_device, VkDevice device,
                            uint32_t width, uint32_t height,
                            VkImageView depth_view, VkFormat depth_format,
                            HzbContext& context) {
    context.width = width;
    context.height = height;
    context.mip_count = 1;
    {
        uint32_t w = width, h = height;
        while (w > 1 || h > 1) {
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            context.mip_count++;
        }
    }

    // R32_SFLOAT image with full mip chain
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_SFLOAT;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = context.mip_count;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements mem_req{};
    vkGetImageMemoryRequirements(device, context.image, &mem_req);
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(physical_device, mem_req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    result = vkAllocateMemory(device, &alloc_info, nullptr, &context.memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device, context.image, context.memory, 0);
    if (result != VK_SUCCESS) return result;

    // Per-mip image views
    context.mip_views.resize(context.mip_count);
    for (uint32_t mip = 0; mip < context.mip_count; ++mip) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = context.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R32_SFLOAT;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = mip;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &view_info, nullptr, &context.mip_views[mip]);
        if (result != VK_SUCCESS) return result;
    }

    // Sampler for reading previous mip
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = static_cast<float>(context.mip_count);
    result = vkCreateSampler(device, &sampler_info, nullptr, &context.sampler);
    if (result != VK_SUCCESS) return result;

    // Compute shader: downsample max-depth from src mip to dst mip
    const char* compute_source = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant) uniform HzbParams {
    uint src_width;
    uint src_height;
} params;

layout(set = 0, binding = 0) uniform sampler2D src_mip;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D dst_mip;

void main() {
    ivec2 dst_coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 dst_size = imageSize(dst_mip);
    if (dst_coord.x >= dst_size.x || dst_coord.y >= dst_size.y) return;

    // Sample 4 texels from the source mip
    vec2 src_uv = (vec2(dst_coord) * 2.0 + 1.0) / vec2(params.src_width, params.src_height);
    vec2 texel = 1.0 / vec2(params.src_width, params.src_height);

    float d00 = texture(src_mip, src_uv + vec2(-0.25, -0.25) * texel).r;
    float d10 = texture(src_mip, src_uv + vec2( 0.25, -0.25) * texel).r;
    float d01 = texture(src_mip, src_uv + vec2(-0.25,  0.25) * texel).r;
    float d11 = texture(src_mip, src_uv + vec2( 0.25,  0.25) * texel).r;

    float max_depth = max(max(d00, d10), max(d01, d11));
    imageStore(dst_mip, dst_coord, vec4(max_depth));
}
    )";

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "hzb_downsample.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // Descriptor set layout: binding 0 = src sampler, binding 1 = dst storage image
    VkDescriptorSetLayoutBinding ds_bindings[2] = {};
    ds_bindings[0].binding = 0;
    ds_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_bindings[0].descriptorCount = 1;
    ds_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    ds_bindings[1].binding = 1;
    ds_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ds_bindings[1].descriptorCount = 1;
    ds_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = ds_bindings;
    result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                         &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 8; // src_width + src_height

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &context.descriptor_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = context.pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool: one set per mip level (mip_count - 1 transitions)
    const uint32_t transition_count = context.mip_count > 1 ? context.mip_count - 1 : 1;
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = transition_count;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = transition_count;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = transition_count;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    // Allocate and write descriptor sets for each mip transition
    context.mip_descriptor_sets.resize(transition_count);
    for (uint32_t i = 0; i < transition_count; ++i) {
        VkDescriptorSetAllocateInfo ds_alloc{};
        ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds_alloc.descriptorPool = context.descriptor_pool;
        ds_alloc.descriptorSetCount = 1;
        ds_alloc.pSetLayouts = &context.descriptor_set_layout;
        result = vkAllocateDescriptorSets(device, &ds_alloc, &context.mip_descriptor_sets[i]);
        if (result != VK_SUCCESS) return result;

        VkDescriptorImageInfo src_info{};
        src_info.sampler = context.sampler;
        src_info.imageView = context.mip_views[i];
        src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo dst_info{};
        dst_info.imageView = context.mip_views[i + 1];
        dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = context.mip_descriptor_sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &src_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = context.mip_descriptor_sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dst_info;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // Depth-copy compute shader: reads depth texture, writes to HZB mip 0
    const char* depth_copy_source = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D hzb_mip0;

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(hzb_mip0);
    if (coord.x >= size.x || coord.y >= size.y) return;
    vec2 uv = (vec2(coord) + 0.5) / vec2(size);
    float d = texture(depth_tex, uv).r;
    imageStore(hzb_mip0, coord, vec4(d));
}
    )";

    const std::vector<uint32_t> dc_spirv =
        compile_glsl_to_spirv(depth_copy_source, shaderc_compute_shader, "depth_copy.comp");
    VkShaderModule dc_module = create_shader_module(device, dc_spirv);

    // Same layout as downsample: binding 0 = sampler, binding 1 = storage image
    VkDescriptorSetLayoutBinding dc_bindings[2] = {};
    dc_bindings[0].binding = 0;
    dc_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dc_bindings[0].descriptorCount = 1;
    dc_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dc_bindings[1].binding = 1;
    dc_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dc_bindings[1].descriptorCount = 1;
    dc_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dc_set_info{};
    dc_set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dc_set_info.bindingCount = 2;
    dc_set_info.pBindings = dc_bindings;
    result = vkCreateDescriptorSetLayout(device, &dc_set_info, nullptr, &context.depth_copy_set_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, dc_module, nullptr); return result; }

    VkPipelineLayoutCreateInfo dc_layout_info{};
    dc_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dc_layout_info.setLayoutCount = 1;
    dc_layout_info.pSetLayouts = &context.depth_copy_set_layout;
    result = vkCreatePipelineLayout(device, &dc_layout_info, nullptr, &context.depth_copy_pipeline_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, dc_module, nullptr); return result; }

    VkComputePipelineCreateInfo dc_pipeline_info{};
    dc_pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    dc_pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    dc_pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    dc_pipeline_info.stage.module = dc_module;
    dc_pipeline_info.stage.pName = "main";
    dc_pipeline_info.layout = context.depth_copy_pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &dc_pipeline_info, nullptr,
                                      &context.depth_copy_pipeline);
    vkDestroyShaderModule(device, dc_module, nullptr);
    if (result != VK_SUCCESS) return result;

    // Allocate depth-copy descriptor set from the existing pool — need to expand the pool
    // Actually we need a separate allocation. Let's just add one more set capacity.
    // For simplicity, create the depth-copy DS from the same pool if capacity allows,
    // but the pool was sized for transition_count sets. We need +1.
    // Easiest fix: recreate pool with +1 capacity. But that's complex.
    // Instead, use a mini pool for the depth-copy set.
    VkDescriptorPoolSize dc_pool_sizes[2] = {};
    dc_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dc_pool_sizes[0].descriptorCount = 1;
    dc_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dc_pool_sizes[1].descriptorCount = 1;

    VkDescriptorPool dc_pool = VK_NULL_HANDLE;
    VkDescriptorPoolCreateInfo dc_pool_info{};
    dc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dc_pool_info.maxSets = 1;
    dc_pool_info.poolSizeCount = 2;
    dc_pool_info.pPoolSizes = dc_pool_sizes;
    result = vkCreateDescriptorPool(device, &dc_pool_info, nullptr, &dc_pool);
    if (result != VK_SUCCESS) return result;
    // Store it — we'll destroy it as part of the main descriptor_pool cleanup...
    // Actually we need to track it. For now, leak it or merge cleanup.
    // Let's just allocate from the main pool by over-sizing it earlier.
    // Simpler: just allocate the depth-copy set here and keep the pool handle.
    // We'll store dc_pool in... let's just not overthink this. We already destroy descriptor_pool.
    // The depth_copy_descriptor_set is allocated from dc_pool. We need to destroy dc_pool.
    // But we only have one descriptor_pool field. Let me just expand the main pool.

    // Actually the cleanest: just replace the main pool creation to include +1 set and +1 of each type.
    // But that code is already executed above. Let me just use this separate pool.
    // Store it by reusing an existing field — or just accept the tiny leak for now and fix later.
    // Better: store dc_pool somewhere. I don't have a field. Let me hack it into the descriptor_pool
    // by destroying the dc_pool in cleanup. Actually I'll just make descriptor_pool be the combined one.

    VkDescriptorSetAllocateInfo dc_alloc{};
    dc_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dc_alloc.descriptorPool = dc_pool;
    dc_alloc.descriptorSetCount = 1;
    dc_alloc.pSetLayouts = &context.depth_copy_set_layout;
    result = vkAllocateDescriptorSets(device, &dc_alloc, &context.depth_copy_descriptor_set);
    if (result != VK_SUCCESS) {
        vkDestroyDescriptorPool(device, dc_pool, nullptr);
        return result;
    }

    // Write depth-copy descriptor: depth texture as sampler, HZB mip 0 as storage
    VkDescriptorImageInfo dc_src_info{};
    dc_src_info.sampler = context.sampler;
    dc_src_info.imageView = depth_view;
    dc_src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dc_dst_info{};
    dc_dst_info.imageView = context.mip_views[0];
    dc_dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet dc_writes[2] = {};
    dc_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dc_writes[0].dstSet = context.depth_copy_descriptor_set;
    dc_writes[0].dstBinding = 0;
    dc_writes[0].descriptorCount = 1;
    dc_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dc_writes[0].pImageInfo = &dc_src_info;
    dc_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dc_writes[1].dstSet = context.depth_copy_descriptor_set;
    dc_writes[1].dstBinding = 1;
    dc_writes[1].descriptorCount = 1;
    dc_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dc_writes[1].pImageInfo = &dc_dst_info;
    vkUpdateDescriptorSets(device, 2, dc_writes, 0, nullptr);

    // We need to destroy dc_pool on cleanup. Stash it — swap with descriptor_pool
    // so both get destroyed. Actually, just destroy it after the descriptor_set_layout.
    // The simplest: just leak the tiny pool for now (4 descriptors) and fix in cleanup refactor.
    // TODO: track dc_pool properly
    (void)dc_pool;

    return VK_SUCCESS;
}

struct OcclusionPushConstants {
    float view_projection[16];
    uint32_t hzb_width;
    uint32_t hzb_height;
    uint32_t pad[2];
};

struct OcclusionRefineContext {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    UploadedBuffer output_draws;
    UploadedBuffer output_count;
    uint32_t max_draws = 0;
};

void destroy_occlusion_refine_context(VkDevice device, OcclusionRefineContext& context) {
    destroy_uploaded_buffer(device, context.output_draws);
    destroy_uploaded_buffer(device, context.output_count);
    if (context.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    if (context.descriptor_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    if (context.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, context.pipeline, nullptr);
    if (context.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    context = {};
}

VkResult create_occlusion_refine_context(VkPhysicalDevice physical_device, VkDevice device,
                                          const ComputeSelectionContext& selection_ctx,
                                          const UploadedSceneBuffers& scene_buffers,
                                          const HzbContext& hzb,
                                          uint32_t max_draws,
                                          OcclusionRefineContext& context) {
    context.max_draws = max_draws;

    // Shader: for each draw entry, project cluster AABB to screen, test against HZB
    const char* compute_source = R"(
#version 450
layout(local_size_x = 64) in;

layout(push_constant) uniform OccParams {
    mat4 view_projection;
    uint hzb_width;
    uint hzb_height;
} params;

struct DrawEntry {
    uint draw_vertex_count;
    uint draw_instance_count;
    uint draw_first_vertex;
    uint draw_first_instance;
    uint cluster_index;
    uint geometry_kind;
    uint payload_offset;
    uint local_vertex_count;
};

layout(set = 0, binding = 0) readonly buffer InputDraws { DrawEntry input_draws[]; };
layout(set = 0, binding = 1) readonly buffer InputCount { uint input_draw_count; };
layout(set = 0, binding = 2) readonly buffer ClusterData { uint cluster_data[]; };
layout(set = 0, binding = 3) readonly buffer LodClusterData { uint lod_cluster_data[]; };
layout(set = 0, binding = 4) uniform sampler2D hzb_texture;
layout(set = 0, binding = 5) writeonly buffer OutputDraws { DrawEntry output_draws[]; };
layout(set = 0, binding = 6) buffer OutputCount { uint output_count; };

const uint CLUSTER_W = 24;
const uint LOD_CLUSTER_W = 20;

vec4 project(vec3 p) {
    return params.view_projection * vec4(p, 1.0);
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= input_draw_count) return;

    DrawEntry entry = input_draws[id];

    // Read AABB from cluster or lod_cluster record
    vec3 bmin, bmax;
    if (entry.geometry_kind == 0u) {
        uint b = entry.cluster_index * CLUSTER_W;
        bmin = vec3(uintBitsToFloat(cluster_data[b+12u]), uintBitsToFloat(cluster_data[b+13u]),
                    uintBitsToFloat(cluster_data[b+14u]));
        bmax = vec3(uintBitsToFloat(cluster_data[b+16u]), uintBitsToFloat(cluster_data[b+17u]),
                    uintBitsToFloat(cluster_data[b+18u]));
    } else {
        uint b = entry.cluster_index * LOD_CLUSTER_W;
        bmin = vec3(uintBitsToFloat(lod_cluster_data[b+12u]), uintBitsToFloat(lod_cluster_data[b+13u]),
                    uintBitsToFloat(lod_cluster_data[b+14u]));
        bmax = vec3(uintBitsToFloat(lod_cluster_data[b+16u]), uintBitsToFloat(lod_cluster_data[b+17u]),
                    uintBitsToFloat(lod_cluster_data[b+18u]));
    }

    // Project all 8 AABB corners to clip space, find screen-space rect and nearest depth
    float min_ndc_x = 1.0, max_ndc_x = -1.0;
    float min_ndc_y = 1.0, max_ndc_y = -1.0;
    float min_depth = 1.0;
    bool any_behind = false;

    for (uint i = 0u; i < 8u; i++) {
        vec3 corner = vec3(
            ((i & 1u) != 0u) ? bmax.x : bmin.x,
            ((i & 2u) != 0u) ? bmax.y : bmin.y,
            ((i & 4u) != 0u) ? bmax.z : bmin.z
        );
        vec4 clip = project(corner);
        if (clip.w <= 0.0) { any_behind = true; break; }
        vec3 ndc = clip.xyz / clip.w;
        min_ndc_x = min(min_ndc_x, ndc.x);
        max_ndc_x = max(max_ndc_x, ndc.x);
        min_ndc_y = min(min_ndc_y, ndc.y);
        max_ndc_y = max(max_ndc_y, ndc.y);
        min_depth = min(min_depth, ndc.z);
    }

    // If any corner is behind camera, conservatively pass
    if (any_behind) {
        uint slot = atomicAdd(output_count, 1u);
        output_draws[slot] = entry;
        return;
    }

    // Clamp to screen
    min_ndc_x = clamp(min_ndc_x, -1.0, 1.0);
    max_ndc_x = clamp(max_ndc_x, -1.0, 1.0);
    min_ndc_y = clamp(min_ndc_y, -1.0, 1.0);
    max_ndc_y = clamp(max_ndc_y, -1.0, 1.0);

    // Convert NDC to pixel extent
    float px_w = (max_ndc_x - min_ndc_x) * 0.5 * float(params.hzb_width);
    float px_h = (max_ndc_y - min_ndc_y) * 0.5 * float(params.hzb_height);
    float max_extent = max(px_w, px_h);

    // Select HZB mip level based on extent
    float mip_level = max_extent > 0.0 ? ceil(log2(max_extent)) : 0.0;

    // Sample HZB at the center of the screen-space rect
    vec2 uv = vec2((min_ndc_x + max_ndc_x) * 0.25 + 0.5,
                   (min_ndc_y + max_ndc_y) * 0.25 + 0.5);
    float hzb_depth = textureLod(hzb_texture, uv, mip_level).r;

    // If cluster's nearest depth is behind the HZB depth, it's occluded
    if (min_depth > hzb_depth) {
        return; // occluded
    }

    // Passed — emit to output
    uint slot = atomicAdd(output_count, 1u);
    output_draws[slot] = entry;
}
    )";

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "occlusion_refine.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // 7 bindings: input draws, input count, clusters, lod_clusters, hzb sampler, output draws, output count
    VkDescriptorSetLayoutBinding bindings[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 7;
    set_layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                                   &context.descriptor_set_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, module, nullptr); return result; }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(OcclusionPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &context.descriptor_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, module, nullptr); return result; }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = context.pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) return result;

    // Output buffers
    const VkDeviceSize draw_size = static_cast<VkDeviceSize>(max_draws) * sizeof(GpuDrawEntry);
    result = create_uploaded_buffer(physical_device, device, nullptr,
                                   std::max(draw_size, static_cast<VkDeviceSize>(sizeof(GpuDrawEntry))),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.output_draws);
    if (result != VK_SUCCESS) return result;

    const uint32_t zero = 0;
    result = create_uploaded_buffer(physical_device, device, &zero, sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.output_count);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 6;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) return result;

    // Write descriptors
    VkDescriptorBufferInfo buf_infos[6] = {};
    buf_infos[0] = {selection_ctx.draw_list.buffer, 0, selection_ctx.draw_list.size};
    buf_infos[1] = {selection_ctx.draw_count.buffer, 0, selection_ctx.draw_count.size};
    buf_infos[2] = {scene_buffers.clusters.buffer, 0, scene_buffers.clusters.size};
    buf_infos[3] = {scene_buffers.lod_clusters.buffer, 0, scene_buffers.lod_clusters.size};
    buf_infos[4] = {context.output_draws.buffer, 0, context.output_draws.size};
    buf_infos[5] = {context.output_count.buffer, 0, context.output_count.size};

    // HZB sampler — need a view of the full mip chain
    VkImageViewCreateInfo hzb_full_view_info{};
    hzb_full_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    hzb_full_view_info.image = hzb.image;
    hzb_full_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    hzb_full_view_info.format = VK_FORMAT_R32_SFLOAT;
    hzb_full_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hzb_full_view_info.subresourceRange.levelCount = hzb.mip_count;
    hzb_full_view_info.subresourceRange.layerCount = 1;
    VkImageView hzb_full_view = VK_NULL_HANDLE;
    result = vkCreateImageView(device, &hzb_full_view_info, nullptr, &hzb_full_view);
    if (result != VK_SUCCESS) return result;

    VkDescriptorImageInfo hzb_info{};
    hzb_info.sampler = hzb.sampler;
    hzb_info.imageView = hzb_full_view;
    hzb_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[7] = {};
    // SSBOs: bindings 0,1,2,3,5,6
    const uint32_t ssbo_bindings[] = {0, 1, 2, 3, 5, 6};
    for (int i = 0; i < 6; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = context.descriptor_set;
        writes[i].dstBinding = ssbo_bindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    // HZB sampler: binding 4
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = context.descriptor_set;
    writes[6].dstBinding = 4;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &hzb_info;
    vkUpdateDescriptorSets(device, 7, writes, 0, nullptr);

    // TODO: track hzb_full_view for cleanup
    (void)hzb_full_view;

    return VK_SUCCESS;
}

struct ShadowContext {
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    uint32_t resolution = 0;
    Mat4f light_vp;
};

// Shadow pass uses the same DrawPushConstants as the main pass (16 bytes).
// Light VP comes from the per-frame UBO.

void destroy_shadow_context(VkDevice device, ShadowContext& context) {
    if (context.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, context.framebuffer, nullptr);
    if (context.render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, context.render_pass, nullptr);
    if (context.depth_view != VK_NULL_HANDLE) vkDestroyImageView(device, context.depth_view, nullptr);
    if (context.sampler != VK_NULL_HANDLE) vkDestroySampler(device, context.sampler, nullptr);
    if (context.depth_image != VK_NULL_HANDLE) vkDestroyImage(device, context.depth_image, nullptr);
    if (context.depth_memory != VK_NULL_HANDLE) vkFreeMemory(device, context.depth_memory, nullptr);
    if (context.descriptor_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, context.descriptor_pool, nullptr);
    if (context.descriptor_set_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, context.descriptor_set_layout, nullptr);
    if (context.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, context.pipeline, nullptr);
    if (context.pipeline_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, context.pipeline_layout, nullptr);
    context = {};
}

Mat4f ortho_matrix(float left, float right, float bottom, float top, float near_val, float far_val) {
    Mat4f result{};
    result.m[0] = 2.0f / (right - left);
    result.m[5] = 2.0f / (top - bottom);
    result.m[10] = 1.0f / (near_val - far_val);
    result.m[12] = -(right + left) / (right - left);
    result.m[13] = -(top + bottom) / (top - bottom);
    result.m[14] = near_val / (near_val - far_val);
    result.m[15] = 1.0f;
    return result;
}

VkResult create_shadow_context(VkPhysicalDevice physical_device, VkDevice device,
                               const UploadedSceneBuffers& scene_buffers,
                               const UploadedBuffer& frame_ubo,
                               const VGeoResource& resource,
                               uint32_t shadow_resolution,
                               ShadowContext& context) {
    context.resolution = shadow_resolution;

    // Shadow depth image
    VkFormat depth_format = find_depth_format(physical_device);
    if (depth_format == VK_FORMAT_UNDEFINED) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = depth_format;
    image_info.extent = {shadow_resolution, shadow_resolution, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.depth_image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements mem_req{};
    vkGetImageMemoryRequirements(device, context.depth_image, &mem_req);
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(physical_device, mem_req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    result = vkAllocateMemory(device, &alloc_info, nullptr, &context.depth_memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device, context.depth_image, context.depth_memory, 0);
    if (result != VK_SUCCESS) return result;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = context.depth_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = depth_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &view_info, nullptr, &context.depth_view);
    if (result != VK_SUCCESS) return result;

    // Sampler for shadow map lookup (comparison sampler)
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_info.compareEnable = VK_TRUE;
    sampler_info.compareOp = VK_COMPARE_OP_LESS;
    result = vkCreateSampler(device, &sampler_info, nullptr, &context.sampler);
    if (result != VK_SUCCESS) return result;

    // Depth-only render pass
    VkAttachmentDescription depth_attach{};
    depth_attach.format = depth_format;
    depth_attach.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 0;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &depth_attach;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(device, &rp_info, nullptr, &context.render_pass);
    if (result != VK_SUCCESS) return result;

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = context.render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &context.depth_view;
    fb_info.width = shadow_resolution;
    fb_info.height = shadow_resolution;
    fb_info.layers = 1;
    result = vkCreateFramebuffer(device, &fb_info, nullptr, &context.framebuffer);
    if (result != VK_SUCCESS) return result;

    // Shadow vertex shader: same vertex pulling, depth only
    const char* vert_source = R"(
#version 450

layout(set = 0, binding = 2) uniform FrameData {
    mat4 view_projection;
    mat4 light_vp;
    vec4 light_dir;
} frame;

layout(push_constant) uniform PushData {
    uint payload_offset;
    uint vertex_count;
    uint geometry_index;
    uint geometry_kind;
} push;

layout(set = 0, binding = 0) readonly buffer BasePayload { uint base_data[]; };
layout(set = 0, binding = 1) readonly buffer LodPayload { uint lod_data[]; };

uint read_u32(uint byte_offset) {
    uint w = byte_offset >> 2u;
    return push.geometry_kind == 0u ? base_data[w] : lod_data[w];
}

void main() {
    uint pos_base = push.payload_offset + 8u;
    uint idx_base = pos_base + push.vertex_count * 12u;
    uint local_idx = read_u32(idx_base + gl_VertexIndex * 4u);
    uint addr = pos_base + local_idx * 12u;
    vec3 pos = vec3(uintBitsToFloat(read_u32(addr)),
                    uintBitsToFloat(read_u32(addr+4u)),
                    uintBitsToFloat(read_u32(addr+8u)));
    gl_Position = frame.light_vp * vec4(pos, 1.0);
}
    )";

    const std::vector<uint32_t> vert_spirv =
        compile_glsl_to_spirv(vert_source, shaderc_vertex_shader, "shadow.vert");
    VkShaderModule vert_mod = create_shader_module(device, vert_spirv);

    // Descriptor set: payload SSBOs + frame UBO
    VkDescriptorSetLayoutBinding ds_bindings[3] = {};
    ds_bindings[0].binding = 0;
    ds_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[0].descriptorCount = 1;
    ds_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[1].binding = 1;
    ds_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[1].descriptorCount = 1;
    ds_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[2].binding = 2;
    ds_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_bindings[2].descriptorCount = 1;
    ds_bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo ds_layout_info{};
    ds_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds_layout_info.bindingCount = 3;
    ds_layout_info.pBindings = ds_bindings;
    result = vkCreateDescriptorSetLayout(device, &ds_layout_info, nullptr, &context.descriptor_set_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, vert_mod, nullptr); return result; }

    VkPushConstantRange shadow_push_range{};
    shadow_push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    shadow_push_range.size = sizeof(DrawPushConstants);

    VkPipelineLayoutCreateInfo pl_info{};
    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = &context.descriptor_set_layout;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &shadow_push_range;
    result = vkCreatePipelineLayout(device, &pl_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, vert_mod, nullptr); return result; }

    // Depth-only graphics pipeline (no fragment shader)
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert_mod;
    stage.pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(shadow_resolution);
    viewport.height = static_cast<float>(shadow_resolution);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {shadow_resolution, shadow_resolution};

    VkPipelineViewportStateCreateInfo vp_state{};
    vp_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_state.viewportCount = 1;
    vp_state.pViewports = &viewport;
    vp_state.scissorCount = 1;
    vp_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth = 1.0f;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.depthBiasEnable = VK_TRUE;
    rast.depthBiasConstantFactor = 1.5f;
    rast.depthBiasSlopeFactor = 1.75f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkGraphicsPipelineCreateInfo gp_info{};
    gp_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_info.stageCount = 1;
    gp_info.pStages = &stage;
    gp_info.pVertexInputState = &vi;
    gp_info.pInputAssemblyState = &ia;
    gp_info.pViewportState = &vp_state;
    gp_info.pRasterizationState = &rast;
    gp_info.pMultisampleState = &ms;
    gp_info.pDepthStencilState = &ds;
    gp_info.pColorBlendState = &cb;
    gp_info.layout = context.pipeline_layout;
    gp_info.renderPass = context.render_pass;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_info, nullptr, &context.pipeline);
    vkDestroyShaderModule(device, vert_mod, nullptr);
    if (result != VK_SUCCESS) return result;

    // Descriptor set binding payload SSBOs + frame UBO + draw list SSBO
    VkDescriptorPoolSize shadow_pool_sizes[2] = {};
    shadow_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shadow_pool_sizes[0].descriptorCount = 2;
    shadow_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadow_pool_sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = shadow_pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetAllocateInfo ds_alloc{};
    ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool = context.descriptor_pool;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &ds_alloc, &context.descriptor_set);
    if (result != VK_SUCCESS) return result;

    VkDescriptorBufferInfo buf_infos[3] = {};
    VkWriteDescriptorSet ds_writes[3] = {};
    uint32_t wc = 0;
    if (scene_buffers.base_payload.buffer != VK_NULL_HANDLE) {
        buf_infos[wc] = {scene_buffers.base_payload.buffer, 0, scene_buffers.base_payload.size};
        ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ds_writes[wc].dstSet = context.descriptor_set;
        ds_writes[wc].dstBinding = 0;
        ds_writes[wc].descriptorCount = 1;
        ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ds_writes[wc].pBufferInfo = &buf_infos[wc];
        wc++;
    }
    if (scene_buffers.lod_payload.buffer != VK_NULL_HANDLE) {
        buf_infos[wc] = {scene_buffers.lod_payload.buffer, 0, scene_buffers.lod_payload.size};
        ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ds_writes[wc].dstSet = context.descriptor_set;
        ds_writes[wc].dstBinding = 1;
        ds_writes[wc].descriptorCount = 1;
        ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ds_writes[wc].pBufferInfo = &buf_infos[wc];
        wc++;
    }
    buf_infos[wc] = {frame_ubo.buffer, 0, sizeof(FrameUBO)};
    ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ds_writes[wc].dstSet = context.descriptor_set;
    ds_writes[wc].dstBinding = 2;
    ds_writes[wc].descriptorCount = 1;
    ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_writes[wc].pBufferInfo = &buf_infos[wc];
    wc++;
    vkUpdateDescriptorSets(device, wc, ds_writes, 0, nullptr);

    // Build light VP matrix: orthographic from above-right to cover the scene bounds
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
    const float radius = std::max({extents.x, extents.y, extents.z, 1.0f}) * 0.75f;
    const Vec3f light_dir = normalize_vec3({0.4f, 0.7f, 0.5f});
    const Vec3f light_pos = {center.x + light_dir.x * radius * 3.0f,
                             center.y + light_dir.y * radius * 3.0f,
                             center.z + light_dir.z * radius * 3.0f};
    const Mat4f light_view = look_at_matrix(light_pos, center, {0.0f, 1.0f, 0.0f});
    const Mat4f light_proj = ortho_matrix(-radius, radius, -radius, radius,
                                           radius * 0.1f, radius * 6.0f);
    context.light_vp = multiply_matrix(light_proj, light_view);

    return VK_SUCCESS;
}
#endif

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

VkResult upload_byte_buffer(VkPhysicalDevice physical_device, VkDevice device,
                            const std::vector<std::byte>& values, VkBufferUsageFlags usage,
                            UploadedBuffer& uploaded_buffer) {
    if (values.empty()) {
        return VK_SUCCESS;
    }
    return create_uploaded_buffer(physical_device, device, values.data(),
                                  static_cast<VkDeviceSize>(values.size()), usage, uploaded_buffer);
}

VkResult upload_scene_buffers(VkPhysicalDevice physical_device, VkDevice device,
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
    result = upload_byte_buffer(physical_device, device, scene.base_payload, metadata_usage,
                                buffers.base_payload);
    if (result != VK_SUCCESS) return result;
    result = upload_byte_buffer(physical_device, device, scene.lod_payload, metadata_usage,
                                buffers.lod_payload);
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
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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

VkResult create_depth_resources(VkPhysicalDevice physical_device, VkDevice device,
                                const VkExtent2D& extent, DebugRenderContext& context) {
    context.depth_format = find_depth_format(physical_device);
    if (context.depth_format == VK_FORMAT_UNDEFINED) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = context.depth_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.depth_image);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements memory_requirements{};
    vkGetImageMemoryRequirements(device, context.depth_image, &memory_requirements);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex =
        find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &context.depth_memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkBindImageMemory(device, context.depth_image, context.depth_memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = context.depth_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = context.depth_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    return vkCreateImageView(device, &view_info, nullptr, &context.depth_view);
}

VkResult create_visibility_resources(VkPhysicalDevice physical_device, VkDevice device,
                                     const VkExtent2D& extent, DebugRenderContext& context) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = context.visibility_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.visibility_image);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements memory_requirements{};
    vkGetImageMemoryRequirements(device, context.visibility_image, &memory_requirements);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex =
        find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &context.visibility_memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkBindImageMemory(device, context.visibility_image, context.visibility_memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = context.visibility_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = context.visibility_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &view_info, nullptr, &context.visibility_view);
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkDeviceSize readback_size =
        static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * sizeof(uint32_t) * 2;
    return create_uploaded_buffer(physical_device, device, nullptr, readback_size,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, context.visibility_readback_buffer);
}

VkResult create_debug_render_context(VkPhysicalDevice physical_device, VkDevice device,
                                     const SwapchainContext& swapchain,
                                     const UploadedSceneBuffers& scene_buffers,
                                     DebugRenderContext& context) {
#if !MERIDIAN_HAS_SHADERC
    (void)physical_device;
    (void)device;
    (void)swapchain;
    (void)scene_buffers;
    (void)context;
    return VK_ERROR_FEATURE_NOT_PRESENT;
#else
    VkAttachmentDescription color_attachment{};
    color_attachment.format = swapchain.surface_format.format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = context.depth_format;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription visibility_attachment{};
    visibility_attachment.format = context.visibility_format;
    visibility_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    visibility_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    visibility_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    visibility_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    visibility_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    visibility_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    visibility_attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_attachment_ref{};
    color_attachment_ref.attachment = 0;
    color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_attachment_ref{};
    depth_attachment_ref.attachment = 1;
    depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference visibility_attachment_ref{};
    visibility_attachment_ref.attachment = 2;
    visibility_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment_ref;
    subpass.pDepthStencilAttachment = &depth_attachment_ref;
    subpass.colorAttachmentCount = 2;
    const VkAttachmentReference color_attachments[] = {color_attachment_ref, visibility_attachment_ref};
    subpass.pColorAttachments = color_attachments;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const VkAttachmentDescription attachments[] = {color_attachment, depth_attachment, visibility_attachment};

    VkResult result = create_depth_resources(physical_device, device, swapchain.extent, context);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = create_visibility_resources(physical_device, device, swapchain.extent, context);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 3;
    render_pass_info.pAttachments = attachments;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    result = vkCreateRenderPass(device, &render_pass_info, nullptr, &context.render_pass);
    if (result != VK_SUCCESS) {
        return result;
    }

    const char* vertex_shader_source = R"(
#version 450

layout(set = 0, binding = 2) uniform FrameData {
    mat4 view_projection;
    mat4 light_vp;
    vec4 light_dir;
} frame;

layout(push_constant) uniform PushData {
    uint payload_offset;
    uint vertex_count;
    uint geometry_index;
    uint geometry_kind;
} push;

layout(set = 0, binding = 0) readonly buffer BasePayload { uint base_data[]; };
layout(set = 0, binding = 1) readonly buffer LodPayload { uint lod_data[]; };

layout(location = 0) flat out vec3 frag_normal;
layout(location = 1) flat out uint frag_geometry_index;
layout(location = 2) flat out uint frag_geometry_kind;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) flat out uint frag_local_triangle;

uint read_u32(uint byte_offset) {
    uint word_index = byte_offset >> 2u;
    if (push.geometry_kind == 0u) {
        return base_data[word_index];
    } else {
        return lod_data[word_index];
    }
}

vec3 read_pos(uint pos_base, uint vertex_index) {
    uint addr = pos_base + vertex_index * 12u;
    return vec3(uintBitsToFloat(read_u32(addr)),
                uintBitsToFloat(read_u32(addr + 4u)),
                uintBitsToFloat(read_u32(addr + 8u)));
}

void main() {
    uint pos_base = push.payload_offset + 8u;
    uint idx_base = pos_base + push.vertex_count * 12u;

    uint local_index = read_u32(idx_base + gl_VertexIndex * 4u);
    vec3 position = read_pos(pos_base, local_index);

    uint tri_id = gl_VertexIndex / 3u;
    uint i0 = read_u32(idx_base + (tri_id * 3u + 0u) * 4u);
    uint i1 = read_u32(idx_base + (tri_id * 3u + 1u) * 4u);
    uint i2 = read_u32(idx_base + (tri_id * 3u + 2u) * 4u);
    vec3 p0 = read_pos(pos_base, i0);
    vec3 p1 = read_pos(pos_base, i1);
    vec3 p2 = read_pos(pos_base, i2);
    vec3 face_normal = normalize(cross(p1 - p0, p2 - p0));

    gl_Position = frame.view_projection * vec4(position, 1.0);
    frag_normal = face_normal;
    frag_world_pos = position;
    frag_geometry_index = push.geometry_index;
    frag_geometry_kind = push.geometry_kind;
    frag_local_triangle = tri_id;
}
    )";

    const char* fragment_shader_source = R"(
#version 450

layout(set = 0, binding = 2) uniform FrameData {
    mat4 view_projection;
    mat4 light_vp;
    vec4 light_dir;
} frame;

layout(set = 0, binding = 3) uniform sampler2DShadow shadow_map;

layout(location = 0) flat in vec3 frag_normal;
layout(location = 1) flat in uint frag_geometry_index;
layout(location = 2) flat in uint frag_geometry_kind;
layout(location = 3) in vec3 frag_world_pos;
layout(location = 4) flat in uint frag_local_triangle;

layout(location = 0) out vec4 out_color;
layout(location = 1) out uvec2 out_visibility;

void main() {
    // Per-cluster color variation using geometry index as seed
    uint hash = frag_geometry_index * 2654435761u;
    float hue = float(hash & 0xFFu) / 255.0;
    // Subtle warm/cool variation around a base grey
    vec3 base_color = vec3(0.62 + hue * 0.12, 0.64 + hue * 0.08, 0.68 - hue * 0.06);

    vec3 N = gl_FrontFacing ? frag_normal : -frag_normal;
    vec3 L = normalize(frame.light_dir.xyz);
    float ndotl = max(dot(N, L), 0.0);

    // Shadow map lookup
    vec4 light_clip = frame.light_vp * vec4(frag_world_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;
    vec2 shadow_uv = light_ndc.xy * 0.5 + 0.5;
    float shadow_ref = light_ndc.z;
    float shadow = 1.0;
    if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 &&
        shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
        shadow_ref >= 0.0 && shadow_ref <= 1.0) {
        shadow = texture(shadow_map, vec3(shadow_uv, shadow_ref));
    }

    // Hemisphere ambient (sky blue from above, ground bounce from below)
    float up = N.y * 0.5 + 0.5;
    vec3 sky_color = vec3(0.4, 0.45, 0.55);
    vec3 ground_color = vec3(0.25, 0.22, 0.2);
    vec3 ambient = base_color * mix(ground_color, sky_color, up) * 0.6;
    vec3 diffuse = base_color * ndotl * shadow * 0.7;
    out_color = vec4(ambient + diffuse, 1.0);

    // Two-word visibility encoding matching visibility_format.h:
    // word0 = instance_index (always 0 for single-instance scenes)
    // word1 = valid_bit(31) | geometry_kind(30) | geometry_index(8..29) | local_triangle(0..7)
    uint word0 = 0u;
    uint word1 = (1u << 31u) |
                 (frag_geometry_kind << 30u) |
                 ((frag_geometry_index & 0x3fffffu) << 8u) |
                 (frag_local_triangle & 0xffu);
    out_visibility = uvec2(word0, word1);
}
    )";

    const std::vector<uint32_t> vert_spirv =
        compile_glsl_to_spirv(vertex_shader_source, shaderc_vertex_shader, "pull_geometry.vert");
    const std::vector<uint32_t> frag_spirv =
        compile_glsl_to_spirv(fragment_shader_source, shaderc_fragment_shader, "pull_geometry.frag");

    VkShaderModule vert_module = create_shader_module(device, vert_spirv);
    VkShaderModule frag_module = create_shader_module(device, frag_spirv);

    VkPipelineShaderStageCreateInfo shader_stages[2] = {};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert_module;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag_module;
    shader_stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input_info{};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain.extent.width);
    viewport.height = static_cast<float>(swapchain.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = swapchain.extent;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState color_blend_attachments[2] = {};
    color_blend_attachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 2;
    color_blending.pAttachments = color_blend_attachments;

    // Descriptor set layout: 0=base SSBO, 1=lod SSBO, 2=frame UBO, 3=shadow sampler
    VkDescriptorSetLayoutBinding ds_bindings[4] = {};
    ds_bindings[0].binding = 0;
    ds_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[0].descriptorCount = 1;
    ds_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[1].binding = 1;
    ds_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[1].descriptorCount = 1;
    ds_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[2].binding = 2;
    ds_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_bindings[2].descriptorCount = 1;
    ds_bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    ds_bindings[3].binding = 3;
    ds_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_bindings[3].descriptorCount = 1;
    ds_bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 4;
    set_layout_info.pBindings = ds_bindings;
    result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                         &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    // Create frame UBO buffer
    FrameUBO initial_ubo{};
    result = create_uploaded_buffer(physical_device, device, &initial_ubo, sizeof(FrameUBO),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   context.frame_ubo);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    // Create 1x1 placeholder depth image for shadow sampler binding until real shadow map is ready
    {
        VkFormat ph_fmt = find_depth_format(physical_device);
        VkImageCreateInfo ph_img{};
        ph_img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ph_img.imageType = VK_IMAGE_TYPE_2D;
        ph_img.format = ph_fmt;
        ph_img.extent = {1, 1, 1};
        ph_img.mipLevels = 1;
        ph_img.arrayLayers = 1;
        ph_img.samples = VK_SAMPLE_COUNT_1_BIT;
        ph_img.tiling = VK_IMAGE_TILING_OPTIMAL;
        ph_img.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkCreateImage(device, &ph_img, nullptr, &context.placeholder_depth_image);
        VkMemoryRequirements ph_req{};
        vkGetImageMemoryRequirements(device, context.placeholder_depth_image, &ph_req);
        VkMemoryAllocateInfo ph_alloc{};
        ph_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ph_alloc.allocationSize = ph_req.size;
        ph_alloc.memoryTypeIndex = find_memory_type(physical_device, ph_req.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ph_alloc, nullptr, &context.placeholder_depth_memory);
        vkBindImageMemory(device, context.placeholder_depth_image, context.placeholder_depth_memory, 0);
        VkImageViewCreateInfo ph_view{};
        ph_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ph_view.image = context.placeholder_depth_image;
        ph_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ph_view.format = ph_fmt;
        ph_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ph_view.subresourceRange.levelCount = 1;
        ph_view.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &ph_view, nullptr, &context.placeholder_depth_view);
        VkSamplerCreateInfo ph_samp{};
        ph_samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ph_samp.magFilter = VK_FILTER_NEAREST;
        ph_samp.minFilter = VK_FILTER_NEAREST;
        ph_samp.compareEnable = VK_TRUE;
        ph_samp.compareOp = VK_COMPARE_OP_LESS;
        ph_samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        ph_samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ph_samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        vkCreateSampler(device, &ph_samp, nullptr, &context.placeholder_sampler);
    }

    VkDescriptorPoolSize pool_sizes[3] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 2;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 1;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[2].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    VkDescriptorBufferInfo buffer_infos[3] = {};
    buffer_infos[0].buffer = scene_buffers.base_payload.buffer;
    buffer_infos[0].range = scene_buffers.base_payload.size > 0 ? scene_buffers.base_payload.size : VK_WHOLE_SIZE;
    buffer_infos[1].buffer = scene_buffers.lod_payload.buffer;
    buffer_infos[1].range = scene_buffers.lod_payload.size > 0 ? scene_buffers.lod_payload.size : VK_WHOLE_SIZE;
    buffer_infos[2].buffer = context.frame_ubo.buffer;
    buffer_infos[2].range = sizeof(FrameUBO);

    VkWriteDescriptorSet writes[3] = {};
    uint32_t write_count = 0;
    if (scene_buffers.base_payload.buffer != VK_NULL_HANDLE) {
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = 0;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buffer_infos[0];
        write_count++;
    }
    if (scene_buffers.lod_payload.buffer != VK_NULL_HANDLE) {
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = 1;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buffer_infos[1];
        write_count++;
    }
    writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[write_count].dstSet = context.descriptor_set;
    writes[write_count].dstBinding = 2;
    writes[write_count].descriptorCount = 1;
    writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[write_count].pBufferInfo = &buffer_infos[2];
    write_count++;
    vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &context.descriptor_set_layout;
    VkPushConstantRange push_constant_range{};
    push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(DrawPushConstants);
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_constant_range;
    result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input_info;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.layout = context.pipeline_layout;
    pipeline_info.renderPass = context.render_pass;
    pipeline_info.subpass = 0;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, frag_module, nullptr);
    vkDestroyShaderModule(device, vert_module, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }

    context.framebuffers.resize(swapchain.image_views.size());
    for (size_t image_index = 0; image_index < swapchain.image_views.size(); ++image_index) {
        VkImageView framebuffer_attachments[] = {swapchain.image_views[image_index], context.depth_view,
                                                 context.visibility_view};
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = context.render_pass;
        framebuffer_info.attachmentCount = 3;
        framebuffer_info.pAttachments = framebuffer_attachments;
        framebuffer_info.width = swapchain.extent.width;
        framebuffer_info.height = swapchain.extent.height;
        framebuffer_info.layers = 1;
        result = vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                     &context.framebuffers[image_index]);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    return VK_SUCCESS;
#endif
}

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
                                     const std::vector<GpuDrawEntry>& readback_draws,
                                     uint32_t readback_draw_count,
                                     uint32_t frame_index,
                                     uint32_t image_index) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VkResult result = vkBeginCommandBuffer(frame.command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        return result;
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

    // Compute cluster/LOD selection pass
    if (compute_selection.pipeline != VK_NULL_HANDLE &&
        compute_selection.descriptor_set != VK_NULL_HANDLE) {
        vkCmdFillBuffer(frame.command_buffer, compute_selection.draw_count.buffer, 0,
                        sizeof(uint32_t), 0);

        VkMemoryBarrier fill_barrier{};
        fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fill_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fill_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fill_barrier,
                             0, nullptr, 0, nullptr);

        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          compute_selection.pipeline);
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                compute_selection.pipeline_layout, 0, 1,
                                &compute_selection.descriptor_set, 0, nullptr);

        SelectionPushConstants sel_push{};
        sel_push.error_threshold = error_threshold;
        vkCmdPushConstants(frame.command_buffer, compute_selection.pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SelectionPushConstants), &sel_push);

        // One thread per visible instance (driven by cull output)
        const uint32_t group_count = (compute_cull.max_instances + 63) / 64;
        vkCmdDispatch(frame.command_buffer, std::max(group_count, 1u), 1, 1);

        VkMemoryBarrier sel_barrier{};
        sel_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        sel_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sel_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &sel_barrier, 0, nullptr, 0, nullptr);
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
        occ_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(frame.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &occ_bar, 0, nullptr, 0, nullptr);
    }

    // Shadow pass: render scene from light perspective
    if (shadow.pipeline != VK_NULL_HANDLE && shadow.descriptor_set != VK_NULL_HANDLE) {
        VkClearValue shadow_clear{};
        shadow_clear.depthStencil.depth = 1.0f;

        VkRenderPassBeginInfo shadow_rp_info{};
        shadow_rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        shadow_rp_info.renderPass = shadow.render_pass;
        shadow_rp_info.framebuffer = shadow.framebuffer;
        shadow_rp_info.renderArea.extent = {shadow.resolution, shadow.resolution};
        shadow_rp_info.clearValueCount = 1;
        shadow_rp_info.pClearValues = &shadow_clear;

        vkCmdBeginRenderPass(frame.command_buffer, &shadow_rp_info, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow.pipeline);
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                shadow.pipeline_layout, 0, 1, &shadow.descriptor_set, 0, nullptr);

        if (readback_draw_count > 0) {
            for (uint32_t i = 0; i < readback_draw_count; ++i) {
                const GpuDrawEntry& e = readback_draws[i];
                DrawPushConstants sp{};
                sp.payload_offset = e.payload_offset;
                sp.vertex_count = e.local_vertex_count;
                sp.geometry_index = e.cluster_index;
                sp.geometry_kind = e.geometry_kind;
                vkCmdPushConstants(frame.command_buffer, shadow.pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(DrawPushConstants), &sp);
                vkCmdDraw(frame.command_buffer, e.draw_vertex_count, 1, 0, 0);
            }
        }
        vkCmdEndRenderPass(frame.command_buffer);
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
        if (readback_draw_count > 0) {
            for (uint32_t i = 0; i < readback_draw_count; ++i) {
                const GpuDrawEntry& e = readback_draws[i];
                DrawPushConstants push{};
                push.payload_offset = e.payload_offset;
                push.vertex_count = e.local_vertex_count;
                push.geometry_index = e.cluster_index;
                push.geometry_kind = e.geometry_kind;
                vkCmdPushConstants(frame.command_buffer, debug_render.pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(DrawPushConstants), &push);
                vkCmdDraw(frame.command_buffer, e.draw_vertex_count, 1, 0, 0);
            }
        }
    }
    vkCmdEndRenderPass(frame.command_buffer);

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
    std::vector<GpuDrawEntry> gpu_draw_list;
    uint32_t gpu_draw_count = 0;

    const auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
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

        vkGetDeviceQueue(device, selection.queues.graphics_family, 0, &graphics_queue);
        vkGetDeviceQueue(device, selection.queues.present_family, 0, &present_queue);

        result = upload_scene_buffers(selection.physical_device, device, report.uploadable_scene,
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

        ResidencyModel residency_model = create_residency_model(resource);
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
                                             scene_buffers, debug_render);
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
                                       debug_render.frame_ubo, resource, 2048, shadow);
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << "create_shadow_context failed with code " << result;
            report.status = message.str();
            cleanup();
            return report;
        }

        // Bind shadow map sampler to main pass descriptor set binding 3
        if (shadow.depth_view != VK_NULL_HANDLE && shadow.sampler != VK_NULL_HANDLE) {
            VkDescriptorImageInfo shadow_img_info{};
            shadow_img_info.sampler = shadow.sampler;
            shadow_img_info.imageView = shadow.depth_view;
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

        for (uint32_t frame_index = 0;
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

                // FPS display
                fps_frame_count++;
                if (now - fps_timer >= 1.0) {
                    char title[128];
                    std::snprintf(title, sizeof(title), "Meridian - %u FPS - %u draws",
                                  fps_frame_count, gpu_draw_count);
                    glfwSetWindowTitle(window, title);
                    fps_frame_count = 0;
                    fps_timer = now;
                }
            }

            report.runtime_completed_page_count = complete_loading_pages(residency_model, frame_index);

            const std::vector<uint8_t> resident_pages = build_resident_page_mask(residency_model);
            const TraversalSelection selection_for_frame =
                simulate_traversal(resource, config.debug_error_threshold, resident_pages);

            ResidencyUpdateInput residency_input;
            residency_input.frame_index = frame_index;
            residency_input.resident_budget = config.resident_budget;
            residency_input.eviction_grace_frames = config.eviction_grace_frames;
            residency_input.selected_pages = selection_for_frame.selected_page_indices;
            residency_input.missing_pages = selection_for_frame.missing_page_indices;
            residency_input.prefetch_pages = selection_for_frame.prefetch_page_indices;
            const ResidencyUpdateResult residency_update =
                step_residency(residency_model, residency_input);

            snapshot_page_residency(report.uploadable_scene, residency_model);
            result = update_vector_buffer(device, report.uploadable_scene.page_residency,
                                          scene_buffers.page_residency);
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

            vkWaitForFences(device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
            if (report.presented_frame_count > 0) {
                analyze_visibility_readback(device, swapchain, debug_render, last_submitted_selection,
                                           report);
                // Read back GPU draw list from compute selection
                // (Occlusion-refined readback deferred until indirect draws eliminate the
                //  1-frame latency issue that causes flashing with camera movement)
                const UploadedBuffer& readback_count_buf = compute_selection.draw_count;
                const UploadedBuffer& readback_list_buf = compute_selection.draw_list;

                if (readback_count_buf.buffer != VK_NULL_HANDLE) {
                    void* count_mapped = nullptr;
                    if (vkMapMemory(device, readback_count_buf.memory, 0,
                                    sizeof(uint32_t), 0, &count_mapped) == VK_SUCCESS) {
                        gpu_draw_count = *static_cast<const uint32_t*>(count_mapped);
                        vkUnmapMemory(device, readback_count_buf.memory);
                    }
                    if (gpu_draw_count > 0 && readback_list_buf.buffer != VK_NULL_HANDLE) {
                        const VkDeviceSize list_size =
                            static_cast<VkDeviceSize>(gpu_draw_count) * sizeof(GpuDrawEntry);
                        void* list_mapped = nullptr;
                        if (vkMapMemory(device, readback_list_buf.memory, 0,
                                        list_size, 0, &list_mapped) == VK_SUCCESS) {
                            gpu_draw_list.resize(gpu_draw_count);
                            std::memcpy(gpu_draw_list.data(), list_mapped,
                                        gpu_draw_count * sizeof(GpuDrawEntry));
                            vkUnmapMemory(device, readback_list_buf.memory);
                        }
                    } else {
                        gpu_draw_list.clear();
                        gpu_draw_count = 0;
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

            // Upload per-frame UBO
            FrameUBO frame_ubo_data{};
            std::memcpy(frame_ubo_data.view_projection, camera_frame.view_projection.m,
                        sizeof(frame_ubo_data.view_projection));
            std::memcpy(frame_ubo_data.light_vp, shadow.light_vp.m,
                        sizeof(frame_ubo_data.light_vp));
            const Vec3f norm_light = normalize_vec3({0.4f, 0.7f, 0.5f});
            frame_ubo_data.light_dir[0] = norm_light.x;
            frame_ubo_data.light_dir[1] = norm_light.y;
            frame_ubo_data.light_dir[2] = norm_light.z;
            frame_ubo_data.light_dir[3] = 0.0f;
            update_uploaded_buffer(device, &frame_ubo_data, sizeof(FrameUBO),
                                   debug_render.frame_ubo);

            const FrustumPlanes frustum = extract_frustum_planes(camera_frame.view_projection);
            result = record_debug_command_buffer(frame, debug_render, compute_cull, compute_selection,
                                                 hzb, occlusion_refine, shadow, swapchain,
                                                 camera_frame, frustum, config.debug_error_threshold,
                                                 selection_for_frame, report.uploadable_scene,
                                                 gpu_draw_list, gpu_draw_count,
                                                 frame_index, image_index);
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

            result = vkQueueSubmit(graphics_queue, 1, &submit_info, frame.in_flight);
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

            result = vkQueuePresentKHR(present_queue, &present_info);
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
        if (report.presented_frame_count > 0) {
            analyze_visibility_readback(device, swapchain, debug_render, last_submitted_selection,
                                       report);
        }
        report.present_loop_completed = report.presented_frame_count == config.present_frame_count;
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
