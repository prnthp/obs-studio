#include "snip-launcher.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <vector>

#include "snip-hotkey.hpp"

namespace {

const char *kBasicWindow = "BasicWindow";
const char *kTrayStopKey = "TrayClickStopsRecording";
const char *kStartupKey = "SnipLauncherOnStartup";

bool GlobalBool(const char *key, bool fallback)
{
	config_t *config = obs_frontend_get_user_config();
	if (!config || !config_has_user_value(config, kBasicWindow, key)) {
		return fallback;
	}
	return config_get_bool(config, kBasicWindow, key);
}

void SetGlobalBool(const char *key, bool value)
{
	config_t *config = obs_frontend_get_user_config();
	if (!config) {
		return;
	}
	config_set_bool(config, kBasicWindow, key, value);
	config_save(config);
}

QString RecordingFolder()
{
	config_t *config = obs_frontend_get_profile_config();
	QString folder;
	if (config) {
		const char *mode = config_get_string(config, "Output", "Mode");
		const bool advanced = mode && std::string(mode) == "Advanced";
		const char *path = advanced ? config_get_string(config, "AdvOut", "RecFilePath")
					    : config_get_string(config, "SimpleOutput", "FilePath");
		if (path && path[0]) {
			folder = QString::fromUtf8(path);
		}
	}
	if (folder.isEmpty()) {
		char *last = obs_frontend_get_last_recording();
		if (last) {
			folder = QFileInfo(QString::fromUtf8(last)).absolutePath();
			bfree(last);
		}
	}
	if (folder.isEmpty()) {
		folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	}
	return QDir::cleanPath(folder);
}

// Maps a Qt key token ("S", "F5", "Space", ...) to the libobs name
// used after OBS_KEY_. Returns empty for keys we cannot name.
std::string ObsKeyName(const QString &token)
{
	static const std::vector<std::pair<QString, const char *>> kNamed = {
		{"Space", "SPACE"}, {"Esc", "ESCAPE"},       {"Tab", "TAB"},       {"Return", "RETURN"},
		{"Enter", "RETURN"}, {"Backspace", "BACKSPACE"}, {"Del", "DELETE"}, {"Left", "LEFT"},
		{"Right", "RIGHT"}, {"Up", "UP"},             {"Down", "DOWN"},
	};
	for (const auto &[qtName, obsName] : kNamed) {
		if (token == qtName) {
			return obsName;
		}
	}
	if (token.size() == 1) {
		return token.toUpper().toStdString();
	}
	if (token.startsWith("F") && token.size() <= 3) {
		return token.toStdString(); // F1..F24 already match
	}
	return {};
}

} // namespace

SnipLauncher::SnipLauncher(std::function<void()> onStartSnip_,
			   std::function<void(const std::string &)> applyHotkeyJson_,
			   std::function<std::string()> shortcutDisplay_, QWidget *parent)
	: QWidget(parent),
	  onStartSnip(std::move(onStartSnip_)),
	  applyHotkeyJson(std::move(applyHotkeyJson_)),
	  shortcutDisplay(std::move(shortcutDisplay_))
{
	setWindowTitle(obs_module_text("SnipRecord.Launcher.Title"));
	setWindowFlags(Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint);
	setMinimumWidth(340);

	auto *title = new QLabel(obs_module_text("SnipRecord.Launcher.Title"), this);
	title->setStyleSheet("font-size: 20px; font-weight: bold;");
	auto *subtitle = new QLabel(obs_module_text("SnipRecord.Launcher.Subtitle"), this);
	subtitle->setStyleSheet("color: #888;");

	startButton = new QPushButton(obs_module_text("SnipRecord.Launcher.StartSnip"), this);
	startButton->setMinimumHeight(46);
	startButton->setStyleSheet(
		"QPushButton { background: #c0392b; color: white; font-size: 15px; font-weight: bold; "
		"border: none; border-radius: 6px; padding: 8px; }"
		"QPushButton:hover { background: #d44333; }");

	auto *folderButton = new QPushButton(obs_module_text("SnipRecord.Launcher.OpenFolder"), this);
	folderButton->setMinimumHeight(34);
	auto *fullUiButton = new QPushButton(obs_module_text("SnipRecord.Launcher.ShowFullUi"), this);
	fullUiButton->setMinimumHeight(34);

	statusLabel = new QLabel(this);
	statusLabel->setStyleSheet("color: #888;");
	exportLabel = new QLabel(this);
	exportLabel->setStyleSheet("color: #6aa84f;");
	exportLabel->hide();

	auto *shortcutLabel = new QLabel(obs_module_text("SnipRecord.Launcher.Shortcut"), this);
	shortcutEdit = new QKeySequenceEdit(this);
	// Click-to-edit only: with the default focus policy this field
	// takes keyboard focus as soon as the window is activated, and
	// every key the user then presses rewrites the shortcut.
	shortcutEdit->setFocusPolicy(Qt::ClickFocus);
	shortcutEdit->setKeySequence(QKeySequence(QString::fromStdString(shortcutDisplay())));
	shortcutHint = new QLabel(this);
	shortcutHint->setStyleSheet("color: #a33;");
	shortcutHint->hide();

	trayStopCheck = new QCheckBox(obs_module_text("SnipRecord.Launcher.TrayStop"), this);
	trayStopCheck->setChecked(GlobalBool(kTrayStopKey, true));
	startupCheck = new QCheckBox(obs_module_text("SnipRecord.Launcher.ShowAtStartup"), this);
	startupCheck->setChecked(GlobalBool(kStartupKey, true));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(18, 16, 18, 16);
	layout->setSpacing(9);
	layout->addWidget(title);
	layout->addWidget(subtitle);
	layout->addSpacing(4);
	layout->addWidget(startButton);
	layout->addWidget(folderButton);
	layout->addWidget(fullUiButton);
	layout->addWidget(statusLabel);
	layout->addWidget(exportLabel);
	layout->addSpacing(8);
	layout->addWidget(shortcutLabel);
	layout->addWidget(shortcutEdit);
	layout->addWidget(shortcutHint);
	layout->addWidget(trayStopCheck);
	layout->addWidget(startupCheck);

	connect(startButton, &QPushButton::clicked, this, [this] {
		if (onStartSnip) {
			onStartSnip();
		}
		RefreshState();
	});
	connect(folderButton, &QPushButton::clicked, this, &SnipLauncher::OpenRecordingFolder);
	connect(fullUiButton, &QPushButton::clicked, this, &SnipLauncher::ShowFullUi);
	connect(shortcutEdit, &QKeySequenceEdit::editingFinished, this, &SnipLauncher::ApplyShortcutEdit);
	connect(trayStopCheck, &QCheckBox::toggled, this, [](bool checked) { SetGlobalBool(kTrayStopKey, checked); });
	connect(startupCheck, &QCheckBox::toggled, this, [](bool checked) { SetGlobalBool(kStartupKey, checked); });

	refreshTimer = new QTimer(this);
	refreshTimer->setInterval(700);
	connect(refreshTimer, &QTimer::timeout, this, &SnipLauncher::RefreshState);
	refreshTimer->start();
	RefreshState();
}

void SnipLauncher::SetExportStatus(const QString &text)
{
	exportLabel->setText(text);
	exportLabel->setVisible(!text.isEmpty());
}

void SnipLauncher::Show()
{
	RefreshState();
	show();
	raise();
	activateWindow();
	// Keep key presses aimed at the window, not the shortcut field.
	startButton->setFocus(Qt::OtherFocusReason);
}

bool SnipLauncher::ShowAtStartup() const
{
	return GlobalBool(kStartupKey, true);
}

void SnipLauncher::closeEvent(QCloseEvent *event)
{
	// Never leave the user with no window at all: closing the
	// launcher brings up the full OBS interface.
	ShowFullUi();
	QWidget::closeEvent(event);
}

void SnipLauncher::RefreshState()
{
	const bool recording = obs_frontend_recording_active();
	startButton->setText(obs_module_text(recording ? "SnipRecord.Launcher.StopRecording"
						       : "SnipRecord.Launcher.StartSnip"));
	statusLabel->setText(obs_module_text(recording ? "SnipRecord.Launcher.StatusRecording"
						       : "SnipRecord.Launcher.StatusIdle"));
	if (shortcutEdit && !shortcutEdit->hasFocus() && shortcutDisplay) {
		shortcutEdit->setKeySequence(QKeySequence(QString::fromStdString(shortcutDisplay())));
	}
}

void SnipLauncher::ApplyShortcutEdit()
{
	shortcutEdit->clearFocus();
	const QKeySequence sequence = shortcutEdit->keySequence();
	shortcutHint->hide();
	if (sequence.isEmpty()) {
		applyHotkeyJson("{\"bindings\":[]}");
		return;
	}

	const QStringList parts = sequence.toString().split('+');
	const std::string keyName = ObsKeyName(parts.last());
	if (keyName.empty() || obs_key_from_name(("OBS_KEY_" + keyName).c_str()) == OBS_KEY_NONE) {
		shortcutHint->setText(obs_module_text("SnipRecord.Launcher.ShortcutUnsupported"));
		shortcutHint->show();
		shortcutEdit->setKeySequence(QKeySequence(QString::fromStdString(shortcutDisplay())));
		return;
	}

	const QKeyCombination combination = sequence[0];
	const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
	applyHotkeyJson(snip::BindingJson(bool(modifiers & Qt::ShiftModifier), bool(modifiers & Qt::ControlModifier),
					  bool(modifiers & Qt::AltModifier), bool(modifiers & Qt::MetaModifier),
					  keyName));
	blog(LOG_INFO, "[snip-record] Snip shortcut changed to %s", sequence.toString().toUtf8().constData());
}

void SnipLauncher::ShowFullUi()
{
	QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (mainWindow) {
		mainWindow->show();
		mainWindow->raise();
		mainWindow->activateWindow();
	}
	hide();
}

void SnipLauncher::OpenRecordingFolder()
{
	const QString folder = RecordingFolder();
	if (!folder.isEmpty()) {
		QDir().mkpath(folder);
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
	}
}
