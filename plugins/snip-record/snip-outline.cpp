#include "snip-outline.hpp"

#include <obs-frontend-api.h>

#include <QPainter>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dwmapi.h>
#endif
#ifdef __APPLE__
#include "snip-platform-mac.hpp"

#include <cstdint>
#endif

SnipOutline::SnipOutline(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setFocusPolicy(Qt::NoFocus);

	timer.setInterval(100);
	connect(&timer, &QTimer::timeout, this, &SnipOutline::Tick);
}

void SnipOutline::ShowFor(const QRect &globalLogicalRect)
{
	followWindow = nullptr;
	// Sit just outside the recorded pixels so the border does not
	// cover (or get baked into) the edge of the video.
	setGeometry(globalLogicalRect.adjusted(-2, -2, 2, 2));
	show();
	raise();

#ifdef _WIN32
	// Windows 10 2004+: keep this window out of any capture entirely.
	SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), WDA_EXCLUDEFROMCAPTURE);
#elif defined(__APPLE__)
	snip_mac::ConfigureOutlineWindow(this);
#endif

	timer.start();
}

void SnipOutline::ShowForWindow(void *windowHandle, unsigned long processId)
{
	followWindow = windowHandle;
	followProcessId = processId;
	show();

#ifdef _WIN32
	SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), WDA_EXCLUDEFROMCAPTURE);
#elif defined(__APPLE__)
	snip_mac::ConfigureOutlineWindow(this);
#endif

	UpdateFollowRect();
	timer.start();
}

void SnipOutline::hideEvent(QHideEvent *event)
{
	timer.stop();
	followWindow = nullptr;
	QWidget::hideEvent(event);
}

void SnipOutline::Tick()
{
	if (followWindow) {
		UpdateFollowRect();
	} else if (!obs_frontend_recording_active()) {
		hide();
	}
}

void SnipOutline::UpdateFollowRect()
{
#ifdef _WIN32
	HWND hwnd = reinterpret_cast<HWND>(followWindow);
	if (!hwnd || !IsWindow(hwnd)) {
		hide();
		return;
	}

	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid != followProcessId || !obs_frontend_recording_active()) {
		hide();
		return;
	}

	RECT rect = {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
		GetWindowRect(hwnd, &rect);
	}

	// Native positioning in physical pixels: the border tracks the
	// window even across displays with different DPI scales, with no
	// logical<->physical conversion to drift. (A minimized window
	// reports off-screen coordinates, which parks the border too.)
	SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, rect.left - 2, rect.top - 2,
		     (rect.right - rect.left) + 4, (rect.bottom - rect.top) + 4, SWP_NOACTIVATE | SWP_SHOWWINDOW);
#elif defined(__APPLE__)
	// followWindow carries a CGWindowID (see the picker).
	const uint32_t windowId = (uint32_t)(uintptr_t)followWindow;
	QRect bounds;
	unsigned long pid = 0;
	if (windowId == 0 || !snip_mac::WindowBounds(windowId, bounds, &pid) || pid != followProcessId ||
	    !obs_frontend_recording_active()) {
		hide();
		return;
	}
	// CGWindow bounds and Qt global geometry share points/top-left.
	setGeometry(bounds.adjusted(-2, -2, 2, 2));
	raise();
#else
	hide();
#endif
}

void SnipOutline::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setPen(QPen(QColor(232, 17, 35), 1));
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(rect().adjusted(0, 0, -1, -1));
}
