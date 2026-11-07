#version 440
// The picture's quad: a corner (0,0 bottom left to 1,1 top right) to where it is drawn (the viewport is the part of the widget the picture fits) and the texel it shows (row 0 is the picture's top)
layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 uv;
layout(std140, binding = 0) uniform Frame {
    mat4 correction; // QRhi's clip space correction (Vulkan's Y points down)
};
void main() {
    uv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = correction * vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
