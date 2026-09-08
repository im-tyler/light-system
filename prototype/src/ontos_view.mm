// ontos_view: thin Phase 3 stream consumer (JoltViewer pattern). Plays back an
// ontos v2 gravity stream and renders the bodies as instanced billboarded
// quads plus a faint 2x2 region grid. Zero coupling to the meshlet/.vgeo core.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <vulkan/vulkan.h>
#if defined(__APPLE__) && __has_include(<vulkan/vulkan_metal.h>)
#include <vulkan/vulkan_metal.h>
#endif
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include <shaderc/shaderc.hpp>

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f64 = double;

namespace {

struct StreamBody {
    u32 id = 0;
    u8 region = 0;
    u8 level = 0;
    f64 x = 0, y = 0, vx = 0, vy = 0, mass = 0;
};

struct StreamFrame {
    u64 tick = 0;
    u64 fine = 0;
    u64 coarse = 0;
    u8 region_level[4] = {1, 1, 1, 1};
    std::vector<StreamBody> bodies;
};

struct Stream {
    u32 body_count = 0;
    std::vector<StreamFrame> frames;
};

struct ByteView {
    const u8* data = nullptr;
    std::size_t size = 0;
};

bool take_u32(ByteView d, std::size_t& off, u32& out) {
    if (d.size - off < 4) return false;
    out = static_cast<u32>(d.data[off]) | (static_cast<u32>(d.data[off + 1]) << 8) |
          (static_cast<u32>(d.data[off + 2]) << 16) | (static_cast<u32>(d.data[off + 3]) << 24);
    off += 4;
    return true;
}

bool take_u64(ByteView d, std::size_t& off, u64& out) {
    if (d.size - off < 8) return false;
    u64 lo = 0;
    u64 hi = 0;
    for (int i = 0; i < 4; ++i) {
        lo |= static_cast<u64>(d.data[off + i]) << (8 * i);
        hi |= static_cast<u64>(d.data[off + 4 + i]) << (8 * i);
    }
    out = lo | (hi << 32);
    off += 8;
    return true;
}

bool take_f64(ByteView d, std::size_t& off, f64& out) {
    u64 bits = 0;
    if (!take_u64(d, off, bits)) return false;
    std::memcpy(&out, &bits, 8);
    return true;
}

u32 le32_at(ByteView d, std::size_t off) {
    return static_cast<u32>(d.data[off]) | (static_cast<u32>(d.data[off + 1]) << 8) |
           (static_cast<u32>(d.data[off + 2]) << 16) | (static_cast<u32>(d.data[off + 3]) << 24);
}

[[noreturn]] void stream_error(const char* what, std::size_t offset) {
    std::ostringstream message;
    message << what << " at offset " << offset;
    throw std::runtime_error(message.str());
}

void finalize_frame(StreamFrame& frame, bool has_snapshot, u64 snapshot_population,
                    u32 body_count, const u8 region_level[4]) {
    if (has_snapshot && snapshot_population != frame.bodies.size()) {
        std::ostringstream message;
        message << "tick " << frame.tick << " snapshot population " << snapshot_population
                << " != body records " << frame.bodies.size();
        throw std::runtime_error(message.str());
    }
    if (frame.bodies.size() != body_count) {
        std::ostringstream message;
        message << "tick " << frame.tick << " has " << frame.bodies.size()
                << " body records, expected " << body_count;
        throw std::runtime_error(message.str());
    }
    std::memcpy(frame.region_level, region_level, sizeof frame.region_level);
}

// Parsing logic ported from tools/ontos/ontos_stream_dump.cpp (record walk
// only; records are trusted, no re-simulation). The file is mmap'd read-only
// for the walk and unmapped again; only the parsed frames stay in RAM.
Stream parse_stream(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        throw std::runtime_error("cannot open stream file: " + path.string());
    }
    struct stat st {};
    if (::fstat(::fileno(f), &st) != 0 || st.st_size <= 0) {
        std::fclose(f);
        throw std::runtime_error("cannot stat stream file: " + path.string());
    }
    const std::size_t file_size = static_cast<std::size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, ::fileno(f), 0);
    std::fclose(f);
    if (mapped == MAP_FAILED) {
        throw std::runtime_error("cannot mmap stream file: " + path.string());
    }
    ByteView data{static_cast<const u8*>(mapped), file_size};

    if (data.size < 20 || std::memcmp(data.data, "ONTO", 4) != 0) {
        ::munmap(mapped, file_size);
        throw std::runtime_error("not an ontos v2 stream (bad magic or truncated header)");
    }
    const u32 version = le32_at(data, 4);
    const u32 world_w = le32_at(data, 8);
    const u32 world_h = le32_at(data, 12);
    const u32 body_count = le32_at(data, 16);
    if (version != 2) {
        ::munmap(mapped, file_size);
        std::ostringstream message;
        message << "unsupported stream version " << version << " (ontos_view requires v2)";
        throw std::runtime_error(message.str());
    }
    if (world_w != 128 || world_h != 128) {
        ::munmap(mapped, file_size);
        std::ostringstream message;
        message << "unsupported world size " << world_w << "x" << world_h;
        throw std::runtime_error(message.str());
    }
    if (body_count == 0 || body_count > 100000) {
        ::munmap(mapped, file_size);
        std::ostringstream message;
        message << "implausible body count " << body_count;
        throw std::runtime_error(message.str());
    }

    Stream stream;
    stream.body_count = body_count;
    u8 region_level[4] = {1, 1, 1, 1};
    bool has_snapshot = false;
    u64 snapshot_population = 0;
    std::size_t off = 20;

    try {
            while (off < data.size) {
            const std::size_t rec_start = off;
            const u8 tag = data.data[off++];
            switch (tag) {
                case 1: {
                    u64 t = 0;
                    if (!take_u64(data, off, t)) stream_error("truncated TickHeader", rec_start);
                    if (!stream.frames.empty()) {
                        finalize_frame(stream.frames.back(), has_snapshot, snapshot_population,
                                       body_count, region_level);
                    }
                    StreamFrame frame;
                    frame.tick = t;
                    stream.frames.push_back(std::move(frame));
                    has_snapshot = false;
                    snapshot_population = 0;
                } break;
                case 2: {
                    u64 p = 0;
                    if (!take_u64(data, off, p)) stream_error("truncated Snapshot", rec_start);
                    has_snapshot = true;
                    snapshot_population = p;
                } break;
                case 3: {
                    u64 t = 0;
                    u32 x = 0;
                    u32 y = 0;
                    if (!take_u64(data, off, t) || !take_u32(data, off, x) || !take_u32(data, off, y)) {
                        stream_error("truncated CellFlipped", rec_start);
                    }
                } break;
                case 4: {
                    u32 rx = 0;
                    u32 ry = 0;
                    if (!take_u32(data, off, rx) || !take_u32(data, off, ry) ||
                        data.size - off < 1) {
                        stream_error("truncated RegionLevel", rec_start);
                    }
                    const u8 lv = data.data[off++];
                    if (rx > 1 || ry > 1 || lv > 2) {
                        stream_error("bad RegionLevel", rec_start);
                    }
                    region_level[ry * 2 + rx] = lv;
                } break;
                case 5: {
                    u64 t = 0;
                    u32 rx = 0;
                    u32 ry = 0;
                    u64 p = 0;
                    u64 h = 0;
                    if (!take_u64(data, off, t) || !take_u32(data, off, rx) ||
                        !take_u32(data, off, ry) || data.size - off < 1) {
                        stream_error("truncated RegionState", rec_start);
                    }
                    const u8 lv = data.data[off++];
                    if (!take_u64(data, off, p) || !take_u64(data, off, h)) {
                        stream_error("truncated RegionState", rec_start);
                    }
                    if (rx > 1 || ry > 1 || lv > 2) {
                        stream_error("bad RegionState", rec_start);
                    }
                    if (stream.frames.empty()) {
                        stream_error("RegionState before any TickHeader", rec_start);
                    }
                    region_level[ry * 2 + rx] = lv;
                } break;
                case 6: {
                    u64 t = 0;
                    u32 bid = 0;
                    u8 reg = 0;
                    u8 lv = 0;
                    f64 x = 0, y = 0, vx = 0, vy = 0, mass = 0;
                    if (!take_u64(data, off, t) || !take_u32(data, off, bid) ||
                        data.size - off < 2) {
                        stream_error("truncated BodyState", rec_start);
                    }
                    reg = data.data[off++];
                    lv = data.data[off++];
                    if (!take_f64(data, off, x) || !take_f64(data, off, y) ||
                        !take_f64(data, off, vx) || !take_f64(data, off, vy) ||
                        !take_f64(data, off, mass)) {
                        stream_error("truncated BodyState", rec_start);
                    }
                    if (stream.frames.empty()) {
                        stream_error("BodyState before any TickHeader", rec_start);
                    }
                    StreamFrame& frame = stream.frames.back();
                    if (bid != frame.bodies.size() || bid >= body_count || lv > 2 ||
                        (reg > 3 && reg != 255) || t != frame.tick) {
                        stream_error("bad BodyState", rec_start);
                    }
                    StreamBody body;
                    body.id = bid;
                    body.region = reg;
                    body.level = lv;
                    body.x = x;
                    body.y = y;
                    body.vx = vx;
                    body.vy = vy;
                    body.mass = mass;
                    frame.bodies.push_back(body);
                } break;
                case 7: {
                    u64 t = 0;
                    u64 fine = 0;
                    u64 cn = 0;
                    f64 mass = 0, tpx = 0, tpy = 0, energy = 0;
                    if (!take_u64(data, off, t) || !take_u64(data, off, fine) ||
                        !take_u64(data, off, cn) || !take_f64(data, off, mass) ||
                        !take_f64(data, off, tpx) || !take_f64(data, off, tpy) ||
                        !take_f64(data, off, energy)) {
                        stream_error("truncated TotalsState", rec_start);
                    }
                    if (stream.frames.empty()) {
                        stream_error("TotalsState before any TickHeader", rec_start);
                    }
                    stream.frames.back().fine = fine;
                    stream.frames.back().coarse = cn;
                } break;
                case 8: {
                    if (data.size - off < 72) {
                        stream_error("truncated RegionCollapsed", rec_start);
                    }
                    off += 72;
                } break;
                case 9: {
                    if (data.size - off < 56) {
                        stream_error("truncated RegionMultipole", rec_start);
                    }
                    off += 56;
                } break;
                default: {
                    std::ostringstream message;
                    message << "unknown record tag " << tag << " at offset " << rec_start;
                    throw std::runtime_error(message.str());
                }
            }
            }
    } catch (...) {
        ::munmap(mapped, file_size);
        throw;
    }
    ::munmap(mapped, file_size);

    if (stream.frames.empty()) {
        throw std::runtime_error("stream contains no ticks");
    }
    finalize_frame(stream.frames.back(), has_snapshot, snapshot_population, body_count,
                   region_level);
    return stream;
}

void hsv_to_rgb(float h, float s, float v, float out[3]) {
    const float hp = h * 6.0f;
    const int hi = static_cast<int>(std::floor(hp)) % 6;
    const float f = hp - std::floor(hp);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    switch (hi) {
        case 0: out[0] = v; out[1] = t; out[2] = p; break;
        case 1: out[0] = q; out[1] = v; out[2] = p; break;
        case 2: out[0] = p; out[1] = v; out[2] = t; break;
        case 3: out[0] = p; out[1] = q; out[2] = v; break;
        case 4: out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

struct BodyInstance {
    float x, y;
    float half_extent, shape;
    float r, g, b, a;
};

struct ViewPush {
    float sx, sy, tx, ty;
    float z;
};

struct Camera2D {
    double cx = 64.0;
    double cy = 64.0;
    double zoom = 5.0;
};

struct WindowState {
    bool resized = false;
    Camera2D camera;
    int fb_width = 1280;
    int fb_height = 720;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* user_data) {
    (void)types;
    (void)user_data;
    const char* tag = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0
                          ? "VALIDATION ERROR"
                          : "VALIDATION WARNING";
    std::fprintf(stderr, "%s: %s\n", tag, data->pMessage);
    return VK_FALSE;
}

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
            const std::filesystem::path manifest =
                std::filesystem::path(candidate) / "VkLayer_khronos_validation.json";
            const std::filesystem::path library =
                std::filesystem::path(candidate) / ".." / ".." / ".." / "lib" /
                "libVkLayer_khronos_validation.dylib";
            if (!std::filesystem::exists(manifest)) {
                continue;
            }
            if (std::filesystem::exists(library)) {
                // Homebrew ships a bare library_path the loader cannot dlopen
                // outside default search paths; shadow it with an absolute path.
                std::ifstream input(manifest);
                std::ostringstream buffer;
                buffer << input.rdbuf();
                std::string text = buffer.str();
                const std::string from = "\"library_path\": \"libVkLayer_khronos_validation.dylib\"";
                const std::string to =
                    "\"library_path\": \"" + std::filesystem::absolute(library).string() + "\"";
                if (text.find(from) != std::string::npos) {
                    std::error_code ec;
                    const std::filesystem::path shadow_dir =
                        std::filesystem::temp_directory_path(ec) / "ontos-view-vklayer";
                    if (!std::filesystem::exists(shadow_dir)) {
                        std::filesystem::create_directories(shadow_dir, ec);
                    }
                    if (!ec) {
                        const std::filesystem::path shadow = shadow_dir / manifest.filename();
                        std::ofstream output(shadow, std::ios::trunc);
                        output << text.replace(text.find(from), from.size(), to);
                        setenv("VK_LAYER_PATH", shadow_dir.c_str(), 0);
                        break;
                    }
                }
            }
            setenv("VK_LAYER_PATH", candidate, 0);
            break;
        }
    }
#endif
}

std::filesystem::path resolve_shader_path(const char* name) {
    std::filesystem::path exe_dir;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string exe_path(size, '\0');
    std::error_code ec;
    if (_NSGetExecutablePath(exe_path.data(), &size) == 0) {
        const auto canonical = std::filesystem::canonical(exe_path, ec);
        if (!ec) exe_dir = canonical.parent_path();
    }
#endif
    std::vector<std::filesystem::path> candidates;
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir / ".." / "shaders" / name);
        candidates.push_back(exe_dir / "shaders" / name);
    }
    candidates.push_back(std::filesystem::current_path() / ".." / "shaders" / name);
    candidates.push_back(std::filesystem::path("shaders") / name);
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    throw std::runtime_error(std::string("shader not found: ") + name);
}

std::string load_shader_source(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open shader: " + path.string());
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::vector<uint32_t> compile_glsl(const std::string& source, shaderc_shader_kind kind,
                                   const char* name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    const shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(source, kind, name, options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error(std::string("shader compilation failed for ") + name + ": " +
                                 result.GetErrorMessage());
    }
    return {result.cbegin(), result.cend()};
}

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                          VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const bool supported = (type_bits & (1u << i)) != 0;
        const bool has_flags = (properties.memoryTypes[i].propertyFlags & required) == required;
        if (supported && has_flags) return i;
    }
    return std::numeric_limits<uint32_t>::max();
}

void create_host_buffer(VkPhysicalDevice physical_device, VkDevice device, const void* data,
                        VkDeviceSize size, VkBufferUsageFlags usage, bool persistent_map,
                        GpuBuffer& out) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &out.buffer);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateBuffer failed");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        find_memory_type(physical_device, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocate.memoryTypeIndex == std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("no host-visible memory type");
    }
    result = vkAllocateMemory(device, &allocate, nullptr, &out.memory);
    if (result != VK_SUCCESS) throw std::runtime_error("vkAllocateMemory failed");
    result = vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (result != VK_SUCCESS) throw std::runtime_error("vkBindBufferMemory failed");
    out.size = size;
    if (data != nullptr || persistent_map) {
        result = vkMapMemory(device, out.memory, 0, size, 0, &out.mapped);
        if (result != VK_SUCCESS) throw std::runtime_error("vkMapMemory failed");
        if (data != nullptr) {
            std::memcpy(out.mapped, data, static_cast<std::size_t>(size));
        }
        if (!persistent_map) {
            vkUnmapMemory(device, out.memory);
            out.mapped = nullptr;
        }
    }
}

void destroy_buffer(VkDevice device, GpuBuffer& buffer) {
    if (buffer.mapped != nullptr) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

struct Viewer {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphics_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkExtent2D extent{};


    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> framebuffers;
    VkImage msaa_color = VK_NULL_HANDLE;
    VkDeviceMemory msaa_color_memory = VK_NULL_HANDLE;
    VkImageView msaa_color_view = VK_NULL_HANDLE;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline body_pipeline = VK_NULL_HANDLE;
    VkPipeline line_pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    std::vector<VkSemaphore> render_finished_per_image;
    VkFence in_flight = VK_NULL_HANDLE;
    GpuBuffer quad_buffer;
    GpuBuffer line_buffer;
    GpuBuffer line_instance_buffer;
    // Double-buffered so the CPU fills slot (frame + 1) & 1 while the GPU may
    // still be reading the other slot from the previous submit.
    GpuBuffer body_instance_buffers[2];

    void destroy_extent_resources() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        for (VkFramebuffer fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        framebuffers.clear();
        if (msaa_color_view != VK_NULL_HANDLE) vkDestroyImageView(device, msaa_color_view, nullptr);
        msaa_color_view = VK_NULL_HANDLE;
        if (msaa_color != VK_NULL_HANDLE) vkDestroyImage(device, msaa_color, nullptr);
        msaa_color = VK_NULL_HANDLE;
        if (msaa_color_memory != VK_NULL_HANDLE) vkFreeMemory(device, msaa_color_memory, nullptr);
        msaa_color_memory = VK_NULL_HANDLE;
        if (depth_view != VK_NULL_HANDLE) vkDestroyImageView(device, depth_view, nullptr);
        depth_view = VK_NULL_HANDLE;
        if (depth_image != VK_NULL_HANDLE) vkDestroyImage(device, depth_image, nullptr);
        depth_image = VK_NULL_HANDLE;
        if (depth_memory != VK_NULL_HANDLE) vkFreeMemory(device, depth_memory, nullptr);
        depth_memory = VK_NULL_HANDLE;
    }

    void destroy_swapchain_views() {
        destroy_extent_resources();
        for (VkImageView view : image_views) vkDestroyImageView(device, view, nullptr);
        image_views.clear();
        images.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    void destroy() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        destroy_swapchain_views();
        destroy_buffer(device, body_instance_buffers[0]);
        destroy_buffer(device, body_instance_buffers[1]);
        destroy_buffer(device, line_instance_buffer);
        destroy_buffer(device, line_buffer);
        destroy_buffer(device, quad_buffer);
        if (in_flight != VK_NULL_HANDLE) vkDestroyFence(device, in_flight, nullptr);
        for (VkSemaphore semaphore : render_finished_per_image) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        render_finished_per_image.clear();
        if (image_available != VK_NULL_HANDLE) vkDestroySemaphore(device, image_available, nullptr);
        if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
        if (line_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, line_pipeline, nullptr);
        if (body_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, body_pipeline, nullptr);
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, render_pass, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (debug_messenger != VK_NULL_HANDLE) {
            auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_messenger != nullptr) {
                destroy_messenger(instance, debug_messenger, nullptr);
            }
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

void create_image(Viewer& v, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                  GpuImage& out) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {v.extent.width, v.extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = v.samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateImage(v.device, &image_info, nullptr, &out.image);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateImage failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(v.device, out.image, &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        find_memory_type(v.physical_device, requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate.memoryTypeIndex == std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("no device-local memory type");
    }
    result = vkAllocateMemory(v.device, &allocate, nullptr, &out.memory);
    if (result != VK_SUCCESS) throw std::runtime_error("vkAllocateMemory failed");
    result = vkBindImageMemory(v.device, out.image, out.memory, 0);
    if (result != VK_SUCCESS) throw std::runtime_error("vkBindImageMemory failed");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(v.device, &view_info, nullptr, &out.view);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateImageView failed");
}
void create_swapchain(Viewer& v, GLFWwindow* window) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.physical_device, v.surface, &capabilities);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v.physical_device, v.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(v.physical_device, v.surface, &format_count,
                                         formats.data());
    VkSurfaceFormatKHR surface_format = formats.front();
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = format;
            break;
        }
    }
    if (surface_format.format != v.swapchain_format) {
        throw std::runtime_error("surface does not expose B8G8R8A8_UNORM sRGB");
    }

    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(v.physical_device, v.surface, &present_mode_count,
                                              nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(v.physical_device, v.surface, &present_mode_count,
                                              present_modes.data());
    VkPresentModeKHR present_mode = present_modes.front();
    for (VkPresentModeKHR mode : present_modes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR) {
            present_mode = mode;
            break;
        }
    }

    VkExtent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = capabilities.currentExtent;
    } else {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        extent.width = std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(height),
                                   capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = v.surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateSwapchainKHR(v.device, &create_info, nullptr, &v.swapchain);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateSwapchainKHR failed");

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(v.device, v.swapchain, &count, nullptr);
    v.images.resize(count);
    vkGetSwapchainImagesKHR(v.device, v.swapchain, &count, v.images.data());
    v.extent = extent;

    v.image_views.resize(v.images.size());
    for (size_t i = 0; i < v.images.size(); ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = v.images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = v.swapchain_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(v.device, &view_info, nullptr, &v.image_views[i]);
        if (result != VK_SUCCESS) throw std::runtime_error("vkCreateImageView failed");
    }
}

// Creates the extent-sized attachments (MSAA color resolve source + depth)
// and the per-swapchain-image framebuffers. Must run after create_swapchain
// and again after every resize.
void create_extent_resources(Viewer& v) {
    if (v.samples != VK_SAMPLE_COUNT_1_BIT) {
        GpuImage msaa{};
        create_image(v, v.swapchain_format,
                     VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, msaa);
        v.msaa_color = msaa.image;
        v.msaa_color_memory = msaa.memory;
        v.msaa_color_view = msaa.view;
    }
    {
        GpuImage depth{};
        create_image(v, v.depth_format,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, depth);
        v.depth_image = depth.image;
        v.depth_memory = depth.memory;
        v.depth_view = depth.view;
    }

    v.framebuffers.resize(v.images.size());
    for (size_t i = 0; i < v.images.size(); ++i) {
        const VkImageView attachments[3] = {v.msaa_color_view, v.image_views[i], v.depth_view};
        const uint32_t attachment_count = v.samples != VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
        const VkImageView* used_attachments =
            v.samples != VK_SAMPLE_COUNT_1_BIT ? attachments : attachments + 1;
        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = v.render_pass;
        fb_info.attachmentCount = attachment_count;
        fb_info.pAttachments = used_attachments;
        fb_info.width = v.extent.width;
        fb_info.height = v.extent.height;
        fb_info.layers = 1;
        const VkResult result =
            vkCreateFramebuffer(v.device, &fb_info, nullptr, &v.framebuffers[i]);
        if (result != VK_SUCCESS) throw std::runtime_error("vkCreateFramebuffer failed");
    }
}

VkPipeline create_pipeline(Viewer& v, VkPrimitiveTopology topology, bool depth_write) {
    const std::string vert_source = load_shader_source(resolve_shader_path("ontos_bodies.vert"));
    const std::string frag_source = load_shader_source(resolve_shader_path("ontos_bodies.frag"));
    const std::vector<uint32_t> vert_spirv =
        compile_glsl(vert_source, shaderc_vertex_shader, "ontos_bodies.vert");
    const std::vector<uint32_t> frag_spirv =
        compile_glsl(frag_source, shaderc_fragment_shader, "ontos_bodies.frag");

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = vert_spirv.size() * sizeof(uint32_t);
    module_info.pCode = vert_spirv.data();
    VkShaderModule vert_module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(v.device, &module_info, nullptr, &vert_module);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateShaderModule failed");
    module_info.codeSize = frag_spirv.size() * sizeof(uint32_t);
    module_info.pCode = frag_spirv.data();
    VkShaderModule frag_module = VK_NULL_HANDLE;
    result = vkCreateShaderModule(v.device, &module_info, nullptr, &frag_module);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateShaderModule failed");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(float) * 2;
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(BodyInstance);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attributes[4]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 1;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(BodyInstance, x);
    attributes[2].location = 2;
    attributes[2].binding = 1;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(BodyInstance, half_extent);
    attributes[3].location = 3;
    attributes[3].binding = 1;
    attributes[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[3].offset = offsetof(BodyInstance, r);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 2;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = topology;
    // Draws are non-indexed, so restart cannot trigger; MoltenVK warns when
    // it is disabled for strips because Metal has no such switch, while list
    // topologies forbid enabling it without primitiveTopologyListRestart.
    input_assembly.primitiveRestartEnable =
        topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = v.samples;

    // Both pipelines depth-test LESS with z pushed per pipeline: grid lines
    // sit at z ~ 0.5, body discs slightly nearer, so blended bodies always
    // layer over the grid regardless of draw order (bodies never write depth,
    // keeping alpha blending commutative with painter's order).
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = v.pipeline_layout;
    pipeline_info.renderPass = v.render_pass;
    pipeline_info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    result = vkCreateGraphicsPipelines(v.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &pipeline);
    vkDestroyShaderModule(v.device, vert_module, nullptr);
    vkDestroyShaderModule(v.device, frag_module, nullptr);
    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateGraphicsPipelines failed");
    return pipeline;
}

void fill_body_instances(const StreamFrame& frame, BodyInstance* instances) {
    static const float region_hue[4] = {0.62f, 0.08f, 0.33f, 0.78f};
    for (size_t i = 0; i < frame.bodies.size(); ++i) {
        const StreamBody& body = frame.bodies[i];
        BodyInstance& inst = instances[i];
        float rgb[3];
        if (body.region <= 3) {
            const float s = body.level == 1 ? 0.85f : 0.22f;
            const float val = body.level == 1 ? 1.0f : 0.60f;
            hsv_to_rgb(region_hue[body.region], s, val, rgb);
        } else {
            const float g = body.level == 1 ? 0.75f : 0.45f;
            rgb[0] = g;
            rgb[1] = g;
            rgb[2] = g;
        }
        inst.x = static_cast<float>(body.x);
        inst.y = static_cast<float>(body.y);
        const float log_size = 0.55f + 1.15f * static_cast<float>(std::log2(body.mass));
        inst.half_extent = std::clamp(log_size, 0.35f, 2.4f);
        inst.shape = 1.0f;
        inst.r = rgb[0];
        inst.g = rgb[1];
        inst.b = rgb[2];
        inst.a = 0.95f;
    }
}

ViewPush compute_view_push(const Camera2D& camera, VkExtent2D extent, float z) {
    const float sx = static_cast<float>(2.0 * camera.zoom / extent.width);
    const float sy = static_cast<float>(2.0 * camera.zoom / extent.height);
    ViewPush push{};
    push.sx = sx;
    push.sy = sy;
    push.tx = static_cast<float>(-camera.cx * sx);
    push.ty = static_cast<float>(-camera.cy * sy);
    push.z = z;
    return push;
}

void print_usage() {
    std::fprintf(stderr,
                 "usage: ontos_view <stream-file> [--validate] [--frames N]\n"
                 "  --validate   enable Vulkan validation layers\n"
                 "  --frames N   non-interactive: render N frames (one tick per frame),\n"
                 "               print per-tick body counts, exit 0\n"
                 "  keys: SPACE pause  +/- rate  R restart  ESC quit  drag pan  wheel zoom"
                 "  WASD pan\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::filesystem::path stream_path;
    bool seed_seen = false;
    bool validate = false;
    bool frames_requested = false;
    uint32_t frame_limit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--validate") {
            validate = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frame_limit = static_cast<uint32_t>(std::atoi(argv[++i]));
            frames_requested = true;
        } else if (!arg.empty() && arg[0] != '-') {
            if (stream_path.empty()) {
                stream_path = argv[i];
            } else {
                // Optional positional seed, accepted and ignored: the viewer
                // plays back records only and never re-simulates.
                seed_seen = true;
            }
        } else {
            print_usage();
            return 1;
        }
    }
    (void)seed_seen;
    if (stream_path.empty() || (frames_requested && frame_limit == 0)) {
        print_usage();
        return 1;
    }

    GLFWwindow* window = nullptr;
    try {
        const Stream stream = parse_stream(stream_path);

        configure_macos_moltenvk_environment();

        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("glfwInit failed");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        // Hidden windows cannot back a MoltenVK surface on macOS, so the
        // non-interactive mode still shows the window; it closes itself after
        // the requested frame count.
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        window = glfwCreateWindow(1280, 720, "ontos_view", nullptr, nullptr);
        if (window == nullptr) {
            glfwTerminate();
            throw std::runtime_error("glfwCreateWindow failed");
        }

        WindowState state;
        glfwSetWindowUserPointer(window, &state);
        glfwSetFramebufferSizeCallback(
            window, [](GLFWwindow* w, int width, int height) {
                WindowState* s = static_cast<WindowState*>(glfwGetWindowUserPointer(w));
                s->resized = true;
                if (width > 0 && height > 0) {
                    s->fb_width = width;
                    s->fb_height = height;
                }
            });
        glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoffset) {
            WindowState* s = static_cast<WindowState*>(glfwGetWindowUserPointer(w));
            Camera2D& cam = s->camera;
            double mx = 0.0;
            double my = 0.0;
            glfwGetCursorPos(w, &mx, &my);
            const double ndc_x = 2.0 * mx / s->fb_width - 1.0;
            const double ndc_y = 1.0 - 2.0 * my / s->fb_height;
            const double wx = cam.cx + ndc_x * s->fb_width / (2.0 * cam.zoom);
            const double wy = cam.cy + ndc_y * s->fb_height / (2.0 * cam.zoom);
            cam.zoom *= std::pow(1.15, -yoffset);
            cam.zoom = std::clamp(cam.zoom, 0.05, 40.0);
            cam.cx = wx - ndc_x * s->fb_width / (2.0 * cam.zoom);
            cam.cy = wy - ndc_y * s->fb_height / (2.0 * cam.zoom);
        });

        uint32_t glfw_extension_count = 0;
        const char** glfw_extensions =
            glfwGetRequiredInstanceExtensions(&glfw_extension_count);
        std::vector<const char*> instance_extensions;
        if (glfw_extensions != nullptr && glfw_extension_count > 0) {
            instance_extensions.assign(glfw_extensions, glfw_extensions + glfw_extension_count);
        } else {
#if defined(__APPLE__)
            // This Homebrew GLFW reports no Vulkan support; mirror
            // meridian_vk_bootstrap and drive the metal surface by hand.
            instance_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            instance_extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#else
            throw std::runtime_error("GLFW did not report required Vulkan instance extensions");
#endif
        }
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        std::vector<const char*> layers;
        if (validate) {
            uint32_t layer_count = 0;
            vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            std::vector<VkLayerProperties> available(layer_count);
            vkEnumerateInstanceLayerProperties(&layer_count, available.data());
            for (const VkLayerProperties& layer : available) {
                if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    break;
                }
            }
            if (layers.empty()) {
                std::fprintf(stderr, "warning: validation layer not found, continuing without\n");
                validate = false;
            }
        }

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "ontos_view";
        app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.pEngineName = "light-system";
        app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        app_info.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app_info;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
        instance_info.ppEnabledExtensionNames = instance_extensions.data();
        instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
        instance_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
        instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

        Viewer v;
        VkResult result = vkCreateInstance(&instance_info, nullptr, &v.instance);
        if (result != VK_SUCCESS) {
            v.instance = VK_NULL_HANDLE;
            throw std::runtime_error("vkCreateInstance failed");
        }

        if (validate) {
            auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(v.instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_messenger != nullptr) {
                VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
                messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
                messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                messenger_info.pfnUserCallback = &debug_callback;
                create_messenger(v.instance, &messenger_info, nullptr, &v.debug_messenger);
            }
        }

        result = VK_ERROR_INITIALIZATION_FAILED;
#if defined(__APPLE__)
        {
            NSWindow* cocoa_window = glfwGetCocoaWindow(window);
            if (cocoa_window != nil) {
                NSView* cocoa_view = [cocoa_window contentView];
                if (cocoa_view != nil) {
                    [cocoa_view setWantsLayer:YES];
                    CAMetalLayer* metal_layer = [CAMetalLayer layer];
                    [cocoa_view setLayer:metal_layer];
                    VkMetalSurfaceCreateInfoEXT create_info{};
                    create_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
                    create_info.pLayer = metal_layer;
                    result = vkCreateMetalSurfaceEXT(v.instance, &create_info, nullptr,
                                                     &v.surface);
                }
            }
        }
#else
        result = glfwCreateWindowSurface(v.instance, window, nullptr, &v.surface);
#endif
        if (result != VK_SUCCESS) {
            v.surface = VK_NULL_HANDLE;
            std::ostringstream message;
            message << "surface creation failed with code " << result;
            throw std::runtime_error(message.str());
        }

        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(v.instance, &device_count, nullptr);
        if (device_count == 0) throw std::runtime_error("no Vulkan physical devices");
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(v.instance, &device_count, devices.data());

        bool found = false;
        for (VkPhysicalDevice candidate : devices) {
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (uint32_t i = 0; i < family_count; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
                VkBool32 present_supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, v.surface, &present_supported);
                if (present_supported != VK_TRUE) continue;

                uint32_t extension_count = 0;
                vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count,
                                                     nullptr);
                std::vector<VkExtensionProperties> extensions(extension_count);
                vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count,
                                                     extensions.data());
                bool has_swapchain = false;
                bool has_portability_subset = false;
                for (const VkExtensionProperties& extension : extensions) {
                    if (std::strcmp(extension.extensionName,
                                    VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                        has_swapchain = true;
                    } else if (std::strcmp(extension.extensionName,
                                           "VK_KHR_portability_subset") == 0) {
                        has_portability_subset = true;
                    }
                }
                if (!has_swapchain) continue;

                const float priority = 1.0f;
                VkDeviceQueueCreateInfo queue_info{};
                queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                queue_info.queueFamilyIndex = i;
                queue_info.queueCount = 1;
                queue_info.pQueuePriorities = &priority;

                std::vector<const char*> device_extensions;
                device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                if (has_portability_subset) {
                    device_extensions.push_back("VK_KHR_portability_subset");
                }

                VkDeviceCreateInfo device_info{};
                device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                device_info.queueCreateInfoCount = 1;
                device_info.pQueueCreateInfos = &queue_info;
                device_info.enabledExtensionCount =
                    static_cast<uint32_t>(device_extensions.size());
                device_info.ppEnabledExtensionNames = device_extensions.data();

                result = vkCreateDevice(candidate, &device_info, nullptr, &v.device);
                if (result != VK_SUCCESS) {
                    v.device = VK_NULL_HANDLE;
                    continue;
                }
                v.physical_device = candidate;
                v.graphics_family = i;
                vkGetDeviceQueue(v.device, i, 0, &v.queue);
                found = true;
                break;
            }
            if (found) break;
        }
        if (!found) throw std::runtime_error("no compatible Vulkan device found");

        // Pick a depth format and the MSAA sample count supported for it.
        {
            const VkFormat depth_candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                                 VK_FORMAT_D16_UNORM};
            bool depth_ok = false;
            for (VkFormat candidate : depth_candidates) {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(v.physical_device, candidate, &properties);
                if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
                    0) {
                    v.depth_format = candidate;
                    depth_ok = true;
                    break;
                }
            }
            if (!depth_ok) throw std::runtime_error("no supported depth format");

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(v.physical_device, &properties);
            const VkSampleCountFlags color_samples = properties.limits.framebufferColorSampleCounts;
            const VkSampleCountFlags depth_samples = properties.limits.framebufferDepthSampleCounts;
            const VkSampleCountFlags shared = color_samples & depth_samples;
            v.samples = VK_SAMPLE_COUNT_1_BIT;
            for (VkSampleCountFlagBits candidate : {VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT,
                                                    VK_SAMPLE_COUNT_2_BIT}) {
                if ((shared & candidate) != 0) {
                    v.samples = candidate;
                    break;
                }
            }
        }

        {
            // Attachments: [0] color (MSAA when enabled, else the swapchain
            // image directly), [1] resolve target (swapchain, MSAA path only),
            // [2] depth (MSAA sample count or 1).
            VkAttachmentDescription attachments[3]{};
            attachments[0].format = v.swapchain_format;
            attachments[0].samples = v.samples;
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[0].storeOp = v.samples != VK_SAMPLE_COUNT_1_BIT
                                         ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                         : VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            attachments[1].format = v.swapchain_format;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            attachments[2].format = v.depth_format;
            attachments[2].samples = v.samples;
            attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            const uint32_t attachment_count = v.samples != VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
            VkAttachmentReference color_ref{};
            color_ref.attachment = 0;
            color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference resolve_ref{};
            resolve_ref.attachment = 1;
            resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference depth_ref{};
            depth_ref.attachment = v.samples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
            depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_ref;
            subpass.pResolveAttachments =
                v.samples != VK_SAMPLE_COUNT_1_BIT ? &resolve_ref : nullptr;
            subpass.pDepthStencilAttachment = &depth_ref;

            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            VkRenderPassCreateInfo render_pass_info{};
            render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            render_pass_info.attachmentCount = attachment_count;
            render_pass_info.pAttachments = attachments;
            render_pass_info.subpassCount = 1;
            render_pass_info.pSubpasses = &subpass;
            render_pass_info.dependencyCount = 1;
            render_pass_info.pDependencies = &dependency;

            result = vkCreateRenderPass(v.device, &render_pass_info, nullptr, &v.render_pass);
            if (result != VK_SUCCESS) throw std::runtime_error("vkCreateRenderPass failed");
        }

        {
            VkPushConstantRange range{};
            range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            range.offset = 0;
            range.size = sizeof(ViewPush);
            VkPipelineLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layout_info.pushConstantRangeCount = 1;
            layout_info.pPushConstantRanges = &range;
            result = vkCreatePipelineLayout(v.device, &layout_info, nullptr, &v.pipeline_layout);
            if (result != VK_SUCCESS) throw std::runtime_error("vkCreatePipelineLayout failed");
        }

        v.body_pipeline =
            create_pipeline(v, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, /*depth_write=*/false);
        v.line_pipeline =
            create_pipeline(v, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, /*depth_write=*/true);
        create_swapchain(v, window);
        create_extent_resources(v);

        {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = v.graphics_family;
            result = vkCreateCommandPool(v.device, &pool_info, nullptr, &v.command_pool);
            if (result != VK_SUCCESS) throw std::runtime_error("vkCreateCommandPool failed");

            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = v.command_pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            result = vkAllocateCommandBuffers(v.device, &allocate, &v.command_buffer);
            if (result != VK_SUCCESS) throw std::runtime_error("vkAllocateCommandBuffers failed");

            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            result = vkCreateSemaphore(v.device, &semaphore_info, nullptr, &v.image_available);
            if (result != VK_SUCCESS) throw std::runtime_error("vkCreateSemaphore failed");
            v.render_finished_per_image.resize(v.images.size(), VK_NULL_HANDLE);
            for (uint32_t i = 0; i < v.render_finished_per_image.size(); ++i) {
                result = vkCreateSemaphore(v.device, &semaphore_info, nullptr,
                                           &v.render_finished_per_image[i]);
                if (result != VK_SUCCESS) throw std::runtime_error("vkCreateSemaphore failed");
            }

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(v.device, &fence_info, nullptr, &v.in_flight);
            if (result != VK_SUCCESS) throw std::runtime_error("vkCreateFence failed");

            const float quad_corners[4][2] = {
                {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
            create_host_buffer(v.physical_device, v.device, quad_corners, sizeof(quad_corners),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, v.quad_buffer);

            const float grid_lines[12][2] = {
                {0.0f, 0.0f},     {128.0f, 0.0f},    {128.0f, 0.0f},   {128.0f, 128.0f},
                {128.0f, 128.0f}, {0.0f, 128.0f},    {0.0f, 128.0f},   {0.0f, 0.0f},
                {64.0f, 0.0f},    {64.0f, 128.0f},   {0.0f, 64.0f},    {128.0f, 64.0f},
            };
            create_host_buffer(v.physical_device, v.device, grid_lines, sizeof(grid_lines),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, v.line_buffer);

            BodyInstance line_instance{};
            line_instance.half_extent = 0.0f;
            line_instance.shape = 0.0f;
            line_instance.r = 0.45f;
            line_instance.g = 0.47f;
            line_instance.b = 0.52f;
            line_instance.a = 0.35f;
            create_host_buffer(v.physical_device, v.device, &line_instance, sizeof(BodyInstance),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, v.line_instance_buffer);

            for (uint32_t slot = 0; slot < 2; ++slot) {
                create_host_buffer(v.physical_device, v.device, nullptr,
                                   static_cast<VkDeviceSize>(stream.body_count) *
                                       sizeof(BodyInstance),
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true,
                                   v.body_instance_buffers[slot]);
            }
        }

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);
        if (state.fb_width > 0 && state.fb_height > 0) {
            state.camera.zoom = 0.9 * std::min(state.fb_width, state.fb_height) / 128.0;
        }

        uint32_t frames_per_tick = 8;
        uint32_t subframe = 0;
        bool paused = false;
        bool space_was_down = false;
        bool r_was_down = false;
        bool plus_was_down = false;
        bool minus_was_down = false;
        size_t tick_index = 0;
        bool tick_changed = true;
        double last_time = glfwGetTime();
        double ema_ms = 16.7;
        u64 ticks_shown = 0;
        bool printed_tick20 = false;
        double title_timer = 0.0;
        bool drag_active = false;
        double last_cursor_x = 0.0;
        double last_cursor_y = 0.0;
        double time_sum = 0.0;
        uint32_t timed_frames = 0;
        uint32_t frame_number = 0;

        while (frames_requested ? (frame_number < frame_limit)
                                : (glfwWindowShouldClose(window) != GLFW_TRUE)) {
            glfwPollEvents();
            if (glfwWindowShouldClose(window) == GLFW_TRUE) break;
            ++frame_number;

            if (!frames_requested) {
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                const bool space_down = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
                if (space_down && !space_was_down) paused = !paused;
                space_was_down = space_down;

                const bool r_down = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
                if (r_down && !r_was_down) {
                    tick_index = 0;
                    subframe = 0;
                    tick_changed = true;
                }
                r_was_down = r_down;

                const bool plus_down = glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS ||
                                       glfwGetKey(window, GLFW_KEY_KP_ADD) == GLFW_PRESS;
                if (plus_down && !plus_was_down && frames_per_tick > 1) frames_per_tick /= 2;
                plus_was_down = plus_down;

                const bool minus_down = glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS ||
                                        glfwGetKey(window, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS;
                if (minus_down && !minus_was_down && frames_per_tick < 256) frames_per_tick *= 2;
                minus_was_down = minus_down;

                double cx = 0.0;
                double cy = 0.0;
                glfwGetCursorPos(window, &cx, &cy);
                const bool button_down =
                    glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                if (button_down && drag_active) {
                    state.camera.cx -= (cx - last_cursor_x) / state.camera.zoom;
                    state.camera.cy += (cy - last_cursor_y) / state.camera.zoom;
                }
                drag_active = button_down;
                last_cursor_x = cx;
                last_cursor_y = cy;

                const double now = glfwGetTime();
                const double dt = std::max(now - last_time, 0.0);
                last_time = now;
                const double pan = 400.0 * dt / state.camera.zoom;
                if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) state.camera.cy += pan;
                if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) state.camera.cy -= pan;
                if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) state.camera.cx -= pan;
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) state.camera.cx += pan;
            }

            if (!paused) {
                if (frames_requested ? frame_number > 1 : subframe + 1 >= frames_per_tick) {
                    subframe = 0;
                    tick_index = (tick_index + 1) % stream.frames.size();
                    tick_changed = true;
                } else {
                    ++subframe;
                }
            }

            const StreamFrame& frame = stream.frames[tick_index];

            if (state.resized) {
                state.resized = false;
                vkDeviceWaitIdle(v.device);
                for (VkSemaphore semaphore : v.render_finished_per_image) {
                    vkDestroySemaphore(v.device, semaphore, nullptr);
                }
                v.render_finished_per_image.clear();
                v.destroy_swapchain_views();
                create_swapchain(v, window);
                create_extent_resources(v);
                VkSemaphoreCreateInfo semaphore_info{};
                semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                v.render_finished_per_image.resize(v.images.size(), VK_NULL_HANDLE);
                for (uint32_t i = 0; i < v.render_finished_per_image.size(); ++i) {
                    result = vkCreateSemaphore(v.device, &semaphore_info, nullptr,
                                               &v.render_finished_per_image[i]);
                    if (result != VK_SUCCESS) throw std::runtime_error("vkCreateSemaphore failed");
                }
            }

            const double frame_start = glfwGetTime();
            vkWaitForFences(v.device, 1, &v.in_flight, VK_TRUE, UINT64_MAX);
            vkResetFences(v.device, 1, &v.in_flight);

            // Fill the slot the previous submit did NOT read: the fence above
            // guarantees the frame-before-last retired, so this slot's bytes
            // are no longer in flight.
            GpuBuffer& instance_slot = v.body_instance_buffers[frame_number & 1];
            if (tick_changed) {
                fill_body_instances(frame, static_cast<BodyInstance*>(instance_slot.mapped));
                ticks_shown += 1;
                std::printf("tick %" PRIu64 " bodies=%zu fine=%" PRIu64 " coarse=%" PRIu64 "\n",
                            frame.tick, frame.bodies.size(), frame.fine, frame.coarse);
                if (frame.tick == 20 && !printed_tick20) {
                    printed_tick20 = true;
                    for (const StreamBody& body : frame.bodies) {
                        std::printf(
                            "tick20 body=%" PRIu32 " region=%u level=%u x=%.17g y=%.17g vx=%.17g"
                            " vy=%.17g mass=%.17g\n",
                            body.id, body.region, body.level, body.x, body.y, body.vx, body.vy,
                            body.mass);
                    }
                }
                tick_changed = false;
            }

            uint32_t image_index = 0;
            result = vkAcquireNextImageKHR(v.device, v.swapchain, UINT64_MAX, v.image_available,
                                           VK_NULL_HANDLE, &image_index);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                state.resized = true;
                continue;
            }
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                throw std::runtime_error("vkAcquireNextImageKHR failed");
            }

            vkResetCommandBuffer(v.command_buffer, 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(v.command_buffer, &begin);
            if (result != VK_SUCCESS) throw std::runtime_error("vkBeginCommandBuffer failed");

            VkClearValue clears[3]{};
            clears[0].color = {{0.05f, 0.06f, 0.08f, 1.0f}};
            uint32_t clear_count = 0;
            if (v.samples != VK_SAMPLE_COUNT_1_BIT) {
                // [0] MSAA color clear, [1] resolve (dont-care), [2] depth clear
                clears[2].depthStencil = {1.0f, 0};
                clear_count = 3;
            } else {
                // [0] color clear, [1] depth clear
                clears[1].depthStencil = {1.0f, 0};
                clear_count = 2;
            }
            VkRenderPassBeginInfo render_begin{};
            render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            render_begin.renderPass = v.render_pass;
            render_begin.framebuffer = v.framebuffers[image_index];
            render_begin.renderArea.extent = v.extent;
            render_begin.clearValueCount = clear_count;
            render_begin.pClearValues = clears;
            vkCmdBeginRenderPass(v.command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width = static_cast<float>(v.extent.width);
            viewport.height = static_cast<float>(v.extent.height);
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent = v.extent;
            vkCmdSetViewport(v.command_buffer, 0, 1, &viewport);
            vkCmdSetScissor(v.command_buffer, 0, 1, &scissor);

            const VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(v.command_buffer, 0, 1, &v.line_buffer.buffer, &offsets[0]);
            vkCmdBindVertexBuffers(v.command_buffer, 1, 1, &v.line_instance_buffer.buffer,
                                   &offsets[1]);
            vkCmdBindPipeline(v.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, v.line_pipeline);
            const ViewPush line_push = compute_view_push(state.camera, v.extent, 0.5f);
            vkCmdPushConstants(v.command_buffer, v.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(ViewPush), &line_push);
            vkCmdDraw(v.command_buffer, 12, 1, 0, 0);

            vkCmdBindVertexBuffers(v.command_buffer, 0, 1, &v.quad_buffer.buffer, &offsets[0]);
            vkCmdBindVertexBuffers(v.command_buffer, 1, 1, &instance_slot.buffer, &offsets[1]);
            vkCmdBindPipeline(v.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, v.body_pipeline);
            const ViewPush body_push = compute_view_push(state.camera, v.extent, 0.25f);
            vkCmdPushConstants(v.command_buffer, v.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(ViewPush), &body_push);
            vkCmdDraw(v.command_buffer, 4, static_cast<uint32_t>(frame.bodies.size()), 0, 0);

            vkCmdEndRenderPass(v.command_buffer);
            result = vkEndCommandBuffer(v.command_buffer);
            if (result != VK_SUCCESS) throw std::runtime_error("vkEndCommandBuffer failed");

            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &v.image_available;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &v.command_buffer;
            VkSemaphore signal_semaphore =
                image_index < v.render_finished_per_image.size()
                    ? v.render_finished_per_image[image_index]
                    : v.render_finished_per_image.front();
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &signal_semaphore;
            result = vkQueueSubmit(v.queue, 1, &submit, v.in_flight);
            if (result != VK_SUCCESS) throw std::runtime_error("vkQueueSubmit failed");

            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &signal_semaphore;
            present.swapchainCount = 1;
            present.pSwapchains = &v.swapchain;
            present.pImageIndices = &image_index;
            result = vkQueuePresentKHR(v.queue, &present);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
                state.resized = true;
            } else if (result != VK_SUCCESS) {
                throw std::runtime_error("vkQueuePresentKHR failed");
            }

            const double frame_end = glfwGetTime();
            const double ms = (frame_end - frame_start) * 1000.0;
            ema_ms = ema_ms * 0.9 + ms * 0.1;
            if (frame_number > 3) {
                time_sum += ms;
                timed_frames += 1;
            }

            if (!frames_requested) {
                title_timer += ms * 0.001;
                if (title_timer > 0.25) {
                    title_timer = 0.0;
                    char title[256];
                    std::snprintf(title, sizeof(title),
                                  "ontos_view  tick %" PRIu64 " / %zu  bodies=%zu  fine=%" PRIu64
                                  "  coarse=%" PRIu64 "  ms=%.2f  rate=1/%u%s",
                                  frame.tick, stream.frames.size(), frame.bodies.size(),
                                  frame.fine, frame.coarse, ema_ms, frames_per_tick,
                                  paused ? "  [paused]" : "");
                    glfwSetWindowTitle(window, title);
                }
            }
        }

        const double avg_ms = timed_frames > 0 ? time_sum / timed_frames : 0.0;
        const StreamFrame& last_frame = stream.frames[tick_index];
        std::printf("OK ticks=%" PRIu64 " bodies=%zu fine=%" PRIu64 " coarse=%" PRIu64
                    " frames=%u avg_ms=%.3f\n",
                    ticks_shown, last_frame.bodies.size(), last_frame.fine, last_frame.coarse,
                    timed_frames, avg_ms);

        vkDeviceWaitIdle(v.device);
        v.destroy();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    } catch (const std::exception& error) {
        if (window != nullptr) glfwDestroyWindow(window);
        glfwTerminate();
        std::fprintf(stderr, "error: %s\n", error.what());
        return 2;
    }
}
