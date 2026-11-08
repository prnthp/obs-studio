#pragma once

#include <QTimer>
#include <QWidget>

// Thin border drawn around the area being recorded. Click-through,
// excluded from screen capture where the OS supports it, and hides
// itself as soon as OBS is no longer recording. In window mode it
// follows the recorded window as it moves and resizes.
class SnipOutline : public QWidget {
	Q_OBJECT

public:
	explicit SnipOutline(QWidget *parent = nullptr);

	// `globalLogicalRect` is in global logical (Qt) pixels.
	void ShowFor(const QRect &globalLogicalRect);
	// Follow a native window (HWND on Windows) given in physical
	// pixels by the OS; positioned via native calls, no Qt
	// coordinate conversion involved.
	void ShowForWindow(void *windowHandle, unsigned long processId);

protected:
	void paintEvent(QPaintEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void Tick();
	void UpdateFollowRect();

	QTimer timer;
	void *followWindow = nullptr;
	unsigned long followProcessId = 0;
};
