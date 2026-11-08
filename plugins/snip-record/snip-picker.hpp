#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

#include "snip-selection.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
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

	bool InstantStart() const;
	// 0 = do nothing, 1 = open video, 2 = show in folder.
	int PostAction() const;
	void SetInstantStartChecked(bool checked);
	void SetPostAction(int action);

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

	QWidget *toolbar = nullptr;
	QPushButton *regionButton = nullptr;
	QPushButton *windowButton = nullptr;
	QPushButton *fullScreenButton = nullptr;
	QPushButton *recordButton = nullptr;
	QCheckBox *matchCanvasCheck = nullptr;
	QCheckBox *instantStartCheck = nullptr;
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

	// Full Screen mode: logical geometry of the display under the
	// cursor (primary display by default).
	QRect fullScreenRect;

	bool recordingBlocked = false;
};
