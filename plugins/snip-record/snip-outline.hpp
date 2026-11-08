#pragma once

#include <QTimer>
#include <QWidget>

// Thin border drawn around the region being recorded. Click-through,
// excluded from screen capture where the OS supports it, and hides
// itself as soon as OBS is no longer recording.
class SnipOutline : public QWidget {
	Q_OBJECT

public:
	explicit SnipOutline(QWidget *parent = nullptr);

	// `globalLogicalRect` is in global logical (Qt) pixels.
	void ShowFor(const QRect &globalLogicalRect);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QTimer hideTimer;
};
