#version 450

layout(location = 0) in vec2 frag_corner;
layout(location = 1) flat in vec4 frag_color;
layout(location = 2) flat in float frag_shape;

layout(location = 0) out vec4 out_color;

void main() {
    float alpha = 1.0;
    if (frag_shape > 1.5) {
        // Contact flash ring: soft annulus peaking near r ~ 0.8.
        float r = length(frag_corner);
        alpha = smoothstep(0.58, 0.74, r) * (1.0 - smoothstep(0.82, 1.0, r));
    } else if (frag_shape > 0.5) {
        float r = length(frag_corner);
        alpha = 1.0 - smoothstep(0.55, 1.0, r);
    }
    out_color = vec4(frag_color.rgb, frag_color.a * alpha);
}
