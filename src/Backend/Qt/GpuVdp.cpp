#include "GpuVdp.h"

extern "C" {
#include "../VDP.h"
}

#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#if __has_include(<vulkan/vulkan.h>)
#include <QVulkanInstance>
#define PARADOX_TEST_VULKAN 1
#endif

#include <cstdio>

QShader Paradox_LoadShader(const char *name) {
	QFile file(QString(":/paradox/shaders/") + name);
	return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader();
}

void GpuVdp::setFrame(const uint32_t *frame_words, size_t count, QSize picture_size) {
	words = QByteArray(reinterpret_cast<const char *>(frame_words), qsizetype(count * sizeof(uint32_t)));
	size = picture_size;
}

void GpuVdp::prepare(QRhi *rhi, QRhiResourceUpdateBatch *batch) {
	if (made_for != rhi) {
		release();
		made_for = rhi;
	}
	if (!uniforms) {
		uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
		uniforms->create();
	}
	bool rebind = !bindings;
	if (!buffer || buffer->size() < quint32(words.size())) {
		buffer.reset(rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, quint32(qMax<qsizetype>(words.size() * 2, 0x40000))));
		buffer->create();
		rebind = true;
	}
	if (!image || image->pixelSize() != size) {
		image.reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::UsedWithLoadStore));
		image->create();
		have_last = false; // (nothing to blend with)
		rebind = true;
	}
	if (rebind) {
		if (!bindings)
			bindings.reset(rhi->newShaderResourceBindings());
		bindings->setBindings({ QRhiShaderResourceBinding::bufferLoad(0, QRhiShaderResourceBinding::ComputeStage, buffer.get()),
		                        QRhiShaderResourceBinding::imageLoadStore(1, QRhiShaderResourceBinding::ComputeStage, image.get(), 0),
		                        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::ComputeStage, uniforms.get()) });
		bindings->create();
	}
	if (!pipeline) {
		pipeline.reset(rhi->newComputePipeline());
		pipeline->setShaderStage({ QRhiShaderStage::Compute, Paradox_LoadShader("vdp.comp.qsb") });
		pipeline->setShaderResourceBindings(bindings.get());
		pipeline->create();
	}
	batch->uploadStaticBuffer(buffer.get(), 0, quint32(words.size()), words.constData());
	const qint32 afterglow_words[4] = { afterglow && have_last ? 1 : 0, afterglow_alpha, 0, 0 };
	batch->updateDynamicBuffer(uniforms.get(), 0, sizeof(afterglow_words), afterglow_words);
}

void GpuVdp::run(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *batch) {
	cb->beginComputePass(batch);
	cb->setComputePipeline(pipeline.get());
	cb->setShaderResources(bindings.get());
	cb->dispatch((size.width() + 7) / 8, (size.height() + 7) / 8, 1);
	cb->endComputePass();
	have_last = true;
}

void GpuVdp::release() {
	pipeline.reset();
	bindings.reset();
	image.reset();
	buffer.reset();
	uniforms.reset();
	made_for = nullptr;
	have_last = false;
}

// ---- the tests' check: the GPU's picture of a captured frame against the CPU's ----

namespace {

struct TestDevice {
	std::unique_ptr<QOffscreenSurface> surface;
#ifdef PARADOX_TEST_VULKAN
	QVulkanInstance vulkan;
#endif
	std::unique_ptr<QRhi> rhi;
	GpuVdp vdp;
	bool tried = false;

	QRhi *Get() {
		if (tried)
			return rhi.get();
		tried = true;
		if (QGuiApplication::instance() == nullptr) {
			static int argc = 1;
			static char arg0[] = "paradox-tests";
			static char *argv[] = { arg0, nullptr };
			new QGuiApplication(argc, argv);
		}
#ifdef PARADOX_TEST_VULKAN
		vulkan.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
		if (vulkan.create()) {
			QRhiVulkanInitParams params;
			params.inst = &vulkan;
			rhi.reset(QRhi::create(QRhi::Vulkan, &params));
		}
#endif
		if (rhi == nullptr) {
			surface.reset(QRhiGles2InitParams::newFallbackSurface());
			QRhiGles2InitParams params;
			params.fallbackSurface = surface.get();
			rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
		}
		if (rhi != nullptr && !GpuVdp::Supported(rhi.get()))
			rhi.reset();
		if (rhi != nullptr)
			fprintf(stderr, "(the GPU's VDP checked on %s)\n", rhi->backendName());
		return rhi.get();
	}
};

} // namespace

extern "C" int VDP_GpuCompareFrame(const VDP_GpuFrame *frame, const uint32_t *cpu, int pitch) {
	static TestDevice &device = *new TestDevice; // (never torn down: at exit Qt's per-thread state, which a device's teardown reads, is gone already)
	QRhi *rhi = device.Get();
	if (rhi == nullptr)
		return -1;
	device.vdp.setFrame(frame->words, frame->count, QSize(frame->width, frame->rows));
	QRhiCommandBuffer *cb = nullptr;
	if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
		return -1;
	QRhiResourceUpdateBatch *batch = rhi->nextResourceUpdateBatch();
	device.vdp.prepare(rhi, batch);
	device.vdp.run(cb, batch);
	QRhiReadbackResult result;
	QRhiResourceUpdateBatch *readback = rhi->nextResourceUpdateBatch();
	readback->readBackTexture({ device.vdp.picture() }, &result);
	cb->resourceUpdate(readback);
	rhi->endOffscreenFrame();

	const uint8_t *gpu = reinterpret_cast<const uint8_t *>(result.data.constData());
	if (result.data.size() < qsizetype(frame->width) * frame->rows * 4)
		return -1;
	int different = 0;
	for (int y = 0; y < frame->rows; y++) {
		for (int x = 0; x < frame->width; x++) {
			const uint8_t *g = gpu + (size_t(y) * size_t(frame->width) + size_t(x)) * 4;
			const uint32_t c = cpu[size_t(y) * size_t(pitch) + size_t(x)];
			const uint32_t got = (uint32_t(g[0]) << 24) | (uint32_t(g[1]) << 16) | (uint32_t(g[2]) << 8) | g[3];
			if (got != c) {
				if (different == 0)
					fprintf(stderr, "(the GPU's VDP: first difference at %d,%d: %08x, the CPU's %08x)\n", x, y, got, c);
				different++;
			}
		}
	}
	return different;
}
