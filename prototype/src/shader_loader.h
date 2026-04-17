#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <stdexcept>

namespace meridian {

inline std::string load_shader_source(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open shader: " + path.string());
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

}  // namespace meridian
