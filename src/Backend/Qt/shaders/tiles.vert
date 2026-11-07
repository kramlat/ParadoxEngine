#version 440
// The terrain editor's tiles, blocks and chunks (TileGpu.cpp): an item a quad, where the pane lists it, in the part of the pane the view covers
layout(location = 0) in vec2 corner;  // (0,0) the item's top left, (1,1) its bottom right
layout(location = 1) in vec4 rect;    // the item in the pane's pixels: x, y, width, height
layout(location = 2) in uint index;
layout(location = 3) in uint flags;
layout(location = 0) out vec2 local;
layout(location = 1) flat out uint v_index;
layout(location = 2) flat out uint v_flags;
layout(std140, binding = 0) uniform View {
    mat4 correction; // QRhi's clip space correction
    vec4 area;       // the part of the pane the view covers: x, y, width, height
    uvec4 art;       // where the data are in the buffer (bytes) and how many: art, its size, the video memory, the palette (words: 64 colours)
    uvec4 blocks;    // blocks, their size, chunks, their size
    uvec4 solids;    // path A's solids, size, path B's, size
    uvec4 heights;   // the heights of the collision curves, their size
};
void main() {
    local = corner;
    vec2 p = rect.xy + corner * rect.zw;
    vec2 ndc = vec2((p.x - area.x) / area.z * 2.0 - 1.0, 1.0 - (p.y - area.y) / area.w * 2.0);
    gl_Position = correction * vec4(ndc, 0.0, 1.0);
    v_index = index;
    v_flags = flags;
}
