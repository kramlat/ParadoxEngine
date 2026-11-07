#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The terrain editor's list operations on the running level's tiles, blocks and chunks (EditorLive.h has the data): copy one and paste it over another, insert a new one, delete one, move one. The numbers of
/// the ones after it change, and everything that names them is rewritten to match: the blocks' tile words, the chunks' block words, both collision-curve tables (a curve for each block), the layout's chunks.
/// Number 0 of each kind is the blank one the engine reads as "nothing" (no tile, no collision, no chunk), so it stays where it is and cannot be inserted before, deleted, moved or pasted over.
///
/// Each call returns NULL when it was done, or why it was not (a message for the editor to show). After one that changed blocks or chunks the editor redraws the level (Editor_RedrawLevel); the tiles
/// are put into the video memory here. What changed numbers is told by EditorTerrain_Mapped, for what the editor keeps beside the data (its selection, where an overlay starts).

typedef enum { EDITOR_TERRAIN_TILE, EDITOR_TERRAIN_BLOCK, EDITOR_TERRAIN_CHUNK } EditorTerrainKind;

size_t EditorTerrain_Count(EditorTerrainKind kind);
size_t EditorTerrain_Capacity(EditorTerrainKind kind); ///< how many there can be: the tile buffer's size, 1024 blocks (a chunk's word), 256 chunks (a layout's byte)

const char *EditorTerrain_Copy(EditorTerrainKind kind, int index); ///< a block takes its two collision curves with it
const char *EditorTerrain_Paste(EditorTerrainKind kind, int index);
bool EditorTerrain_HasCopy(EditorTerrainKind kind);

const char *EditorTerrain_Insert(EditorTerrainKind kind, int at); ///< a blank one that becomes number `at` (1 to the count: the end)
const char *EditorTerrain_Delete(EditorTerrainKind kind, int index); ///< while there are at least two; what named it names number 0 now
const char *EditorTerrain_Move(EditorTerrainKind kind, int from, int to);

/// Where number `old` of that kind is after the last insert, delete or move (a deleted one is 0)
int EditorTerrain_Mapped(EditorTerrainKind kind, int old);

/// Replaces the level's terrain with a blank one to start a new level from (Keymaker mode): 3 tiles (blank, earth, grass), 3 blocks (blank, earth, grass over earth), 2 chunks, a full-solid collision curve on the earth block, a floor of the second chunk
/// across the layout's fourth row, a palette (earth, grass), and no objects or rings. The level's own planes are redrawn by the caller (Editor_RedrawLevel). False when the level has no terrain buffers to write into.
bool EditorTerrain_MakeBlank(void);

#ifdef __cplusplus
}
#endif
