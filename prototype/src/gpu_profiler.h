#pragma once
#include "vk_context.h"
#include <string>
#include <vector>

namespace meridian {

struct GpuTimerResult {
    std::string name;
    double ms;
};

struct GpuProfiler {
    VkQueryPool query_pool = VK_NULL_HANDLE;
    uint32_t query_count = 0;
    float timestamp_period = 0.0f; // nanoseconds per tick
    // Valid timestamp bits of the queue family the queries are written
    // from (64 = full-width). Deltas are computed modulo 2^bits.
    uint32_t timestamp_valid_bits = 0;
    std::vector<std::string> names;
};

VkResult create_gpu_profiler(VkPhysicalDevice physical_device, VkDevice device,
                              uint32_t queue_family, uint32_t max_timers, GpuProfiler& profiler);
void destroy_gpu_profiler(VkDevice device, GpuProfiler& profiler);
std::vector<GpuTimerResult> read_gpu_timers(VkDevice device, const GpuProfiler& profiler);

}  // namespace meridian
