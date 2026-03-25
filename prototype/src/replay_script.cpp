#include "replay_script.h"

#include "vgeo_builder.h"

#include <fstream>
#include <sstream>

namespace meridian {

namespace {

std::string trim(std::string_view input) {
    const size_t begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const size_t end = input.find_last_not_of(" \t\r\n");
    return std::string(input.substr(begin, end - begin + 1));
}

std::vector<std::string> split_list(std::string_view input) {
    std::vector<std::string> values;
    std::string current;
    std::istringstream stream{std::string(input)};
    while (std::getline(stream, current, ',')) {
        const std::string trimmed = trim(current);
        if (!trimmed.empty()) {
            values.push_back(trimmed);
        }
    }
    return values;
}

uint32_t parse_u32(std::string_view value) {
    const std::string trimmed = trim(value);
    try {
        return static_cast<uint32_t>(std::stoul(trimmed));
    } catch (const std::exception&) {
        throw BuilderError("invalid replay u32 value: " + std::string(value));
    }
}

float parse_float(std::string_view value) {
    const std::string trimmed = trim(value);
    try {
        return std::stof(trimmed);
    } catch (const std::exception&) {
        throw BuilderError("invalid replay float value: " + std::string(value));
    }
}

}  // namespace

ReplayScript load_replay_script(const std::filesystem::path& script_path) {
    std::ifstream input(script_path);
    if (!input) {
        throw BuilderError("failed to open replay script: " + script_path.string());
    }

    ReplayScript script;
    script.name = script_path.stem().string();

    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            throw BuilderError("replay script line missing '=' at line " +
                               std::to_string(line_number));
        }

        const std::string key = trim(trimmed.substr(0, equals));
        const std::string value = trim(trimmed.substr(equals + 1));

        if (key == "name") {
            script.name = value;
        } else if (key == "frame_count") {
            script.frame_count = parse_u32(value);
        } else if (key == "resident_budget") {
            script.resident_budget = parse_u32(value);
        } else if (key == "eviction_grace_frames") {
            script.eviction_grace_frames = parse_u32(value);
        } else if (key == "bootstrap_resident") {
            script.bootstrap_resident = value;
        } else if (key == "error_thresholds") {
            const std::vector<std::string> values = split_list(value);
            script.error_thresholds.clear();
            script.error_thresholds.reserve(values.size());
            for (const std::string& item : values) {
                script.error_thresholds.push_back(parse_float(item));
            }
        } else {
            throw BuilderError("unknown replay script key: " + key);
        }
    }

    if (script.frame_count == 0) {
        script.frame_count = static_cast<uint32_t>(script.error_thresholds.size());
    }
    if (script.frame_count == 0) {
        throw BuilderError("replay script must define frame_count or error_thresholds");
    }
    if (script.error_thresholds.size() != script.frame_count) {
        throw BuilderError("replay script error_threshold count must match frame_count");
    }

    if (script.bootstrap_resident != "none" && script.bootstrap_resident != "all" &&
        script.bootstrap_resident != "base-only" && script.bootstrap_resident != "lod-only") {
        throw BuilderError("invalid replay bootstrap_resident mode: " + script.bootstrap_resident);
    }

    return script;
}

}  // namespace meridian
