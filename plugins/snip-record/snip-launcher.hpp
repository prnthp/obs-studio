#pragma once

#include <QString>
#include <QWidget>

#include <functional>
#include <string>

class QCheckBox;
class QLabel;
class QPushButton;
class QKeySequenceEdit;
class QTimer;

// The simple "OBS for Dummies" home window shown at launch instead of
// the full OBS interface: start a snip, open the recordings folder, or
// switch to the full UI (which stays one click away). Also hosts the
// shortcut field and the tray-click option.
class SnipLauncher : public QWidget {
	Q_OBJECT

public:
	// onStartSnip toggles the picker / stops a running snip (the
	// module's hotkey action). applyHotkeyJson replaces the toggle
	// hotkey binding (OBS profile-config JSON). shortcutDisplay
	// returns the current binding as e.g. "Alt+Shift+S".
	SnipLauncher(std::function<void()> onStartSnip, std::function<void(const std::string &)> applyHotkeyJson,
		     std::function<std::string()> shortcutDisplay, QWidget *parent = nullptr);

	void Show();
	bool ShowAtStartup() const;
	// Progress/result line for the background GIF/WebP export.
	void SetExportStatus(const QString &text);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void RefreshState();
	void ApplyShortcutEdit();
	void ShowFullUi();
	void OpenRecordingFolder();

	std::function<void()> onStartSnip;
	std::function<void(const std::string &)> applyHotkeyJson;
	std::function<std::string()> shortcutDisplay;

	QPushButton *startButton = nullptr;
	QLabel *statusLabel = nullptr;
	QLabel *exportLabel = nullptr;
	QKeySequenceEdit *shortcutEdit = nullptr;
	QLabel *shortcutHint = nullptr;
	QCheckBox *trayStopCheck = nullptr;
	QCheckBox *startupCheck = nullptr;
	QTimer *refreshTimer = nullptr;
};
