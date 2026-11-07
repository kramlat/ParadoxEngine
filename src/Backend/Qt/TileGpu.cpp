#include "TileGpu.h"
#include "GpuVdp.h"
#include "PictureView.h"

extern "C" {
#include "../../EditorLive.h"
#include "../VDP.h"
}

#include <cstring>

bool TileGpu_Wanted() {
	return PictureView_Drawing() != PictureDrawing::Cpu;
}

namespace {

// Puts `bytes` at the end of the data on a 4 byte boundary: where they went
uint32_t Append(QByteArray &data, const void *bytes, size_t size) {
	while (data.size() & 3)
		data.append('\0');
	const uint32_t at = uint32_t(data.size());
	if (bytes != nullptr && size > 0)
		data.append(reinterpret_cast<const char *>(bytes), qsizetype(size));
	return at;
}

constexpr float kCorners[] = { 0, 0, 1, 0, 0, 1, 1, 1 };

} // namespace

TileView::TileView(QWidget *parent) : QRhiWidget(parent) {
	setApi(PictureView_Drawing() == PictureDrawing::Vulkan ? Api::Vulkan : Api::OpenGL);
	setAttribute(Qt::WA_TransparentForMouseEvents); // (the pane takes the mouse)
	setFocusPolicy(Qt::NoFocus);
}

void TileView::initialize(QRhiCommandBuffer *) {
	if (made_for != rhi()) {
		releaseResources();
		made_for = rhi();
	}
	if (!rhi()->isFeatureSupported(QRhi::Compute)) { // (what says the shaders can read a storage buffer)
		if (failed)
			failed();
		return;
	}
	if (pipeline)
		return;
	corners.reset(rhi()->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kCorners)));
	corners->create();
	uniforms.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 144));
	uniforms->create();
	storage.reset(rhi()->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, 0x40000));
	storage->create();
	bindings.reset(rhi()->newShaderResourceBindings());
	bindings->setBindings({ QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms.get()),
	                        QRhiShaderResourceBinding::bufferLoad(1, QRhiShaderResourceBinding::FragmentStage, storage.get()) });
	bindings->create();

	pipeline.reset(rhi()->newGraphicsPipeline());
	pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	pipeline->setShaderStages({ { QRhiShaderStage::Vertex, Paradox_LoadShader("tiles.vert.qsb") }, { QRhiShaderStage::Fragment, Paradox_LoadShader("tiles.frag.qsb") } });
	QRhiVertexInputLayout layout;
	layout.setBindings({ { 2 * sizeof(float) }, { sizeof(TileItem), QRhiVertexInputBinding::PerInstance } });
	layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
	                       { 1, 1, QRhiVertexInputAttribute::Float4, offsetof(TileItem, x) },
	                       { 1, 2, QRhiVertexInputAttribute::UInt, offsetof(TileItem, index) },
	                       { 1, 3, QRhiVertexInputAttribute::UInt, offsetof(TileItem, flags) } });
	pipeline->setVertexInputLayout(layout);
	QRhiGraphicsPipeline::TargetBlend blend; // (premultiplied: the fills and the solids are see-through)
	blend.enable = true;
	blend.srcColor = QRhiGraphicsPipeline::One;
	blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
	blend.srcAlpha = QRhiGraphicsPipeline::One;
	blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
	pipeline->setTargetBlends({ blend });
	pipeline->setSampleCount(sampleCount());
	pipeline->setShaderResourceBindings(bindings.get());
	pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
	pipeline->create();
	corners_uploaded = false;
}

void TileView::render(QRhiCommandBuffer *cb) {
	if (!pipeline) {
		cb->beginPass(renderTarget(), background, { 1.0f, 0 });
		cb->endPass();
		return;
	}
	QRhiResourceUpdateBatch *batch = rhi()->nextResourceUpdateBatch();
	if (!corners_uploaded) {
		batch->uploadStaticBuffer(corners.get(), kCorners);
		corners_uploaded = true;
	}

	// what the items are made of, as the editor has it now: the art, the video memory (the live tiles), the palette, the blocks, chunks, solids and the curves' heights
	data.clear();
	size_t art_size = 0, blocks_size = 0, chunks_size = 0, solids_size[2] = { 0, 0 };
	const uint8_t *art = Editor_Art(&art_size);
	const uint8_t *blocks = Editor_Blocks(&blocks_size);
	const uint8_t *chunks = Editor_Chunks(&chunks_size);
	const uint8_t *solids[2] = { Editor_Solids(0, &solids_size[0]), Editor_Solids(1, &solids_size[1]) };
	const uint8_t *heights = Editor_HeightMap();
	uint32_t palette[64];
	for (int line = 0; line < 4; line++)
		for (int i = 0; i < 16; i++) {
			const uint16_t word = Editor_GetColour(line, i);
			palette[line * 16 + i] = uint32_t((((word >> 1) & 7) * 255 / 7) << 16 | (((word >> 5) & 7) * 255 / 7) << 8 | (((word >> 9) & 7) * 255 / 7));
		}
	struct {
		float correction[16];
		float area[4];
		uint32_t art[4], blocks[4], solids[4], heights[4];
	} view;
	memcpy(view.correction, rhi()->clipSpaceCorrMatrix().constData(), sizeof(view.correction));
	view.area[0] = float(area.x());
	view.area[1] = float(area.y());
	view.area[2] = float(qMax(1, area.width()));
	view.area[3] = float(qMax(1, area.height()));
	if (art == nullptr)
		art_size = 0;
	if (heights == nullptr)
		solids_size[0] = solids_size[1] = 0; // (no curves: no solids drawn)
	view.art[0] = Append(data, art, art_size);
	view.art[1] = uint32_t(art_size);
	view.art[2] = Append(data, VDP_TileSpace(), VRAM_SIZE);
	view.art[3] = Append(data, palette, sizeof(palette)) / 4;
	view.blocks[0] = Append(data, blocks, blocks != nullptr ? blocks_size : 0);
	view.blocks[1] = blocks != nullptr ? uint32_t(blocks_size) : 0;
	view.blocks[2] = Append(data, chunks, chunks != nullptr ? chunks_size : 0);
	view.blocks[3] = chunks != nullptr ? uint32_t(chunks_size) : 0;
	for (int path = 0; path < 2; path++) {
		view.solids[path * 2] = Append(data, solids[path], solids[path] != nullptr ? solids_size[path] : 0);
		view.solids[path * 2 + 1] = solids[path] != nullptr ? uint32_t(solids_size[path]) : 0;
	}
	view.heights[0] = Append(data, heights, heights != nullptr ? 256 * 16 : 0);
	view.heights[1] = heights != nullptr ? 256 * 16 : 0;
	view.heights[2] = view.heights[3] = 0;
	Append(data, nullptr, 0);
	data.append(4, '\0'); // (a last word for the byte reads at the end)
	if (storage->size() < quint32(data.size())) {
		storage.reset(rhi()->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, quint32(data.size() * 2)));
		storage->create();
		bindings->setBindings({ QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms.get()),
		                        QRhiShaderResourceBinding::bufferLoad(1, QRhiShaderResourceBinding::FragmentStage, storage.get()) });
		bindings->create();
	}
	batch->uploadStaticBuffer(storage.get(), 0, quint32(data.size()), data.constData());
	batch->updateDynamicBuffer(uniforms.get(), 0, sizeof(view), &view);

	items.clear();
	if (list)
		list(items, area);
	if (!items.empty()) {
		const quint32 bytes = quint32(items.size() * sizeof(TileItem));
		if (!instances || instances->size() < bytes) {
			instances.reset(rhi()->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, qMax<quint32>(bytes * 2, 0x10000)));
			instances->create();
		}
		batch->uploadStaticBuffer(instances.get(), 0, bytes, items.data());
	}

	cb->beginPass(renderTarget(), background, { 1.0f, 0 }, batch);
	if (!items.empty()) {
		const QSize target = renderTarget()->pixelSize();
		cb->setGraphicsPipeline(pipeline.get());
		cb->setViewport(QRhiViewport(0, 0, float(target.width()), float(target.height())));
		cb->setShaderResources();
		const QRhiCommandBuffer::VertexInput inputs[] = { { corners.get(), 0 }, { instances.get(), 0 } };
		cb->setVertexInput(0, 2, inputs);
		cb->draw(4, quint32(items.size()));
	}
	cb->endPass();
}

void TileView::releaseResources() {
	pipeline.reset();
	bindings.reset();
	storage.reset();
	uniforms.reset();
	instances.reset();
	corners.reset();
	made_for = nullptr;
	corners_uploaded = false;
}
