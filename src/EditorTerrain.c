#include "EditorTerrain.h"

#include <string.h>

#include "EditorLive.h"
#include "LevelData.h"
#include "EditorModel.h"

#define TILE_SIZE_BYTES 32
#define BLOCK_SIZE_BYTES 8
#define CHUNK_SIZE_BYTES 128
#define MAX_BLOCKS 0x400
#define MAX_CHUNKS 0x100
#define MAX_TILES 0x800 // (a block's tile word has 11 bits for it)

typedef enum { OP_NONE, OP_INSERT, OP_DELETE, OP_MOVE } OpKind;
typedef struct {
    OpKind op;
    int a, b;
} Op;

static Op last_op[3];

static int MapIndex(const Op *o, int v) {
    switch (o->op) {
    case OP_INSERT:
        return v >= o->a ? v + 1 : v;
    case OP_DELETE:
        return v == o->a ? 0 : (v > o->a ? v - 1 : v);
    case OP_MOVE:
        if (v == o->a)
            return o->b;
        if (o->a < o->b)
            return (v > o->a && v <= o->b) ? v - 1 : v;
        return (v >= o->b && v < o->a) ? v + 1 : v;
    default:
        return v;
    }
}

int EditorTerrain_Mapped(EditorTerrainKind kind, int old) {
    return MapIndex(&last_op[kind], old);
}

// The entries of a table (`count` of `elem` bytes) as the operation leaves them
static void ShiftEntries(uint8_t *base, size_t elem, size_t count, const Op *o) {
    uint8_t *at = base + (size_t)o->a * elem;
    switch (o->op) {
    case OP_INSERT:
        memmove(at + elem, at, (count - (size_t)o->a) * elem);
        memset(at, 0, elem);
        break;
    case OP_DELETE:
        memmove(at, at + elem, (count - (size_t)o->a - 1) * elem);
        memset(base + (count - 1) * elem, 0, elem);
        break;
    case OP_MOVE: {
        uint8_t tmp[CHUNK_SIZE_BYTES];
        uint8_t *to = base + (size_t)o->b * elem;
        memcpy(tmp, at, elem);
        if (o->a < o->b)
            memmove(at, at + elem, (size_t)(o->b - o->a) * elem);
        else
            memmove(to + elem, to, (size_t)(o->a - o->b) * elem);
        memcpy(to, tmp, elem);
        break;
    }
    default:
        break;
    }
}

size_t EditorTerrain_Count(EditorTerrainKind kind) {
    size_t bytes = 0;
    switch (kind) {
    case EDITOR_TERRAIN_TILE:
        Editor_Art(&bytes);
        return bytes / TILE_SIZE_BYTES;
    case EDITOR_TERRAIN_BLOCK:
        Editor_Blocks(&bytes);
        return bytes / BLOCK_SIZE_BYTES;
    default:
        Editor_Chunks(&bytes);
        return bytes / CHUNK_SIZE_BYTES;
    }
}

size_t EditorTerrain_Capacity(EditorTerrainKind kind) {
    switch (kind) {
    case EDITOR_TERRAIN_TILE: {
        const size_t fit = Editor_ArtCapacity() / TILE_SIZE_BYTES;
        return fit < MAX_TILES ? fit : MAX_TILES;
    }
    case EDITOR_TERRAIN_BLOCK:
        return MAX_BLOCKS;
    default:
        return MAX_CHUNKS;
    }
}

// ---- the references ----

static void RemapBlockTiles(const Op *o) { // the tile of each of a block's four words
    size_t bytes;
    uint8_t *blocks = Editor_Blocks(&bytes);
    for (size_t i = 0; blocks != NULL && i + 1 < bytes; i += 2) {
        const uint16_t w = (uint16_t)((blocks[i] << 8) | blocks[i + 1]);
        const uint16_t n = (uint16_t)((w & 0xF800) | (MapIndex(o, w & 0x7FF) & 0x7FF));
        blocks[i] = (uint8_t)(n >> 8);
        blocks[i + 1] = (uint8_t)n;
    }
}

static void RemapChunkBlocks(const Op *o) { // the block of each of a chunk's words (its flips and solidity are kept)
    size_t bytes;
    uint8_t *chunks = Editor_Chunks(&bytes);
    for (size_t i = 0; chunks != NULL && i + 1 < bytes; i += 2) {
        const uint16_t w = (uint16_t)((chunks[i] << 8) | chunks[i + 1]);
        const uint16_t n = (uint16_t)((w & 0xFC00) | (MapIndex(o, w & 0x3FF) & 0x3FF));
        chunks[i] = (uint8_t)(n >> 8);
        chunks[i + 1] = (uint8_t)n;
    }
}

static void RemapLayout(const Op *o) { // the chunk of each cell of the layout
    size_t bytes;
    uint8_t *layout = Editor_Layout(&bytes);
    for (size_t i = 0; layout != NULL && i < bytes; i++)
        layout[i] = (uint8_t)MapIndex(o, layout[i]);
}

static void CommitTilesFrom(int first) {
    const int count = (int)EditorTerrain_Count(EDITOR_TERRAIN_TILE);
    for (int t = first < 0 ? 0 : first; t < count; t++)
        Editor_CommitArt(t);
}

// Applies an operation to the data of its kind and everything that names it
static void Apply(EditorTerrainKind kind, Op o) {
    size_t bytes;
    const size_t count = EditorTerrain_Count(kind);
    last_op[kind] = o;
    int first_changed = o.a;
    if (o.op == OP_MOVE && o.b < first_changed)
        first_changed = o.b;
    if (kind == EDITOR_TERRAIN_TILE) {
        uint8_t *art = Editor_Art(&bytes);
        ShiftEntries(art, TILE_SIZE_BYTES, count, &o);
        const size_t new_bytes = o.op == OP_INSERT ? bytes + TILE_SIZE_BYTES : o.op == OP_DELETE ? bytes - TILE_SIZE_BYTES : bytes;
        Editor_NoteArt(art, new_bytes);
        RemapBlockTiles(&o);
        CommitTilesFrom(first_changed);
    } else if (kind == EDITOR_TERRAIN_BLOCK) {
        uint8_t *blocks = Editor_Blocks(&bytes);
        ShiftEntries(blocks, BLOCK_SIZE_BYTES, count, &o);
        for (int path = 0; path < 2; path++) { // (a curve for each block: the table goes with the blocks)
            size_t sb;
            uint8_t *solids = Editor_Solids(path, &sb);
            const size_t entries = sb > count ? sb : count;
            ShiftEntries(solids, 1, entries < MAX_BLOCKS ? entries : MAX_BLOCKS, &o);
            size_t new_sb = sb;
            if (o.op == OP_INSERT && sb < MAX_BLOCKS)
                new_sb = sb + 1;
            else if (o.op == OP_DELETE && sb > 0)
                new_sb = sb - 1;
            Editor_NoteSolids(path, new_sb);
        }
        Editor_NoteBlocks(o.op == OP_INSERT ? bytes + BLOCK_SIZE_BYTES : o.op == OP_DELETE ? bytes - BLOCK_SIZE_BYTES : bytes);
        RemapChunkBlocks(&o);
    } else {
        uint8_t *chunks = Editor_Chunks(&bytes);
        ShiftEntries(chunks, CHUNK_SIZE_BYTES, count, &o);
        Editor_NoteChunks(o.op == OP_INSERT ? bytes + CHUNK_SIZE_BYTES : o.op == OP_DELETE ? bytes - CHUNK_SIZE_BYTES : bytes);
        RemapLayout(&o);
    }
}

// ---- the operations ----

static const char *Check(EditorTerrainKind kind, int index, bool zero_ok) {
    if (index < 0 || (size_t)index >= EditorTerrain_Count(kind))
        return "there is no such one";
    if (index == 0 && !zero_ok)
        return "number 0 is the blank one: it stays";
    return NULL;
}

const char *EditorTerrain_Insert(EditorTerrainKind kind, int at) {
    const size_t count = EditorTerrain_Count(kind);
    if (count == 0)
        return "this level has none to insert among";
    if (count >= EditorTerrain_Capacity(kind))
        return "the limit is reached: there is no room for another";
    if (at < 1 || (size_t)at > count)
        return "a new one goes after number 0, up to the end";
    Apply(kind, (Op){ OP_INSERT, at, 0 });
    return NULL;
}

const char *EditorTerrain_Delete(EditorTerrainKind kind, int index) {
    const char *why = Check(kind, index, false);
    if (why != NULL)
        return why;
    if (EditorTerrain_Count(kind) < 2)
        return "at least two are needed to delete one";
    Apply(kind, (Op){ OP_DELETE, index, 0 });
    return NULL;
}

const char *EditorTerrain_Move(EditorTerrainKind kind, int from, int to) {
    const char *why = Check(kind, from, false);
    if (why == NULL)
        why = Check(kind, to, false);
    if (why != NULL)
        return why;
    if (from == to)
        return "it is there already";
    Apply(kind, (Op){ OP_MOVE, from, to });
    return NULL;
}

// ---- copy and paste ----

static struct {
    bool has[3];
    uint8_t data[3][CHUNK_SIZE_BYTES];
    uint8_t curve[2]; // a block's curves on the two paths
} clip;

bool EditorTerrain_HasCopy(EditorTerrainKind kind) {
    return clip.has[kind];
}

const char *EditorTerrain_Copy(EditorTerrainKind kind, int index) {
    const char *why = Check(kind, index, true);
    if (why != NULL)
        return why;
    size_t bytes;
    if (kind == EDITOR_TERRAIN_TILE) {
        memcpy(clip.data[kind], Editor_Art(&bytes) + (size_t)index * TILE_SIZE_BYTES, TILE_SIZE_BYTES);
    } else if (kind == EDITOR_TERRAIN_BLOCK) {
        memcpy(clip.data[kind], Editor_Blocks(&bytes) + (size_t)index * BLOCK_SIZE_BYTES, BLOCK_SIZE_BYTES);
        for (int path = 0; path < 2; path++) {
            size_t sb;
            const uint8_t *solids = Editor_Solids(path, &sb);
            clip.curve[path] = (size_t)index < sb ? solids[index] : 0;
        }
    } else {
        memcpy(clip.data[kind], Editor_Chunks(&bytes) + (size_t)index * CHUNK_SIZE_BYTES, CHUNK_SIZE_BYTES);
    }
    clip.has[kind] = true;
    return NULL;
}

const char *EditorTerrain_Paste(EditorTerrainKind kind, int index) {
    if (!clip.has[kind])
        return "nothing is copied";
    const char *why = Check(kind, index, false);
    if (why != NULL)
        return why;
    size_t bytes;
    if (kind == EDITOR_TERRAIN_TILE) {
        memcpy(Editor_Art(&bytes) + (size_t)index * TILE_SIZE_BYTES, clip.data[kind], TILE_SIZE_BYTES);
        Editor_CommitArt(index);
    } else if (kind == EDITOR_TERRAIN_BLOCK) {
        memcpy(Editor_Blocks(&bytes) + (size_t)index * BLOCK_SIZE_BYTES, clip.data[kind], BLOCK_SIZE_BYTES);
        for (int path = 0; path < 2; path++) {
            size_t sb;
            uint8_t *solids = Editor_Solids(path, &sb);
            if ((size_t)index < sb)
                solids[index] = clip.curve[path];
        }
    } else {
        memcpy(Editor_Chunks(&bytes) + (size_t)index * CHUNK_SIZE_BYTES, clip.data[kind], CHUNK_SIZE_BYTES);
    }
    return NULL;
}

// ---- a blank level ----
bool EditorTerrain_MakeBlank(void) {
    size_t art_bytes, block_bytes, chunk_bytes, layout_bytes;
    uint8_t *art = Editor_Art(&art_bytes);
    uint8_t *blocks = Editor_Blocks(&block_bytes);
    uint8_t *chunks = Editor_Chunks(&chunk_bytes);
    uint8_t *layout = Editor_Layout(&layout_bytes);
    if (art == NULL || blocks == NULL || chunks == NULL || layout == NULL || Editor_ArtCapacity() < 3 * TILE_SIZE_BYTES)
        return false;
    // tiles: 0 blank, 1 earth (colour 1), 2 grass (colour 2)
    memset(art, 0, 3 * TILE_SIZE_BYTES);
    memset(art + 1 * TILE_SIZE_BYTES, 0x11, TILE_SIZE_BYTES);
    memset(art + 2 * TILE_SIZE_BYTES, 0x22, TILE_SIZE_BYTES);
    Editor_NoteArt(art, 3 * TILE_SIZE_BYTES);
    for (int t = 0; t < 3; t++)
        Editor_CommitArt(t);
    // blocks: 0 blank, 1 earth, 2 grass over earth (palette line 1: 1 << 13)
    memset(blocks, 0, 3 * BLOCK_SIZE_BYTES);
    static const uint16_t block_words[2][4] = { { 0x2001, 0x2001, 0x2001, 0x2001 }, { 0x2002, 0x2002, 0x2001, 0x2001 } };
    for (int b = 0; b < 2; b++)
        for (int k = 0; k < 4; k++) {
            blocks[(b + 1) * BLOCK_SIZE_BYTES + k * 2] = (uint8_t)(block_words[b][k] >> 8);
            blocks[(b + 1) * BLOCK_SIZE_BYTES + k * 2 + 1] = (uint8_t)block_words[b][k];
        }
    Editor_NoteBlocks(3 * BLOCK_SIZE_BYTES);
    // chunks: 0 blank, 1 earth with a grass top row, solid on both paths
    memset(chunks, 0, 2 * CHUNK_SIZE_BYTES);
    for (int k = 0; k < 64; k++) {
        chunks[CHUNK_SIZE_BYTES + k * 2] = 0xF0; // top and left/right/bottom solid on both paths
        chunks[CHUNK_SIZE_BYTES + k * 2 + 1] = k < 8 ? 2 : 1;
    }
    Editor_NoteChunks(2 * CHUNK_SIZE_BYTES);
    // the collision curve that is full (16 high in every column), the one the earth and the grass have on both paths
    const uint8_t *heights = Editor_HeightMap();
    int full = 0;
    for (int c = 1; heights != NULL && c < 256 && full == 0; c++) {
        int all = 1;
        for (int x = 0; x < 16; x++)
            all = all && heights[c * 16 + x] == 16;
        if (all)
            full = c;
    }
    for (int path = 0; path < 2; path++) {
        size_t sb;
        uint8_t *solids = Editor_Solids(path, &sb);
        memset(solids, 0, 0x400);
        solids[1] = solids[2] = (uint8_t)full;
        Editor_NoteSolids(path, 3);
    }
    // the layout: nothing, but a floor of chunk 1 across plane A's fourth row (chunk 1 is 128 pixels: y $180)
    memset(layout, 0, layout_bytes);
    for (int col = 0; col < 32; col++)
        layout[3 * 0x100 + col] = 1;
    // the colours the earth and the grass are in, and the editor's own list of what it has set
    Editor_SetColour(1, 1, 0x048);
    Editor_SetColour(1, 2, 0x0E0);
    EditorLevel_Clear(&editor_session.level);
    return true;
}
