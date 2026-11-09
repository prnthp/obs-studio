#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

#include <vector>

#include "snip-selection.hpp"

class SnipScreenOverlay;

class QCheckBox;
class QComboBox;
class QLabel;
class QPainter;
class QPushButton;
class QScreen;
class SnipOutline;
class SnipRecorder;

// Fullscreen, Snipping Tool-style overlay covering the virtual desktop.
// Drag to pick a region, click a window, or click a display, then press
// Enter (or the Record button) to start recording immediately.
class SnipPicker : public QWidget {
	Q_OBJECT

public:
	explicit SnipPicker(SnipRecorder *recorder, QWidget *parent = nullptr);

	void OpenPicker();

	// Reflects the persisted "match canvas" setting in the toolbar.
	void SetMatchCanvasChecked(bool checked, bool userSet);

	// Shows the recording outline for the last confirmed selection.
	// Called by the module on RECORDING_STARTED, so the outline only
	// appears once recording really starts (window mode waits for
	// the capture to hook first).
	void ShowOutlineForLastSelection();
	void HideOutline();

	bool InstantStart() const;
	// 0 = do nothing, 1 = open video, 2 = show in folder.
	int PostAction() const;
	void SetInstantStartChecked(bool checked);
	void SetPostAction(int action);

	// What goes into the recording: desktop audio (channels 1-2),
	// microphone (channels 3-6) and the mouse pointer.
	bool DesktopAudio() const;
	bool MicAudio() const;
	bool CaptureCursor() const;
	void SetCaptureOptions(bool desktopAudio, bool micAudio, bool cursor);

	// Optional animated export converted from the MP4 after each
	// snip: 0 = none, 1 = GIF, 2 = WebP (external ffmpeg required).
	int ExportFormat() const;
	void SetExportFormat(int format);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	friend class SnipScreenOverlay;

	// Mouse handling in global logical pixels, shared by this
	// window and the per-screen overlay windows (macOS).
	void HandleMousePress(const QPoint &globalPos);
	void HandleMouseMove(const QPoint &globalPos);
	void HandleMouseRelease(const QPoint &globalPos);
	void HandleMouseDoubleClick(const QPoint &globalPos);

	// Dim + selection highlight painting for one overlay window
	// covering `widgetRect` at global `origin`.
	void PaintSelection(QPainter &painter, const QRect &widgetRect, const QPoint &origin);

	// macOS: one extra overlay window per additional display (a
	// single window spanning displays at different offsets/scales
	// does not receive events on every display there).
	void CreateScreenOverlays(QScreen *pickerScreen);
	void HideScreenOverlays();
	void RefreshOverlays();

	void SetMode(SnipSelection::Mode mode);
	bool CanRecord() const;
	void UpdateToolbar();
	void UpdateHoverWindow(const QPoint &globalPos);
	void SelectWindowAt(const QPoint &globalPos);
	bool BuildSelection(SnipSelection &selection) const;
	void StartRecording();
	void MaybeInstantStart();

	// Rect currently highlighted/selected, in global logical pixels.
	QRect ActiveRect() const;

	SnipRecorder *recorder;

	// Additional per-display overlay windows (macOS only).
	std::vector<QWidget *> screenOverlays;

	QWidget *toolbar = nullptr;
	QPushButton *regionButton = nullptr;
	QPushButton *windowButton = nullptr;
	QPushButton *fullScreenButton = nullptr;
	QPushButton *recordButton = nullptr;
	QCheckBox *matchCanvasCheck = nullptr;
	QCheckBox *instantStartCheck = nullptr;
	QCheckBox *desktopAudioCheck = nullptr;
	QCheckBox *micAudioCheck = nullptr;
	QCheckBox *cursorCheck = nullptr;
	QComboBox *exportCombo = nullptr;
	QComboBox *postActionCombo = nullptr;
	QLabel *hintLabel = nullptr;
	SnipOutline *outline = nullptr;

	SnipSelection::Mode mode = SnipSelection::Mode::Region;

	// Region mode drag state (global logical pixels).
	bool dragging = false;
	QPoint dragStart;
	QPoint dragCurrent;
	QRect regionRect;
	// Last confirmed region; pre-selected when the picker reopens.
	QRect lastRegion;

	// Window mode state. windowValue/windowPhysicalRect are captured
	// when the window is clicked.
	QRect hoverWindowRect;
	bool hasHoverWindow = false;
	QRect selectedWindowRect;
	bool hasSelectedWindow = false;
	QString selectedWindowValue;
	QRect selectedWindowPhysicalRect;
	void *selectedWindowHandle = nullptr;
	unsigned long selectedWindowPid = 0;

	// Last selection confirmed with Start, for the outline.
	SnipSelection lastSelection;
	bool hasLastSelection = false;

	// Full Screen mode: logical geometry of the display under the
	// cursor (primary display by default).
	QRect fullScreenRect;

	bool recordingBlocked = false;
};
