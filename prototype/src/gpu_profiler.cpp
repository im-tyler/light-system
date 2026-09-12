#include "gpu_profiler.h"

#if MERIDIAN_VK_CONTEXT_HAS_VULKAN

namespace meridian {

// Tick delta between two timestamps of a counter with `valid_bits` valid
// bits. The counter wraps at 2^valid_bits, so the subtraction must be
// taken modulo 2^valid_bits or a wrapped interval reports a huge bogus
// duration. 64 (or the 0 "unset" placeholder) subtracts directly.
constexpr uint64_t timestamp_delta_ticks(uint64_t end, uint64_t start, uint32_t valid_bits) {
    if (valid_bits == 0 || valid_bits >= 64) {
        return end - start;
    }
    const uint64_t mask = (uint64_t{1} << valid_bits) - 1;
    return (end - start) & mask;
}

static_assert(timestamp_delta_ticks(150, 100, 64) == 50);
static_assert(timestamp_delta_ticks(150, 100, 0) == 50);
static_assert(timestamp_delta_ticks(2, 0xffffffffull, 32) == 3);
static_assert(timestamp_delta_ticks(0, (1ull << 36) - 5, 36) == 5);

VkResult create_gpu_profiler(VkPhysicalDevice physical_device, VkDevice device,
                              uint32_t queue_family, uint32_t max_timers, GpuProfiler& profiler) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);

    if (!properties.limits.timestampComputeAndGraphics) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    if (family_count > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
    }
    if (queue_family >= families.size()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // Zero valid bits means the queue family never supports timestamps.
    const uint32_t valid_bits = families[queue_family].timestampValidBits;
    if (valid_bits == 0) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    profiler.timestamp_period = properties.limits.timestampPeriod;
    profiler.timestamp_valid_bits = valid_bits;
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
    profiler.timestamp_valid_bits = 0;
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
            const uint64_t ticks = timestamp_delta_ticks(end.timestamp, start.timestamp,
                                                         profiler.timestamp_valid_bits);
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
