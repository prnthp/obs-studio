#include "snip-outline.hpp"

#include <obs-frontend-api.h>

#include <QPainter>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

SnipOutline::SnipOutline(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setFocusPolicy(Qt::NoFocus);

	hideTimer.setInterval(500);
	connect(&hideTimer, &QTimer::timeout, this, [this] {
		if (!obs_frontend_recording_active()) {
			hide();
		}
	});
}

void SnipOutline::ShowFor(const QRect &globalLogicalRect)
{
	// Sit just outside the recorded pixels so the border does not
	// cover (or get baked into) the edge of the video.
	setGeometry(globalLogicalRect.adjusted(-3, -3, 3, 3));
	show();
	raise();

#ifdef _WIN32
	// Windows 10 2004+: keep this window out of any capture entirely.
	SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), WDA_EXCLUDEFROMCAPTURE);
#endif

	hideTimer.start();
}

void SnipOutline::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setPen(QPen(QColor(232, 17, 35), 2));
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(rect().adjusted(1, 1, -1, -1));
}
