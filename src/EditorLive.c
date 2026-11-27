#include "Sst.h"
#include "EditorLive.h"

#include <string.h>

#include "EngineObject.h"
#include "Backend/Joypad.h"
#include "Backend/VDP.h"
#include "EnginePalette.h"
#include "GameInterface.h"
#include "LevelData.h"
#include "LevelLayout.h"
#include "ScrollEngine.h"

EditorSession editor_session;
bool editor_open;
bool editor_present;
bool editor_playtest;

static uint8_t live_objects[EDITOR_MAX_OBJECTS * 6 + 6];
static uint8_t live_rings[EDITOR_MAX_RINGS * 4 + 4];
static bool reload_pending;
static uint16_t colour[4][16];
static bool colour_set[4][16];
static bool target_set;
static int target_zone, target_act;
static char target_code[16];

static void WriteLive(void) {
    EditorLevel_WriteObjects(&editor_session.level, live_objects, sizeof(live_objects));
    EditorLevel_WriteRings(&editor_session.level, live_rings, sizeof(live_rings));
}

static int cursor_x, cursor_y;
static uint8_t pad_before, ext_before, pad_actions;
static int move_speed;

uint8_t Editor_TakePadActions(void) {
    const uint8_t actions = pad_actions;
    pad_actions = 0;
    return actions;
}

// The lowest the cursor and the camera go: the bottom of the level's layout (its rows of 128 pixels: a tall level is more than $800 pixels), $7FF for a level that is not that tall; an object's Y has 12 bits
int Editor_LevelBottom(void) {
    int rows = level_layout_fg.rows > level_layout_bg.rows ? level_layout_fg.rows : level_layout_bg.rows;
    if (rows < LEVEL_LAYOUT_ROWS) rows = LEVEL_LAYOUT_ROWS;
    const int bottom = rows * 128 - 1;
    return bottom > 0xFFF ? 0xFFF : bottom;
}

void Editor_SetCursor(int x, int y) {
    cursor_x = x < 0 ? 0 : x;
    cursor_y = y < 0 ? 0 : y;
}

void Editor_GetCursor(int *x, int *y) {
    *x = cursor_x;
    *y = cursor_y;
}

#define EDITOR_PARK 0x7000 // (past the end of any level)

static void HoldColours(void);

void Editor_SetPlaytest(bool on) {
    if (on == editor_playtest || !editor_present)
        return;
    if (on) { // (the player comes to the cursor)
        player->x_pos.f.u = (int16_t)cursor_x;
        player->y_pos.f.u = (int16_t)cursor_y;
        player->x_vel = player->y_vel = player->ground_vel = 0;
    } else { // (back to editing: the cursor is where the player was left, and the player is parked again at the next frame)
        cursor_x = player->x_pos.f.u;
        cursor_y = player->y_pos.f.u;
    }
    editor_playtest = on;
    Game_EditorSetMode(!on);
}

void Editor_Hold(void) {
    if (!editor_present)
        return;
    HoldColours();
    if (editor_playtest)
        return; // (the player is playing)
    // The pad (or the keys that stand for it) is object placement mode's: the D-pad moves the cursor, and the camera with it, faster the longer it is held (debug mode's own ramp); C places, A and Y choose the next and the
    // previous object, the right stick or [ ] the subtype, B deletes what is under the cursor. The window (Qt side) takes the actions with Editor_TakePadActions: it knows what is chosen
    const uint8_t pad = Joypad_GetState1(), ext = Joypad_GetExtState1();
    const uint8_t pressed = (uint8_t)(pad & ~pad_before);
    const uint8_t ext_pressed = (uint8_t)(ext & ~ext_before);
    pad_before = pad;
    ext_before = ext;
    if (pressed & JPAD_C) pad_actions |= EDITOR_PAD_PLACE;
    if (pressed & JPAD_B) pad_actions |= EDITOR_PAD_DELETE;
    if (ext_pressed & JPAD_EXT_Y) pad_actions |= EDITOR_PAD_PREVIOUS;
    else if (pressed & JPAD_A) pad_actions |= EDITOR_PAD_NEXT;
    if (ext_pressed & JPAD_EXT_SUBTYPE_DEC) pad_actions |= EDITOR_PAD_SUBTYPE_DEC;
    if (ext_pressed & JPAD_EXT_SUBTYPE_INC) pad_actions |= EDITOR_PAD_SUBTYPE_INC;
    const bool moving = (pad & (JPAD_UP | JPAD_DOWN | JPAD_LEFT | JPAD_RIGHT)) != 0;
    if (moving) {
        if (move_speed < 16) move_speed++; // (4 pixels a frame at first, 16 after a quarter of a second)
    } else {
        move_speed = 0;
    }
    const int step = moving ? 4 + (move_speed * 12) / 16 : 0;
    int nx = cursor_x + ((pad & JPAD_RIGHT) ? step : 0) - ((pad & JPAD_LEFT) ? step : 0);
    int ny = cursor_y + ((pad & JPAD_DOWN) ? step : 0) - ((pad & JPAD_UP) ? step : 0);
    nx = nx < 0 ? 0 : (nx > 0x3FFF ? 0x3FFF : nx);
    const int bottom = Editor_LevelBottom();
    ny = ny < 0 ? 0 : (ny > bottom ? bottom : ny);
    cursor_x = nx;
    cursor_y = ny;
    // While the level is edited the player is parked far outside it, so nothing that waits for him to come near (the signpost, the checkpoints, the path swappers, the badniks that go for him...) can be triggered;
    // the cursor is where the editor looks, and a playtest starts from it
    player->x_pos.f.u = EDITOR_PARK;
    player->y_pos.f.u = EDITOR_PARK;
    player->x_vel = player->y_vel = player->ground_vel = 0;
}

static int16_t saved_x, saved_y;
static bool standing_in;

bool Editor_Editing(void) {
    return editor_present && !editor_playtest;
}

void Editor_CameraBegin(void) {
    if (!Editor_Editing())
        return;
    saved_x = player->x_pos.f.u;
    saved_y = player->y_pos.f.u;
    player->x_pos.f.u = (int16_t)cursor_x;
    player->y_pos.f.u = (int16_t)cursor_y;
    standing_in = true;
}

void Editor_CameraEnd(void) {
    if (!standing_in)
        return;
    standing_in = false;
    player->x_pos.f.u = saved_x;
    player->y_pos.f.u = saved_y;
}

void Editor_Open(int zone, int act, const uint8_t *objects, const uint8_t *rings) {
    target_set = false;
    EditorLevel_Clear(&editor_session.level);
    EditorLevel_LoadObjects(&editor_session.level, objects);
    EditorLevel_LoadRings(&editor_session.level, rings);
    editor_session.level.dirty = false;
    editor_session.zone = zone;
    editor_session.act = act;
    editor_open = true;
    WriteLive();
    Game_EditorLamp(0); // (what the playtest leaves of the checkpoints is not the level's)
    Game_EditorSetMode(true); // (the level is edited, not played: a game mode of its own)
}

void Editor_SetTarget(int zone, int act, const char *code) {
    target_set = code != NULL;
    target_zone = zone;
    target_act = act;
    target_code[0] = 0;
    if (code != NULL)
        strncat(target_code, code, sizeof(target_code) - 1);
}
bool Editor_HasTarget(void) { return editor_open && target_set; }
int Editor_TargetZone(void) { return target_zone; }
int Editor_TargetAct(void) { return target_act; }
const char *Editor_TargetCode(void) { return target_code; }

static bool animation_off;
void Editor_SetAnimationOff(bool off) { animation_off = off; }
bool Editor_AnimationOff(void) { return editor_open && animation_off; }

void Editor_Close(void) {
    if (!editor_open)
        return;
    animation_off = false;
    target_set = false;
    if (Editor_Editing()) { // (the player is where the cursor is when the level is played again)
        player->x_pos.f.u = (int16_t)cursor_x;
        player->y_pos.f.u = (int16_t)cursor_y;
    }
    editor_open = false;
    editor_present = false;
    editor_playtest = false;
    Game_EditorSetMode(false); // (the level is played again, from the game's own data)
    memset(colour_set, 0, sizeof(colour_set)); // (what the editor set is the editor's: the level's own palette comes back)
    reload_pending = true;
}

void Editor_Left(void) {
    editor_playtest = false;
    Game_EditorLamp(1);
}

void Editor_Apply(void) {
    if (!editor_open)
        return;
    WriteLive();
    reload_pending = true;
}

const uint8_t *Editor_ObjectLayout(int zone, int act, const uint8_t *original) {
    return editor_open && zone == editor_session.zone && act == editor_session.act ? live_objects : original;
}

const uint8_t *Editor_RingLayout(int zone, int act, const uint8_t *original) {
    return editor_open && zone == editor_session.zone && act == editor_session.act ? live_rings : original;
}

bool Editor_TakeReload(void) {
    const bool pending = reload_pending;
    reload_pending = false;
    return pending;
}

void Editor_ClearLevelObjects(void) {
    Sst_Reset(); // (the dynamic objects and the level's go; the characters stay)
}

// ---- The terrain ----
static uint8_t *art_buffer;
static size_t art_bytes, block_bytes, chunk_bytes, solid_bytes[2];

void Editor_NoteArt(uint8_t *art, size_t bytes) { art_buffer = art; art_bytes = bytes; }
static size_t art_capacity;
void Editor_NoteArtCapacity(size_t bytes) { art_capacity = bytes; }
size_t Editor_ArtCapacity(void) { return art_capacity > art_bytes ? art_capacity : art_bytes; }
void Editor_NoteBlocks(size_t bytes) { block_bytes = bytes; }
void Editor_NoteChunks(size_t bytes) { chunk_bytes = bytes; }
void Editor_NoteSolids(int path, size_t bytes) { solid_bytes[path & 1] = bytes; }

uint8_t *Editor_Art(size_t *bytes) { *bytes = art_bytes; return art_buffer; }
uint8_t *Editor_Blocks(size_t *bytes) { *bytes = block_bytes; return level_map16; }
const uint8_t *Editor_BlocksView(size_t *bytes) { *bytes = sizeof(level_map16); return level_map16; } // (the whole table: the animated blocks that the level writes past the file's are there too)
uint8_t *Editor_Chunks(size_t *bytes) { *bytes = chunk_bytes; return level_map128; }
uint8_t *Editor_Solids(int path, size_t *bytes) { *bytes = solid_bytes[path & 1]; return coll_index[path & 1]; }
// The layout as the editor handles it: a fixed buffer of both planes (LEVEL_LAYOUT_MAX_ROWS rows of $100 bytes), made from the level's planes when the editor first looks at a layout, and put back into them (each
// plane at the size that holds it) when the picture is redrawn. The planes themselves are only as big as the level. editor_rows is how tall the level is (16 to 32): a layout of more rows than 16 wraps by its own masks.
static uint8_t editor_layout[LEVEL_LAYOUT_MAX_ROWS * 0x100];
static unsigned editor_layout_generation = ~0u;
static bool editor_layout_changed;
static int editor_cols[2] = { LEVEL_LAYOUT_COLS, LEVEL_LAYOUT_COLS };
static bool editor_bg_wraps, editor_bg_wraps_y;
static int editor_rows[2] = { LEVEL_LAYOUT_ROWS, LEVEL_LAYOUT_ROWS }; // each plane's height: the foreground's and the background's (a background is often shorter than 16 rows)
static void SyncLayout(void) {
    if (editor_layout_generation != level_layout_generation) { // (another layout has been loaded since)
        LevelLayout_ToInterleaved(editor_layout);
        editor_layout_generation = level_layout_generation;
        editor_layout_changed = false;
        editor_rows[0] = level_layout_fg.rows > 0 ? level_layout_fg.rows : LEVEL_LAYOUT_ROWS; // (the layout's own heights, whatever they are)
        editor_rows[1] = level_layout_bg.rows > 0 ? level_layout_bg.rows : LEVEL_LAYOUT_ROWS;
        for (int k = 0; k < 2; k++)
            if (editor_rows[k] > LEVEL_LAYOUT_MAX_ROWS) editor_rows[k] = LEVEL_LAYOUT_MAX_ROWS;
        editor_cols[0] = level_layout_fg.width > 0 ? level_layout_fg.width : LEVEL_LAYOUT_COLS;
        editor_cols[1] = level_layout_bg.width > 0 ? level_layout_bg.width : LEVEL_LAYOUT_COLS;
        editor_bg_wraps = level_layout_bg.wraps_x;
        editor_bg_wraps_y = level_layout_bg.wraps_y;
    }
}
uint8_t *Editor_Layout(size_t *bytes) {
    SyncLayout();
    *bytes = sizeof(editor_layout);
    editor_layout_changed = true; // (the caller may write it)
    return editor_layout;
}
int Editor_LayoutCols(int plane) {
    SyncLayout();
    return editor_cols[plane & 1];
}
void Editor_SetLayoutCols(int plane, int cols) {
    SyncLayout();
    if (cols < 1) cols = 1;
    if (cols > LEVEL_LAYOUT_COLS) cols = LEVEL_LAYOUT_COLS;
    editor_cols[plane & 1] = cols;
}
bool Editor_BgWraps(void) {
    SyncLayout();
    return editor_bg_wraps;
}
void Editor_SetBgWraps(bool wraps) {
    SyncLayout();
    editor_bg_wraps = wraps;
    editor_layout_changed = true; // (the planes take it when the picture is redrawn)
}
bool Editor_BgWrapsY(void) {
    SyncLayout();
    return editor_bg_wraps_y;
}
void Editor_SetBgWrapsY(bool wraps) {
    SyncLayout();
    editor_bg_wraps_y = wraps;
    editor_layout_changed = true;
}
int Editor_LayoutRows(int plane) {
    SyncLayout();
    return editor_rows[plane & 1];
}
void Editor_SetLayoutRows(int plane, int rows) {
    SyncLayout();
    if (rows < 1) rows = 1;
    if (rows > LEVEL_LAYOUT_MAX_ROWS) rows = LEVEL_LAYOUT_MAX_ROWS;
    editor_rows[plane & 1] = rows;
    // the level wraps by its height, 32 rows at the least (LevelLayout_Load)
    LevelLayoutSize wrap = { LEVEL_LAYOUT_COLS, LEVEL_LAYOUT_COLS, LEVEL_LAYOUT_MAX_ROWS, LEVEL_LAYOUT_MAX_ROWS };
    ScrollEngine_SetLevelMasksForLayout(&wrap, level_layout_loops);
}
const uint8_t *Editor_HeightMap(void) { return Collision_HeightMap; }
const uint8_t *Editor_CurveAngles(void) { return Collision_Angle; }

void Editor_CommitArt(int tile) {
    if (art_buffer == NULL || tile < 0 || (size_t)(tile + 1) * 32 > art_bytes)
        return;
    VDP_SeekVRAM((size_t)tile * 32);
    VDP_WriteVRAM(art_buffer + (size_t)tile * 32, 32);
}

void Editor_RedrawLevel(void) {
    if (editor_layout_changed && editor_layout_generation == level_layout_generation) {
        const LevelLayoutSize wanted = { (uint16_t)editor_cols[0], (uint16_t)editor_cols[1], (uint16_t)editor_rows[0], (uint16_t)editor_rows[1], editor_bg_wraps, editor_bg_wraps_y };
        LevelLayout_FromInterleavedSized(editor_layout, &wanted); // (the planes take what the editor changed)
        editor_layout_changed = false;
    }
    Game_EditorRedraw();
}

uint16_t Editor_GetColour(int line, int index) {
    line &= 3;
    index &= 15;
    return colour_set[line][index] ? colour[line][index] : dry_palette[line][index];
}

uint16_t Editor_GetBaseColour(int line, int index) {
    line &= 3;
    index &= 15;
    return colour_set[line][index] ? colour[line][index] : dry_palette_dup[line][index];
}

void Editor_SetColour(int line, int index, uint16_t value) {
    line &= 3;
    index &= 15;
    colour[line][index] = value & 0x0EEE;
    colour_set[line][index] = true;
    dry_palette[line][index] = colour[line][index];
}

// (the colours the editor has set, put back every frame over what a palette cycle or a fade writes)
static void HoldColours(void) {
    for (int line = 0; line < 4; line++)
        for (int i = 0; i < 16; i++)
            if (colour_set[line][i])
                dry_palette[line][i] = colour[line][i];
}
