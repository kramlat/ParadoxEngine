#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QWidget>

// How the game's picture reaches the screen: through the GPU with Qt's RHI (Vulkan, or OpenGL where Vulkan does not start), or by the CPU with QPainter, the last resort. Each draws the same
// picture: the VDP's frame, scaled up to fit the widget with its aspect kept (black bars round it) and its pixels left sharp.
enum class PictureDrawing { Vulkan, OpenGL, Cpu };

// The first of Vulkan, OpenGL and the CPU that starts on this machine; PARADOX_DRAWING=vulkan, opengl or cpu asks for one (and a GPU one that does not start falls back as ever). Chosen once.
PictureDrawing PictureView_Drawing();
const char *PictureView_DrawingName(PictureDrawing drawing);

// Where a picture of `picture` pixels goes in a widget of `widget`: as big as fits, centred
QRectF PictureView_Fit(const QSizeF &widget, const QSize &picture);

// The widget that shows the picture (it takes no input: its parent's mouse and keys go on as before)
class PictureSurface {
public:
	virtual ~PictureSurface() = default;
	virtual QWidget *widget() = 0;
	virtual void setFrame(const QImage &frame) = 0; // RGBX8888
	// A frame the GPU draws (VDP_CaptureFrame's words; the CRT's afterglow as the CPU's frames have it): false when this surface cannot (the CPU's, or a device without compute)
	virtual bool setGpuFrame(const uint32_t *words, size_t count, QSize size) { (void)words; (void)count; (void)size; return false; }
	// The last GPU frame's picture, pixel for pixel (read back from the window: the console's screenshot), or a null image
	virtual QImage gpuPicture() { return QImage(); }
};
PictureSurface *PictureView_Make(QWidget *parent);
