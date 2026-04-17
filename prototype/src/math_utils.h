#pragma once

#include <cmath>
#include <algorithm>
#include <array>
#include <cstdint>

#include "vgeo_builder.h"

namespace meridian {

struct Mat4f {
    float m[16] = {0.0f};
};

struct CameraFrameData {
    Mat4f view_projection;
    Vec3f camera_position = {0.0f, 0.0f, 0.0f};
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

inline Vec3f camera_forward(const InteractiveCamera& cam) {
    return {std::sin(cam.yaw) * std::cos(cam.pitch),
            std::sin(cam.pitch),
            -std::cos(cam.yaw) * std::cos(cam.pitch)};
}

inline Vec3f camera_right(const InteractiveCamera& cam) {
    return {std::cos(cam.yaw), 0.0f, std::sin(cam.yaw)};
}

struct FrameUBO {
    float view_projection[16];
    float light_vp[16];
    float light_dir[4];
};

inline Mat4f identity_matrix() {
    Mat4f matrix;
    matrix.m[0] = 1.0f;
    matrix.m[5] = 1.0f;
    matrix.m[10] = 1.0f;
    matrix.m[15] = 1.0f;
    return matrix;
}

inline Mat4f multiply_matrix(const Mat4f& lhs, const Mat4f& rhs) {
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

inline Vec3f subtract_vec3(const Vec3f& lhs, const Vec3f& rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

inline Vec3f cross_vec3(const Vec3f& lhs, const Vec3f& rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

inline float dot_vec3(const Vec3f& lhs, const Vec3f& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

inline Vec3f normalize_vec3(const Vec3f& value) {
    const float length = std::sqrt(dot_vec3(value, value));
    if (length <= 1e-6f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return {value.x / length, value.y / length, value.z / length};
}

inline Mat4f look_at_matrix(const Vec3f& eye, const Vec3f& target, const Vec3f& up) {
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

inline Mat4f perspective_matrix(float vertical_fov_radians, float aspect_ratio, float near_plane,
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

inline Mat4f ortho_matrix(float left, float right, float bottom, float top, float near_val, float far_val) {
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

struct FrustumPlanes {
    float planes[6][4];
};

inline FrustumPlanes extract_frustum_planes(const Mat4f& vp) {
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

} // namespace meridian
