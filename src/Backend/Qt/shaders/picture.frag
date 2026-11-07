#version 440
// The picture's texel, opaque (the frame is RGBX)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
layout(binding = 1) uniform sampler2D picture;
void main() {
    colour = vec4(texture(picture, uv).rgb, 1.0);
}
