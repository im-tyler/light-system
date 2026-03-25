#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace meridian {

struct ReplayScript {
    std::string name;
    uint32_t frame_count = 0;
    uint32_t resident_budget = 0xffffffffu;
    uint32_t eviction_grace_frames = 2;
    std::string bootstrap_resident = "none";
    std::vector<float> error_thresholds;
};

ReplayScript load_replay_script(const std::filesystem::path& script_path);

}  // namespace meridian
