#pragma once

// The VDP's picture made on the GPU (shaders/vdp.comp) from the words VDP_CaptureFrame writes down: the compute pass and the picture it makes, for the window's picture (PictureView.cpp) and the
// test that checks it against the CPU's (VDP_GpuCompareFrame).

#include <QByteArray>
#include <QSize>
#include <rhi/qrhi.h>

#include <memory>

// A shader the build baked into the resources (qt_add_shaders: shaders/<name>.qsb)
QShader Paradox_LoadShader(const char *name);

class GpuVdp {
public:
	// The words of a frame and the picture's size; `afterglow` blends it with the last one (the CRT's, as Render.c does it: `alpha` the new frame's weight out of 255)
	void setFrame(const uint32_t *words, size_t count, QSize size);
	void setAfterglow(bool on, int alpha) { afterglow = on; afterglow_alpha = alpha; }

	// Whether `rhi` can run it (compute shaders)
	static bool Supported(QRhi *rhi) { return rhi->isFeatureSupported(QRhi::Compute); }

	// Puts the frame's uploads into `batch` (made again when the device or the size changed); then run() records the compute pass, which takes `batch`. picture() is what it drew.
	void prepare(QRhi *rhi, QRhiResourceUpdateBatch *batch);
	void run(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *batch);
	QRhiTexture *picture() const { return image.get(); }
	bool ready() const { return image != nullptr && !size.isEmpty(); }

	void release();

private:
	QByteArray words;
	QSize size;
	bool afterglow = false, have_last = false;
	int afterglow_alpha = 255;
	QRhi *made_for = nullptr;
	std::unique_ptr<QRhiBuffer> buffer, uniforms;
	std::unique_ptr<QRhiTexture> image;
	std::unique_ptr<QRhiShaderResourceBindings> bindings;
	std::unique_ptr<QRhiComputePipeline> pipeline;
};
