#include "snip-picker.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include "snip-outline.hpp"
#include "snip-recorder.hpp"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dwmapi.h>
#endif

namespace {

constexpr int kMinRegionSize = 8;

snip::Rect ToSnipRect(const QRect &rect)
{
	return {rect.x(), rect.y(), rect.width(), rect.height()};
}

#ifdef _WIN32

struct MonitorEntry {
	HMONITOR handle = nullptr;
	RECT rect = {};
	std::string deviceId;
	std::string gdiName;
};

BOOL CALLBACK CollectMonitors(HMONITOR handle, HDC, LPRECT, LPARAM param)
{
	auto *entries = reinterpret_cast<std::vector<MonitorEntry> *>(param);
	MONITORINFOEXA info = {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoA(handle, &info)) {
		return TRUE;
	}

	MonitorEntry entry;
	entry.handle = handle;
	entry.rect = info.rcMonitor;
	entry.gdiName = info.szDevice;

	DISPLAY_DEVICEA device = {};
	device.cb = sizeof(device);
	if (EnumDisplayDevicesA(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
		entry.deviceId = device.DeviceID;
	} else {
		entry.deviceId = info.szDevice;
	}

	entries->push_back(std::move(entry));
	return TRUE;
}

std::vector<MonitorEntry> AllMonitors()
{
	std::vector<MonitorEntry> entries;
	EnumDisplayMonitors(nullptr, nullptr, CollectMonitors, reinterpret_cast<LPARAM>(&entries));
	return entries;
}

// Finds the Win32 monitor matching a Qt screen. QScreen::name() on
// Windows is the GDI device name (e.g. "\\.\DISPLAY1"), which is what
// MONITORINFOEX::szDevice reports.
const MonitorEntry *MonitorForScreen(const std::vector<MonitorEntry> &monitors, QScreen *screen)
{
	if (!screen) {
		return monitors.empty() ? nullptr : &monitors.front();
	}
	const QByteArray name = screen->name().toLocal8Bit();
	for (const MonitorEntry &entry : monitors) {
		if (entry.gdiName == name.constData()) {
			return &entry;
		}
	}
	return monitors.empty() ? nullptr : &monitors.front();
}

const MonitorEntry *MonitorAtPhysicalPoint(const std::vector<MonitorEntry> &monitors, const RECT &rect)
{
	const POINT center = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
	for (const MonitorEntry &entry : monitors) {
		if (PtInRect(&entry.rect, center)) {
			return &entry;
		}
	}
	return monitors.empty() ? nullptr : &monitors.front();
}

// Same escaping win-capture's ms_build_window_strings() expects:
// '#' -> "#22" first, then ':' -> "#3A".
QString EncodeWindowComponent(QString value)
{
	value.replace("#", "#22");
	value.replace(":", "#3A");
	return value;
}

QString WindowValueForHWND(HWND hwnd)
{
	const int titleLength = GetWindowTextLengthW(hwnd);
	std::wstring title(titleLength > 0 ? titleLength + 1 : 1, L'\0');
	if (titleLength > 0) {
		GetWindowTextW(hwnd, title.data(), int(title.size()));
	}
	title.resize(titleLength > 0 ? titleLength : 0);

	wchar_t className[256] = {};
	GetClassNameW(hwnd, className, int(std::size(className)));

	QString executable;
	DWORD processId = 0;
	if (GetWindowThreadProcessId(hwnd, &processId) && processId != 0) {
		if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)) {
			wchar_t path[1024] = {};
			DWORD pathSize = DWORD(std::size(path));
			if (QueryFullProcessImageNameW(process, 0, path, &pathSize)) {
				const QString fullPath = QString::fromWCharArray(path, int(pathSize));
				executable = fullPath.mid(fullPath.lastIndexOf('\\') + 1);
			}
			CloseHandle(process);
		}
	}

	return EncodeWindowComponent(QString::fromStdWString(title)) + ":" +
	       EncodeWindowComponent(QString::fromWCharArray(className)) + ":" + EncodeWindowComponent(executable);
}

struct WindowSearch {
	POINT point;
	HWND exclude;
	HWND found = nullptr;
	RECT rect = {};
};

BOOL CALLBACK FindWindowAtPoint(HWND hwnd, LPARAM param)
{
	auto *search = reinterpret_cast<WindowSearch *>(param);
	if (hwnd == search->exclude || !IsWindowVisible(hwnd)) {
		return TRUE;
	}

	DWORD cloaked = 0;
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
		return TRUE;
	}

	char className[256] = {};
	GetClassNameA(hwnd, className, int(std::size(className)));
	if (std::string(className) == "Progman" || std::string(className) == "WorkerW" ||
	    std::string(className) == "Shell_TrayWnd" || std::string(className) == "Shell_SecondaryTrayWnd") {
		return TRUE;
	}

	RECT rect = {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
		GetWindowRect(hwnd, &rect);
	}
	if (rect.right - rect.left < 16 || rect.bottom - rect.top < 16 || !PtInRect(&rect, search->point)) {
		return TRUE;
	}

	search->found = hwnd;
	search->rect = rect;
	return FALSE; // EnumWindows visits in Z order: first hit is topmost
}

#endif // _WIN32

} // namespace

SnipPicker::SnipPicker(SnipRecorder *recorder_, QWidget *parent) : QWidget(parent), recorder(recorder_)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setCursor(Qt::CrossCursor);

	toolbar = new QWidget(this);
	toolbar->setStyleSheet("background: rgba(28, 28, 30, 235); border-radius: 8px;");
	toolbar->setCursor(Qt::ArrowCursor);

	auto *modeGroup = new QButtonGroup(toolbar);
	auto makeModeButton = [this, modeGroup](const char *textKey) {
		auto *button = new QPushButton(obs_module_text(textKey), toolbar);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus); // keep Enter/Esc handling on the picker
		button->setStyleSheet("QPushButton { color: #ddd; border: none; padding: 6px 12px; }"
				      "QPushButton:checked { background: #2f6fd0; color: white; border-radius: 5px; }");
		modeGroup->addButton(button);
		return button;
	};
	regionButton = makeModeButton("SnipRecord.Picker.Region");
	windowButton = makeModeButton("SnipRecord.Picker.Window");
	fullScreenButton = makeModeButton("SnipRecord.Picker.FullScreen");

	recordButton = new QPushButton(obs_module_text("SnipRecord.Picker.Record"), toolbar);
	recordButton->setFocusPolicy(Qt::NoFocus);
	recordButton->setStyleSheet(
		"QPushButton { background: #c0392b; color: white; border: none; padding: 6px 14px; border-radius: 5px; }"
		"QPushButton:disabled { background: #555; color: #999; }");
	auto *cancelButton = new QPushButton(obs_module_text("SnipRecord.Picker.Cancel"), toolbar);
	cancelButton->setFocusPolicy(Qt::NoFocus);
	cancelButton->setStyleSheet("QPushButton { color: #ddd; border: none; padding: 6px 12px; }");

	matchCanvasCheck = new QCheckBox(obs_module_text("SnipRecord.Picker.MatchCanvas"), toolbar);
	matchCanvasCheck->setFocusPolicy(Qt::NoFocus);
	matchCanvasCheck->setStyleSheet("QCheckBox { color: #ddd; padding: 2px 6px; }");

	hintLabel = new QLabel(toolbar);
	hintLabel->setStyleSheet("color: #aaa; padding: 2px 12px;");

	auto *buttonsRow = new QHBoxLayout;
	buttonsRow->setContentsMargins(6, 6, 6, 0);
	buttonsRow->addWidget(regionButton);
	buttonsRow->addWidget(windowButton);
	buttonsRow->addWidget(fullScreenButton);
	buttonsRow->addSpacing(12);
	buttonsRow->addWidget(recordButton);
	buttonsRow->addWidget(cancelButton);

	auto *optionsRow = new QHBoxLayout;
	optionsRow->setContentsMargins(6, 0, 6, 0);
	optionsRow->addWidget(matchCanvasCheck);
	optionsRow->addStretch();

	auto *toolbarLayout = new QVBoxLayout(toolbar);
	toolbarLayout->setContentsMargins(0, 0, 0, 6);
	toolbarLayout->addLayout(buttonsRow);
	toolbarLayout->addLayout(optionsRow);
	toolbarLayout->addWidget(hintLabel);

	connect(regionButton, &QPushButton::clicked, this, [this] { SetMode(SnipSelection::Mode::Region); });
	connect(windowButton, &QPushButton::clicked, this, [this] { SetMode(SnipSelection::Mode::Window); });
	connect(fullScreenButton, &QPushButton::clicked, this, [this] { SetMode(SnipSelection::Mode::FullScreen); });
	connect(recordButton, &QPushButton::clicked, this, &SnipPicker::StartRecording);
	connect(cancelButton, &QPushButton::clicked, this, &QWidget::hide);
	connect(matchCanvasCheck, &QCheckBox::toggled, this, [this](bool checked) {
		recorder->SetMatchCanvas(checked);
		obs_frontend_save(); // persist via the module's save callback
	});

	outline = new SnipOutline();

	SetMode(SnipSelection::Mode::Region);
}

void SnipPicker::SetMatchCanvasChecked(bool checked)
{
	const QSignalBlocker blocker(matchCanvasCheck);
	matchCanvasCheck->setChecked(checked);
	recorder->SetMatchCanvas(checked);
}

void SnipPicker::OpenPicker()
{
	recordingBlocked = obs_frontend_recording_active();
	dragging = false;
	regionRect = lastRegion; // pre-select the previous region, if any
	hasHoverWindow = false;
	hasSelectedWindow = false;

	QScreen *primary = QGuiApplication::primaryScreen();
	setGeometry(primary ? primary->virtualGeometry() : QRect(0, 0, 1920, 1080));
	fullScreenRect = primary ? primary->geometry() : geometry();

	toolbar->adjustSize();
	const int toolbarX = geometry().width() / 2 - toolbar->width() / 2;
	toolbar->move(std::max(0, toolbarX), 16);

	SetMode(mode);
	UpdateToolbar();
	show();
	raise();
	activateWindow();
	setFocus(Qt::ActiveWindowFocusReason);
}

// The picker is a topmost tool window and may not hold keyboard focus
// (e.g. when invoked while another app is focused), which silently
// swallows the 1/2/3/Enter/Esc keys. Grab the keyboard for the short
// time the overlay is up so mode switching always works.
void SnipPicker::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	grabKeyboard();
}

void SnipPicker::hideEvent(QHideEvent *event)
{
	releaseKeyboard();
	QWidget::hideEvent(event);
}

void SnipPicker::SetMode(SnipSelection::Mode newMode)
{
	mode = newMode;
	regionButton->setChecked(mode == SnipSelection::Mode::Region);
	windowButton->setChecked(mode == SnipSelection::Mode::Window);
	fullScreenButton->setChecked(mode == SnipSelection::Mode::FullScreen);
	setCursor(mode == SnipSelection::Mode::Region ? Qt::CrossCursor : Qt::ArrowCursor);
	UpdateToolbar();
	update();
}

bool SnipPicker::CanRecord() const
{
	if (recordingBlocked) {
		return false;
	}
	if (mode == SnipSelection::Mode::Region) {
		return regionRect.width() >= kMinRegionSize && regionRect.height() >= kMinRegionSize;
	}
	if (mode == SnipSelection::Mode::Window) {
		return hasSelectedWindow;
	}
	return true; // Full Screen always has a target display
}

void SnipPicker::UpdateToolbar()
{
	const char *hintKey = "SnipRecord.Picker.Hint.Region";
	if (mode == SnipSelection::Mode::Window) {
		hintKey = "SnipRecord.Picker.Hint.Window";
	} else if (mode == SnipSelection::Mode::FullScreen) {
		hintKey = "SnipRecord.Picker.Hint.FullScreen";
	}

	hintLabel->setText(recordingBlocked ? obs_module_text("SnipRecord.Picker.AlreadyRecording")
					    : obs_module_text(hintKey));

	recordButton->setEnabled(CanRecord());
}

QRect SnipPicker::ActiveRect() const
{
	switch (mode) {
	case SnipSelection::Mode::Region:
		if (dragging) {
			const QRect drag = QRect(dragStart, dragCurrent).normalized();
			if (drag.width() >= kMinRegionSize && drag.height() >= kMinRegionSize) {
				return drag;
			}
		}
		return regionRect;
	case SnipSelection::Mode::Window:
		if (hasSelectedWindow) {
			return selectedWindowRect;
		}
		return hasHoverWindow ? hoverWindowRect : QRect();
	case SnipSelection::Mode::FullScreen:
		return fullScreenRect;
	}
	return {};
}

void SnipPicker::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.fillRect(rect(), QColor(0, 0, 0, 110));

	const QRect active = ActiveRect();
	if (!active.isValid() || active.isEmpty()) {
		return;
	}

	// `active` is in global logical pixels; this widget's origin is the
	// virtual desktop's top-left corner.
	const QPoint origin = geometry().topLeft();
	const QRect local = active.translated(-origin);

	painter.fillRect(local, QColor(90, 150, 255, 45));
	painter.setPen(QPen(QColor(59, 158, 255), 2));
	painter.drawRect(local.adjusted(1, 1, -1, -1));

	// Physical size is what the capture source will produce.
	QScreen *screen = QGuiApplication::screenAt(active.center());
	const double scale = screen ? screen->devicePixelRatio() : 1.0;
	const QString sizeText =
		QString("%1 × %2").arg(int(active.width() * scale + 0.5)).arg(int(active.height() * scale + 0.5));

	QFontMetrics metrics(painter.font());
	const QRect textRect = metrics.boundingRect(sizeText).adjusted(-6, -3, 6, 3);
	QRect labelRect(local.topLeft() - QPoint(0, textRect.height() + 4), textRect.size());
	if (labelRect.top() < 0) {
		labelRect.moveTop(local.top() + 4);
	}
	painter.fillRect(labelRect, QColor(28, 28, 30, 220));
	painter.setPen(Qt::white);
	painter.drawText(labelRect, Qt::AlignCenter, sizeText);
}

void SnipPicker::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		return;
	}
	const QPoint global = event->globalPosition().toPoint();

	if (mode == SnipSelection::Mode::Region) {
		dragging = true;
		dragStart = dragCurrent = global;
	} else if (mode == SnipSelection::Mode::Window) {
		SelectWindowAt(global);
	} else {
		if (QScreen *screen = QGuiApplication::screenAt(global)) {
			fullScreenRect = screen->geometry();
		}
	}
	UpdateToolbar();
	update();
}

void SnipPicker::mouseMoveEvent(QMouseEvent *event)
{
	const QPoint global = event->globalPosition().toPoint();
	if (mode == SnipSelection::Mode::Region) {
		if (dragging) {
			dragCurrent = global;
			update();
		}
	} else if (mode == SnipSelection::Mode::Window) {
		UpdateHoverWindow(global);
	} else {
		if (QScreen *screen = QGuiApplication::screenAt(global)) {
			if (fullScreenRect != screen->geometry()) {
				fullScreenRect = screen->geometry();
				update();
			}
		}
	}
}

void SnipPicker::mouseReleaseEvent(QMouseEvent *event)
{
	if (mode != SnipSelection::Mode::Region || event->button() != Qt::LeftButton || !dragging) {
		return;
	}
	dragging = false;
	dragCurrent = event->globalPosition().toPoint();
	regionRect = QRect(dragStart, dragCurrent).normalized();
	if (regionRect.width() < kMinRegionSize || regionRect.height() < kMinRegionSize) {
		regionRect = {};
	} else {
		lastRegion = regionRect;
	}
	UpdateToolbar();
	update();
}

void SnipPicker::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		return;
	}
	// Note: the second press of the double-click already started a
	// new drag, so check the stored rect itself here.
	if (mode == SnipSelection::Mode::Region && regionRect.width() >= kMinRegionSize &&
	    regionRect.height() >= kMinRegionSize && regionRect.contains(event->globalPosition().toPoint())) {
		StartRecording();
	} else if (mode != SnipSelection::Mode::Region) {
		StartRecording();
	}
}

void SnipPicker::keyPressEvent(QKeyEvent *event)
{
	switch (event->key()) {
	case Qt::Key_Escape:
		hide();
		break;
	case Qt::Key_1:
		SetMode(SnipSelection::Mode::Region);
		break;
	case Qt::Key_2:
		SetMode(SnipSelection::Mode::Window);
		break;
	case Qt::Key_3:
		SetMode(SnipSelection::Mode::FullScreen);
		break;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		StartRecording();
		break;
	default:
		QWidget::keyPressEvent(event);
		break;
	}
}

void SnipPicker::UpdateHoverWindow(const QPoint &globalPos)
{
#ifdef _WIN32
	WindowSearch search = {{globalPos.x(), globalPos.y()}, reinterpret_cast<HWND>(winId()), nullptr, {}};
	EnumWindows(FindWindowAtPoint, reinterpret_cast<LPARAM>(&search));
	if (search.found) {
		// Convert physical Win32 rect to Qt logical pixels via the
		// display it is on (fraction mapping, DPI-safe).
		const std::vector<MonitorEntry> monitors = AllMonitors();
		const MonitorEntry *monitor = MonitorAtPhysicalPoint(monitors, search.rect);
		const snip::Rect physical = {search.rect.left, search.rect.top, search.rect.right - search.rect.left,
					     search.rect.bottom - search.rect.top};
		QScreen *screen = QGuiApplication::screenAt(globalPos);
		if (monitor && screen) {
			const QRect geo = screen->geometry();
			const snip::Rect logical =
				snip::MapRect(physical,
					      ToSnipRect(QRect(monitor->rect.left, monitor->rect.top,
							       monitor->rect.right - monitor->rect.left,
							       monitor->rect.bottom - monitor->rect.top)),
					      ToSnipRect(geo));
			hoverWindowRect = QRect(logical.x, logical.y, logical.width, logical.height);
		} else {
			hoverWindowRect = QRect(physical.x, physical.y, physical.width, physical.height);
		}
		hasHoverWindow = true;
	} else {
		hasHoverWindow = false;
	}
	update();
#else
	(void)globalPos;
#endif
}

void SnipPicker::SelectWindowAt(const QPoint &globalPos)
{
#ifdef _WIN32
	WindowSearch search = {{globalPos.x(), globalPos.y()}, reinterpret_cast<HWND>(winId()), nullptr, {}};
	EnumWindows(FindWindowAtPoint, reinterpret_cast<LPARAM>(&search));
	if (!search.found) {
		return;
	}

	selectedWindowValue = WindowValueForHWND(search.found);
	selectedWindowPhysicalRect = QRect(search.rect.left, search.rect.top, search.rect.right - search.rect.left,
					   search.rect.bottom - search.rect.top);
	// Recompute the logical rect through the same path as hover.
	const std::vector<MonitorEntry> monitors = AllMonitors();
	const MonitorEntry *monitor = MonitorAtPhysicalPoint(monitors, search.rect);
	QScreen *screen = QGuiApplication::screenAt(globalPos);
	if (monitor && screen) {
		const QRect geo = screen->geometry();
		const snip::Rect logical =
			snip::MapRect(ToSnipRect(selectedWindowPhysicalRect),
				      {monitor->rect.left, monitor->rect.top, monitor->rect.right - monitor->rect.left,
				       monitor->rect.bottom - monitor->rect.top},
				      ToSnipRect(geo));
		selectedWindowRect = QRect(logical.x, logical.y, logical.width, logical.height);
	} else {
		selectedWindowRect = selectedWindowPhysicalRect;
	}
	hasSelectedWindow = true;
	hasHoverWindow = false;
#else
	(void)globalPos;
#endif
}

bool SnipPicker::BuildSelection(SnipSelection &selection) const
{
	selection = {};
	selection.mode = mode;

	if (mode == SnipSelection::Mode::Window) {
		if (!hasSelectedWindow) {
			return false;
		}
		selection.rect = ToSnipRect(selectedWindowPhysicalRect);
		selection.logicalRect = ToSnipRect(selectedWindowRect);
		selection.windowValue = selectedWindowValue.toUtf8().constData();
#ifdef _WIN32
		const std::vector<MonitorEntry> monitors = AllMonitors();
		const RECT rect = {selectedWindowPhysicalRect.left(), selectedWindowPhysicalRect.top(),
				   selectedWindowPhysicalRect.right(), selectedWindowPhysicalRect.bottom()};
		if (const MonitorEntry *monitor = MonitorAtPhysicalPoint(monitors, rect)) {
			selection.monitorRect = {monitor->rect.left, monitor->rect.top,
						 monitor->rect.right - monitor->rect.left,
						 monitor->rect.bottom - monitor->rect.top};
			selection.monitorId = monitor->deviceId;
		}
#endif
		return !selection.windowValue.empty();
	}

	const QRect logical = ActiveRect();
	if (!logical.isValid() || logical.isEmpty()) {
		return false;
	}
	selection.logicalRect = ToSnipRect(logical);

	QScreen *screen = QGuiApplication::screenAt(logical.center());
	if (!screen) {
		screen = QGuiApplication::primaryScreen();
	}
	if (!screen) {
		return false;
	}

#ifdef _WIN32
	const std::vector<MonitorEntry> monitors = AllMonitors();
	const MonitorEntry *monitor = MonitorForScreen(monitors, screen);
	if (!monitor) {
		return false;
	}

	const snip::Rect monitorPhysical = {monitor->rect.left, monitor->rect.top,
					    monitor->rect.right - monitor->rect.left,
					    monitor->rect.bottom - monitor->rect.top};
	selection.monitorRect = monitorPhysical;
	selection.monitorId = monitor->deviceId;
	selection.rect = snip::Intersect(
		snip::MapRect(ToSnipRect(logical), ToSnipRect(screen->geometry()), monitorPhysical), monitorPhysical);
#else
	selection.rect = ToSnipRect(logical);
	selection.monitorRect = selection.rect;
#endif
	return snip::IsUsable(selection.rect, kMinRegionSize, kMinRegionSize);
}

void SnipPicker::StartRecording()
{
	if (!CanRecord()) {
		return;
	}

	SnipSelection selection;
	if (!BuildSelection(selection)) {
		return;
	}

	hide();
	// Give the overlay a frame to disappear before capture starts, so
	// it is not visible in the first recorded frames.
	QTimer::singleShot(150, this, [this, selection] {
		if (recorder->StartRecording(selection) && selection.mode == SnipSelection::Mode::Region) {
			outline->ShowFor(QRect(selection.logicalRect.x, selection.logicalRect.y,
					       selection.logicalRect.width, selection.logicalRect.height));
		}
	});
}
