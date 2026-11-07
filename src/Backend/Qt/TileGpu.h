#pragma once

// The terrain editor's pictures on the GPU (shaders/tiles.*): what a pane shows is a list of items, each a fill or a tile, block or chunk of the level (the shader looks up the blocks and tiles a block or
// chunk is made of), or the solids of a block; a TileView draws a pane's items in the part of it that is in sight. TerrainEditor.cpp lists them (its TileSink) and draws its marks over them.

#include <QRect>
#include <QRhiWidget>
#include <rhi/qrhi.h>

#include <functional>
#include <memory>
#include <vector>

struct TileItem {
	float x, y, w, h; // where, in the pane's pixels
	uint32_t index;   // the tile, block or chunk; a fill's colour (0xAARRGGBB)
	uint32_t flags;   // the kind (TileKind) and the TILE_ITEM_* flags; a tile's palette line in bits 8-9, the path whose solids are shown in bits 10-11 (1 or 2), a solid item's chunk word in bits 16-31
};

enum TileKind : uint32_t { TILE_ITEM_FILL = 0, TILE_ITEM_TILE = 1, TILE_ITEM_BLOCK = 2, TILE_ITEM_CHUNK = 3, TILE_ITEM_SOLID = 4 };
enum : uint32_t { TILE_ITEM_X_FLIP = 8, TILE_ITEM_Y_FLIP = 16, TILE_ITEM_DIM = 32, TILE_ITEM_LIVE = 64 };

class TileView final : public QRhiWidget {
public:
	explicit TileView(QWidget *parent);

	std::function<void(std::vector<TileItem> &items, const QRect &area)> list; // the items, for the part of the pane in sight (`area`, the pane's pixels)
	std::function<void()> failed; // the device cannot draw them (no storage buffers): the pane draws itself
	QRect area;                   // where the view is in the pane
	QColor background;

protected:
	void initialize(QRhiCommandBuffer *cb) override;
	void render(QRhiCommandBuffer *cb) override;
	void releaseResources() override;

private:
	std::vector<TileItem> items;
	QByteArray data;
	QRhi *made_for = nullptr;
	std::unique_ptr<QRhiBuffer> corners, instances, uniforms, storage;
	std::unique_ptr<QRhiShaderResourceBindings> bindings;
	std::unique_ptr<QRhiGraphicsPipeline> pipeline;
	bool corners_uploaded = false;
};

// Whether the terrain editor draws on the GPU (the window's picture does: PictureView)
bool TileGpu_Wanted();
