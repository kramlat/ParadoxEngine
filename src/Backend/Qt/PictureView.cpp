#include "PictureView.h"
#include "GpuVdp.h"
extern "C" {
#include "../VDP.h"
}

#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QPainter>
#include <QRhiWidget>
#include <QLibrary>
#include <rhi/qrhi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

// Whether Vulkan works here: the loader is there, makes an instance, and the instance has a device. It is asked through the loader's own entry points (the few types it takes are written
// out below), so that the engine builds where Qt has Vulkan but the Vulkan headers are not installed; Qt's RHI does the rest with its own instance.
bool VulkanStarts() {
#if defined(Q_OS_WIN)
	QLibrary loader("vulkan-1");
#elif defined(Q_OS_MACOS)
	QLibrary loader("libvulkan.1.dylib");
#else
	QLibrary loader("vulkan", 1);
#endif
	if (!loader.load())
		return false;
	struct InstanceInfo { // VkInstanceCreateInfo
		int32_t type; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 1
		const void *next;
		uint32_t flags;
		const void *application;
		uint32_t layers;
		const char *const *layer_names;
		uint32_t extensions;
		const char *const *extension_names;
	};
	using Instance = void *;
	using GetProc = void *(*)(Instance, const char *);
	using Create = int32_t (*)(const InstanceInfo *, const void *, Instance *);
	using Destroy = void (*)(Instance, const void *);
	using Devices = int32_t (*)(Instance, uint32_t *, void **);
	const auto get_proc = reinterpret_cast<GetProc>(loader.resolve("vkGetInstanceProcAddr"));
	if (get_proc == nullptr)
		return false;
	const auto create = reinterpret_cast<Create>(get_proc(nullptr, "vkCreateInstance"));
	if (create == nullptr)
		return false;
	const InstanceInfo info = { 1, nullptr, 0, nullptr, 0, nullptr, 0, nullptr };
	Instance instance = nullptr;
	if (create(&info, nullptr, &instance) != 0 || instance == nullptr)
		return false;
	const auto devices = reinterpret_cast<Devices>(get_proc(instance, "vkEnumeratePhysicalDevices"));
	const auto destroy = reinterpret_cast<Destroy>(get_proc(instance, "vkDestroyInstance"));
	uint32_t count = 0;
	const bool found = devices != nullptr && devices(instance, &count, nullptr) == 0 && count > 0;
	if (destroy != nullptr)
		destroy(instance, nullptr);
	return found;
}

// Whether Qt's RHI starts on this machine with `impl`: for OpenGL, a device made and let go again
bool Starts(QRhi::Implementation impl) {
	if (impl == QRhi::Vulkan)
		return VulkanStarts();
	std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
	QRhiGles2InitParams params;
	params.fallbackSurface = surface.get();
	std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));
	return rhi != nullptr;
}

// The picture through the GPU: a texture drawn as one quad into the part of the widget it fits, with the nearest texel for each pixel. The texture is the CPU's frame, uploaded, or the
// one the VDP's compute shader made (GpuVdp) from the frame's words.
class RhiPicture final : public QRhiWidget, public PictureSurface {
public:
	RhiPicture(Api api, QWidget *parent) : QRhiWidget(parent) {
		setApi(api);
		setAttribute(Qt::WA_TransparentForMouseEvents); // (the parent takes the mouse)
		setFocusPolicy(Qt::NoFocus);
		vdp.setAfterglow(true, 176); // (Render.c's CRT_MOTION_BLUR_ALPHA)
	}

	QWidget *widget() override { return this; }

	void setFrame(const QImage &image) override {
		frame = image;
		frame_new = true;
		showing_gpu = false;
		update();
	}

	bool setGpuFrame(const uint32_t *words, size_t count, QSize size) override {
		if (!compute)
			return false;
		vdp.setFrame(words, count, size);
		gpu_new = true;
		showing_gpu = true;
		update();
		return true;
	}

	QImage gpuPicture() override {
		if (!showing_gpu || !vdp.ready())
			return QImage();
		const QImage shot = grabFramebuffer(); // (the widget's pixels: the picture scaled up into the part it fits, which nearest sampling takes back down)
		const QSize picture = vdp.picture()->pixelSize();
		const QRectF fit = PictureView_Fit(QSizeF(shot.size()), picture);
		QImage out(picture, QImage::Format_RGBX8888);
		for (int y = 0; y < picture.height(); y++)
			for (int x = 0; x < picture.width(); x++)
				out.setPixel(x, y, shot.pixel(qBound(0, int(fit.x() + (x + 0.5) * fit.width() / picture.width()), shot.width() - 1),
				                              qBound(0, int(fit.y() + (y + 0.5) * fit.height() / picture.height()), shot.height() - 1)));
		return out;
	}

protected:
	void initialize(QRhiCommandBuffer *) override {
		if (made_for != rhi()) { // a new device (the first, or after the window moved to another): everything again
			releaseResources();
			made_for = rhi();
			first_upload = true;
		}
		compute = GpuVdp::Supported(rhi());
		if (!compute)
			VDP_UseGpu(false); // (no compute shaders on this device: the CPU draws the frames, this shows them)
		if (pipeline)
			return;
		vertices.reset(rhi()->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuad)));
		vertices->create();
		uniforms.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64));
		uniforms->create();
		sampler.reset(rhi()->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
		sampler->create();
		MakeTexture(frame.isNull() ? QSize(1, 1) : frame.size());
		upload_bindings.reset(rhi()->newShaderResourceBindings());
		Bind(upload_bindings.get(), texture.get());

		pipeline.reset(rhi()->newGraphicsPipeline());
		pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
		pipeline->setShaderStages({ { QRhiShaderStage::Vertex, Paradox_LoadShader("picture.vert.qsb") }, { QRhiShaderStage::Fragment, Paradox_LoadShader("picture.frag.qsb") } });
		QRhiVertexInputLayout layout;
		layout.setBindings({ { 2 * sizeof(float) } });
		layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
		pipeline->setVertexInputLayout(layout);
		pipeline->setSampleCount(sampleCount());
		pipeline->setShaderResourceBindings(upload_bindings.get());
		pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
		pipeline->create();
	}

	void render(QRhiCommandBuffer *cb) override {
		QRhiResourceUpdateBatch *batch = rhi()->nextResourceUpdateBatch();
		if (first_upload) {
			batch->uploadStaticBuffer(vertices.get(), kQuad);
			const QMatrix4x4 correction = rhi()->clipSpaceCorrMatrix(); // (Vulkan's Y points down: this turns it the way the shader thinks)
			batch->updateDynamicBuffer(uniforms.get(), 0, 64, correction.constData());
			first_upload = false;
		}
		QSize picture;
		QRhiShaderResourceBindings *bindings = upload_bindings.get();
		if (showing_gpu) {
			if (gpu_new) {
				QRhiTexture *before = vdp.picture();
				vdp.prepare(rhi(), batch);
				vdp.run(cb, batch); // (the uploads go with the compute pass)
				batch = nullptr;
				gpu_new = false;
				if (vdp.picture() != before || !gpu_bindings) { // (a new picture: its size changed)
					if (!gpu_bindings)
						gpu_bindings.reset(rhi()->newShaderResourceBindings());
					Bind(gpu_bindings.get(), vdp.picture());
				}
			}
			if (vdp.ready()) {
				picture = vdp.picture()->pixelSize();
				bindings = gpu_bindings.get();
			}
		} else if (!frame.isNull()) {
			if (frame_new) {
				if (texture->pixelSize() != frame.size()) {
					MakeTexture(frame.size());
					Bind(upload_bindings.get(), texture.get());
				}
				batch->uploadTexture(texture.get(), QRhiTextureUploadDescription({ 0, 0, QRhiTextureSubresourceUploadDescription(frame) }));
				frame_new = false;
			}
			picture = frame.size();
		}

		const QSize target = renderTarget()->pixelSize();
		cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, batch);
		if (!picture.isEmpty() && bindings != nullptr) {
			const QRectF fit = PictureView_Fit(QSizeF(target), picture);
			cb->setGraphicsPipeline(pipeline.get());
			cb->setViewport(QRhiViewport(float(fit.x()), float(target.height() - fit.bottom()), float(fit.width()), float(fit.height()))); // (from the bottom left)
			cb->setShaderResources(bindings);
			const QRhiCommandBuffer::VertexInput input(vertices.get(), 0);
			cb->setVertexInput(0, 1, &input);
			cb->draw(4);
		}
		cb->endPass();
	}

	void releaseResources() override {
		pipeline.reset();
		upload_bindings.reset();
		gpu_bindings.reset();
		texture.reset();
		sampler.reset();
		vertices.reset();
		uniforms.reset();
		vdp.release();
		made_for = nullptr;
		gpu_new = true; // (the frame's words are kept: drawn again on the new device)
	}

private:
	static constexpr float kQuad[] = { 0, 0, 1, 0, 0, 1, 1, 1 }; // a strip: the picture's corners, (0,0) its bottom left

	void MakeTexture(const QSize &size) {
		texture.reset(rhi()->newTexture(QRhiTexture::RGBA8, size));
		texture->create();
	}

	void Bind(QRhiShaderResourceBindings *bindings, QRhiTexture *picture) {
		bindings->setBindings({ QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniforms.get()),
		                        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, picture, sampler.get()) });
		bindings->create();
	}

	QImage frame;
	bool frame_new = false, showing_gpu = false, gpu_new = false;
	bool first_upload = true;
	bool compute = true; // (until the device says otherwise)
	GpuVdp vdp;
	QRhi *made_for = nullptr;
	std::unique_ptr<QRhiGraphicsPipeline> pipeline;
	std::unique_ptr<QRhiShaderResourceBindings> upload_bindings, gpu_bindings;
	std::unique_ptr<QRhiTexture> texture;
	std::unique_ptr<QRhiSampler> sampler;
	std::unique_ptr<QRhiBuffer> vertices, uniforms;
};

// The picture by the CPU: QPainter scales it into the widget
class CpuPicture final : public QWidget, public PictureSurface {
public:
	explicit CpuPicture(QWidget *parent) : QWidget(parent) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setFocusPolicy(Qt::NoFocus);
	}

	QWidget *widget() override { return this; }

	void setFrame(const QImage &image) override {
		frame = image;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.fillRect(rect(), Qt::black);
		if (frame.isNull())
			return;
		p.setRenderHint(QPainter::SmoothPixmapTransform, false); // crisp pixels, like the GPU's nearest texels
		p.drawImage(PictureView_Fit(QSizeF(size()), frame.size()), frame);
	}

private:
	QImage frame;
};

} // namespace

PictureDrawing PictureView_Drawing() {
	static int chosen = -1;
	if (chosen >= 0)
		return PictureDrawing(chosen);
	const char *wanted = getenv("PARADOX_DRAWING");
	PictureDrawing drawing = PictureDrawing::Cpu;
	const bool headless = QGuiApplication::platformName().startsWith("offscreen") || QGuiApplication::platformName().startsWith("minimal");
	if (wanted != nullptr && strcmp(wanted, "cpu") == 0) {
		drawing = PictureDrawing::Cpu;
	} else if (headless) {
		drawing = PictureDrawing::Cpu; // (no window to show anything in: the frames are read from the CPU's picture anyway)
	} else if (!(wanted != nullptr && strcmp(wanted, "opengl") == 0) && Starts(QRhi::Vulkan)) {
		drawing = PictureDrawing::Vulkan;
	} else if (Starts(QRhi::OpenGLES2)) {
		drawing = PictureDrawing::OpenGL;
	}
	fprintf(stderr, "Drawing the picture with %s\n", PictureView_DrawingName(drawing));
	chosen = int(drawing);
	return drawing;
}

const char *PictureView_DrawingName(PictureDrawing drawing) {
	switch (drawing) {
	case PictureDrawing::Vulkan: return "Vulkan";
	case PictureDrawing::OpenGL: return "OpenGL";
	default:                     return "the CPU";
	}
}

QRectF PictureView_Fit(const QSizeF &widget, const QSize &picture) {
	if (picture.isEmpty())
		return QRectF();
	const double scale = qMin(widget.width() / picture.width(), widget.height() / picture.height());
	const double w = picture.width() * scale, h = picture.height() * scale;
	return QRectF((widget.width() - w) / 2, (widget.height() - h) / 2, w, h);
}

PictureSurface *PictureView_Make(QWidget *parent) {
	switch (PictureView_Drawing()) {
	case PictureDrawing::Vulkan: return new RhiPicture(QRhiWidget::Api::Vulkan, parent);
	case PictureDrawing::OpenGL: return new RhiPicture(QRhiWidget::Api::OpenGL, parent);
	default:                     return new CpuPicture(parent);
	}
}
