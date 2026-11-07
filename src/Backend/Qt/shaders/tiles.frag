#version 440
// An item's pixel (TileGpu.cpp's kinds): a fill, a tile, a block or a chunk (looked up as TerrainEditor.cpp's TileImage, DrawBlock and DrawChunk do: a tile's colour 0 is the dark check, the
// video memory for the live tiles), with the darkening of the tiles that are not high priority, and the solids of a path over a block (DrawSolidOverlay). Colours go out premultiplied.
layout(location = 0) in vec2 local;
layout(location = 1) flat in uint v_index;
layout(location = 2) flat in uint v_flags;
layout(location = 0) out vec4 colour;
layout(std140, binding = 0) uniform View {
    mat4 correction;
    vec4 area;
    uvec4 art;
    uvec4 blocks;
    uvec4 solids;
    uvec4 heights;
};
layout(std430, binding = 1) readonly buffer Data { uint d[]; };

const uint FILL = 0u, TILE = 1u, BLOCK = 2u, CHUNK = 3u, SOLID = 4u;
const uint X_FLIP = 8u, Y_FLIP = 16u, DIM = 32u, LIVE = 64u;

uint Byte(uint at) { return (d[at >> 2] >> ((at & 3u) * 8u)) & 0xFFu; }
uint Be16(uint at) { return (Byte(at) << 8) | Byte(at + 1u); }

vec3 Rgb(uint c) { return vec3(float((c >> 16) & 0xFFu), float((c >> 8) & 0xFFu), float(c & 0xFFu)) / 255.0; }

// Pixel (x, y) of tile `tile` (unflipped) in palette line `line`
vec3 TilePixel(uint tile, uint line, uint x, uint y, bool live) {
    uint c = 0u;
    uint at = tile * 32u + y * 4u + (x >> 1);
    if (live && tile < 0x800u) {
        uint b = Byte(art.z + at);
        c = (x & 1u) != 0u ? (b & 15u) : (b >> 4);
    } else if ((tile + 1u) * 32u <= art.y) {
        uint b = Byte(art.x + at);
        c = (x & 1u) != 0u ? (b & 15u) : (b >> 4);
    }
    return c == 0u ? vec3(40.0, 40.0, 48.0) / 255.0 : Rgb(d[art.w + line * 16u + c]);
}

// Pixel (x, y) of a tile word's picture (8x8): its flips, palette line and priority
vec3 WordPixel(uint word, uint x, uint y, bool live, bool dim) {
    if ((word & 0x0800u) != 0u) x = 7u - x;
    if ((word & 0x1000u) != 0u) y = 7u - y;
    vec3 c = TilePixel(word & 0x7FFu, (word >> 13) & 3u, x, y, live);
    if (dim && (word & 0x8000u) == 0u)
        c = mix(c, vec3(0.0, 0.0, 40.0 / 255.0), 165.0 / 255.0);
    return c;
}

// Pixel (x, y) of a block (16x16, unflipped): false when the block is not there (nothing drawn)
bool BlockPixel(uint block, uint x, uint y, bool live, bool dim, out vec3 c) {
    if ((block + 1u) * 8u > blocks.y)
        return false;
    uint word = Be16(blocks.x + block * 8u + ((y >> 3) * 2u + (x >> 3)) * 2u);
    c = WordPixel(word, x & 7u, y & 7u, live, dim);
    return true;
}

// The solids over a block, at (u, v) in it (0..1): the colour and its cover (premultiplied), from the chunk's word for the block and the path (1 or 2)
vec4 Solid(uint word, uint path, vec2 uv) {
    uint bits = path == 2u ? (word >> 14) & 3u : (word >> 12) & 3u;
    if (bits == 0u)
        return vec4(0.0);
    uint table = path == 2u ? solids.z : solids.x, size = path == 2u ? solids.w : solids.y;
    uint block = word & 0x3FFu;
    if (block >= size)
        return vec4(0.0);
    uint curve = Byte(table + block);
    if (curve == 0u)
        return vec4(0.0);
    bool x_flip = (word & 0x0400u) != 0u, y_flip = (word & 0x0800u) != 0u;
    uint col = min(uint(uv.x * 16.0), 15u);
    int h = int(Byte(heights.x + curve * 16u + (x_flip ? 15u - col : col)));
    h = h <= 16 ? h : (h >= 0xF0 ? -(256 - h) : 16);
    if (h == 0)
        return vec4(0.0);
    bool from_bottom = (h > 0) != y_flip;
    float rows = float(abs(h)) / 16.0;
    if (from_bottom ? uv.y < 1.0 - rows : uv.y >= rows)
        return vec4(0.0);
    if (bits == 1u)
        return vec4(1.0, 1.0, 1.0, 1.0) * (130.0 / 255.0);
    if (bits == 3u)
        return vec4(0.0, 0.0, 0.0, 150.0 / 255.0);
    vec4 yellow = vec4(1.0, 230.0 / 255.0, 0.0, 1.0) * (130.0 / 255.0);
    bool bottom = y_flip ? uv.y < 0.25 : uv.y >= 0.75;
    if (!bottom)
        return yellow;
    vec4 red = vec4(1.0, 40.0 / 255.0, 40.0 / 255.0, 1.0) * (170.0 / 255.0);
    return red + yellow * (1.0 - red.a);
}

void main() {
    uint kind = v_flags & 7u;
    bool live = (v_flags & LIVE) != 0u, dim = (v_flags & DIM) != 0u;
    if (kind == FILL) {
        float a = float(v_index >> 24) / 255.0;
        colour = vec4(Rgb(v_index) * a, a);
        return;
    }
    if (kind == SOLID) {
        colour = Solid(v_flags >> 16, (v_flags >> 10) & 3u, local);
        if (colour.a == 0.0)
            discard;
        return;
    }
    if (kind == TILE) {
        uint x = min(uint(local.x * 8.0), 7u), y = min(uint(local.y * 8.0), 7u);
        if ((v_flags & X_FLIP) != 0u) x = 7u - x;
        if ((v_flags & Y_FLIP) != 0u) y = 7u - y;
        vec3 c = TilePixel(v_index, (v_flags >> 8) & 3u, x, y, live);
        colour = vec4(dim ? mix(c, vec3(0.0, 0.0, 40.0 / 255.0), 165.0 / 255.0) : c, 1.0);
        return;
    }
    if (kind == BLOCK) {
        uint x = min(uint(local.x * 16.0), 15u), y = min(uint(local.y * 16.0), 15u);
        if ((v_flags & X_FLIP) != 0u) x = 15u - x;
        if ((v_flags & Y_FLIP) != 0u) y = 15u - y;
        vec3 c;
        if (!BlockPixel(v_index, x, y, live, dim, c))
            discard;
        colour = vec4(c, 1.0);
        return;
    }
    // a chunk: 8x8 blocks, each with its flips (and solidity, when a path's solids are shown)
    uint px = min(uint(local.x * 128.0), 127u), py = min(uint(local.y * 128.0), 127u);
    if ((v_index + 1u) * 128u > blocks.w)
        discard;
    uint word = Be16(blocks.z + v_index * 128u + ((py >> 4) * 8u + (px >> 4)) * 2u);
    uint x = px & 15u, y = py & 15u;
    if ((word & 0x0400u) != 0u) x = 15u - x;
    if ((word & 0x0800u) != 0u) y = 15u - y;
    vec3 c;
    if (!BlockPixel(word & 0x3FFu, x, y, live, dim, c))
        discard;
    uint path = (v_flags >> 10) & 3u;
    if (path != 0u) {
        vec4 s = Solid(word, path, vec2(float(px & 15u) + 0.5, float(py & 15u) + 0.5) / 16.0);
        c = s.rgb + c * (1.0 - s.a);
    }
    colour = vec4(c, 1.0);
}
