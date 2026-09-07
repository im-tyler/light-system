#version 450

layout(push_constant) uniform ViewParams {
    vec4 view;  // xy = world->NDC scale, zw = NDC translate
} params;

layout(location = 0) in vec2 in_corner;
layout(location = 1) in vec2 in_position;
layout(location = 2) in vec2 in_shape;  // x = half extent, y = 1 disc / 0 solid
layout(location = 3) in vec4 in_color;

layout(location = 0) out vec2 frag_corner;
layout(location = 1) flat out vec4 frag_color;
layout(location = 2) flat out float frag_shape;

void main() {
    vec2 world = in_position + in_corner * in_shape.x;
    frag_corner = in_corner;
    frag_color = in_color;
    frag_shape = in_shape.y;
    gl_Position = vec4(world * params.view.xy + params.view.zw, 0.0, 1.0);
}
