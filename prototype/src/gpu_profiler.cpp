#include "gpu_profiler.h"

#if MERIDIAN_VK_CONTEXT_HAS_VULKAN

namespace meridian {

VkResult create_gpu_profiler(VkPhysicalDevice physical_device, VkDevice device,
                              uint32_t max_timers, GpuProfiler& profiler) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    if (!properties.limits.timestampComputeAndGraphics) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    profiler.timestamp_period = properties.limits.timestampPeriod;
    profiler.query_count = max_timers * 2; // start + end per timer

    VkQueryPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    pool_info.queryCount = profiler.query_count;

    VkResult result = vkCreateQueryPool(device, &pool_info, nullptr, &profiler.query_pool);
    if (result != VK_SUCCESS) {
        profiler.query_pool = VK_NULL_HANDLE;
        profiler.query_count = 0;
        return result;
    }

    profiler.names.resize(max_timers);
    return VK_SUCCESS;
}

void destroy_gpu_profiler(VkDevice device, GpuProfiler& profiler) {
    if (profiler.query_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, profiler.query_pool, nullptr);
        profiler.query_pool = VK_NULL_HANDLE;
    }
    profiler.query_count = 0;
    profiler.names.clear();
}

std::vector<GpuTimerResult> read_gpu_timers(VkDevice device, const GpuProfiler& profiler) {
    std::vector<GpuTimerResult> results;
    if (profiler.query_pool == VK_NULL_HANDLE || profiler.query_count == 0) {
        return results;
    }

    const uint32_t timer_count = profiler.query_count / 2;

    // Each query returns a uint64_t timestamp + uint64_t availability
    struct QueryEntry {
        uint64_t timestamp;
        uint64_t availability;
    };
    std::vector<QueryEntry> raw(profiler.query_count);

    VkResult vr = vkGetQueryPoolResults(
        device, profiler.query_pool, 0, profiler.query_count,
        profiler.query_count * sizeof(QueryEntry), raw.data(), sizeof(QueryEntry),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (vr != VK_SUCCESS && vr != VK_NOT_READY) {
        return results;
    }

    const double ns_per_tick = static_cast<double>(profiler.timestamp_period);

    for (uint32_t i = 0; i < timer_count; ++i) {
        const QueryEntry& start = raw[i * 2];
        const QueryEntry& end = raw[i * 2 + 1];

        GpuTimerResult r;
        r.name = (i < profiler.names.size()) ? profiler.names[i] : "unknown";

        if (start.availability != 0 && end.availability != 0) {
            const uint64_t ticks = end.timestamp - start.timestamp;
            r.ms = static_cast<double>(ticks) * ns_per_tick / 1e6;
        } else {
            r.ms = 0.0;
        }
        results.push_back(r);
    }

    return results;
}

}  // namespace meridian

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN
