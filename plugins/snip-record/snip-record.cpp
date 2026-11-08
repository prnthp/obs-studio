#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <util/config-file.h>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMainWindow>
#include <QObject>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#endif

#include "snip-hotkey.hpp"
#include "snip-launcher.hpp"
#include "snip-picker.hpp"
#include "snip-recorder.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("snip-record", "en-US")

namespace {

SnipRecorder recorder;
SnipPicker *picker = nullptr;
SnipLauncher *launcher = nullptr;
obs_hotkey_id toggleHotkey = OBS_INVALID_HOTKEY_ID;

const char *kHotkeyName = "snip_record.toggle";

// The Settings dialog saves frontend hotkey bindings to the profile
// config ("Hotkeys" section), but OBS only loads back its own built-in
// hotkeys at startup. Load ours explicitly, or the binding set in
// Settings silently stops working after a restart.
void LoadHotkeyBinding()
{
	if (toggleHotkey == OBS_INVALID_HOTKEY_ID) {
		return;
	}

	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		return;
	}

	const char *json = config_get_string(config, "Hotkeys", kHotkeyName);
	if (!json || !json[0]) {
		// Fresh install: default to Alt+Shift+S so the snip flow
		// works out of the box. Persisted like any user binding,
		// so Settings > Hotkeys shows (and can change) it too.
		config_set_string(config, "Hotkeys", kHotkeyName, snip::DefaultBindingJson());
		config_save(config);
		json = config_get_string(config, "Hotkeys", kHotkeyName);
		blog(LOG_INFO, "[snip-record] No saved shortcut; defaulting to Alt+Shift+S");
		if (!json || !json[0]) {
			return;
		}
	}

	OBSDataAutoRelease data = obs_data_create_from_json(json);
	if (!data) {
		return;
	}

	OBSDataArrayAutoRelease bindings = obs_data_get_array(data, "bindings");
	obs_hotkey_load(toggleHotkey, bindings);
	blog(LOG_INFO, "[snip-record] Loaded %zu hotkey binding(s) from profile",
	     bindings ? size_t(obs_data_array_count(bindings)) : size_t(0));
}

// First binding of the toggle hotkey as display text ("Alt+Shift+S").
std::string CurrentShortcutDisplay()
{
	config_t *config = obs_frontend_get_profile_config();
	const char *json = config ? config_get_string(config, "Hotkeys", kHotkeyName) : nullptr;
	if (!json || !json[0]) {
		return "Alt+Shift+S";
	}
	OBSDataAutoRelease data = obs_data_create_from_json(json);
	OBSDataArrayAutoRelease bindings = data ? obs_data_get_array(data, "bindings") : nullptr;
	if (!bindings || obs_data_array_count(bindings) == 0) {
		return {};
	}
	OBSDataAutoRelease binding = obs_data_array_item(bindings, 0);
	QStringList parts;
	if (obs_data_get_bool(binding, "control")) {
		parts << "Ctrl";
	}
	if (obs_data_get_bool(binding, "alt")) {
		parts << "Alt";
	}
	if (obs_data_get_bool(binding, "shift")) {
		parts << "Shift";
	}
	if (obs_data_get_bool(binding, "meta")) {
		parts << "Meta";
	}
	QString key = QString::fromUtf8(obs_data_get_string(binding, "key"));
	key.remove("OBS_KEY_");
	if (key == "SPACE") {
		key = "Space";
	} else if (key.size() > 1) {
		key = key.left(1) + key.mid(1).toLower();
	}
	parts << key;
	return parts.join('+').toStdString();
}

void ApplyHotkeyJson(const std::string &json)
{
	if (toggleHotkey == OBS_INVALID_HOTKEY_ID) {
		return;
	}
	config_t *config = obs_frontend_get_profile_config();
	if (config) {
		config_set_string(config, "Hotkeys", kHotkeyName, json.c_str());
		config_save(config);
	}
	OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
	OBSDataArrayAutoRelease bindings = data ? obs_data_get_array(data, "bindings") : nullptr;
	obs_hotkey_load(toggleHotkey, bindings);
}

// Installation defaults for the simple recorder: live in the tray,
// record to MP4 in the Videos folder. Only fills values the user has
// never set, so later changes in OBS Settings always win.
void SetGlobalDefaultBool(const char *key, bool value)
{
	config_t *config = obs_frontend_get_user_config();
	if (config && !config_has_user_value(config, "BasicWindow", key)) {
		config_set_bool(config, "BasicWindow", key, value);
		config_save(config);
	}
}

void ApplyInstallDefaults()
{
	// Determine "fresh install" once, before anything below sets
	// FirstRun: true only on the very first launch ever.
	static bool freshChecked = false;
	static bool freshInstall = false;
	if (!freshChecked) {
		freshChecked = true;
		if (config_t *user = obs_frontend_get_user_config()) {
			freshInstall = !config_get_bool(user, "General", "FirstRun");
		}
	}

	SetGlobalDefaultBool("SysTrayEnabled", true);
	SetGlobalDefaultBool("SysTrayWhenStarted", true);
	SetGlobalDefaultBool("SysTrayMinimizeToTray", true);
	SetGlobalDefaultBool("TrayClickStopsRecording", true);

	// Skip OBS's first-run Auto-Config Wizard: this recorder never
	// streams, so its questions (and bandwidth test) only get in the
	// way. OBSBasic queues the wizard when FirstRun is unset and the
	// app config has no LastVersion; module load happens before the
	// main window reads it, so setting FirstRun here wins.
	if (config_t *user = obs_frontend_get_user_config()) {
		if (!config_get_bool(user, "General", "FirstRun")) {
			config_set_bool(user, "General", "FirstRun", true);
			config_save(user);
		}
	}

	config_t *profile = obs_frontend_get_profile_config();
	if (!profile) {
		return;
	}
	bool changed = false;
	if (!config_has_user_value(profile, "SimpleOutput", "RecFormat2")) {
		config_set_string(profile, "SimpleOutput", "RecFormat2", "mp4");
		changed = true;
	}
	// Recording-oriented defaults (what the wizard's "recording"
	// path would pick): high quality, 60 fps instead of streaming
	// bitrates at 30. OBS pre-creates Stream/30 profile defaults
	// before modules load, so "only if unset" never fires; force
	// them, but only on a genuinely fresh install - once FirstRun
	// is set we never touch these again, so deliberate user changes
	// in Settings are permanent.
	if (freshInstall) {
		config_set_string(profile, "SimpleOutput", "RecQuality", "HQ");
		config_set_uint(profile, "Video", "FPSType", 1); // integer FPS
		config_set_uint(profile, "Video", "FPSInt", 60);
		changed = true;
	}
	if (!config_has_user_value(profile, "SimpleOutput", "FilePath")) {
		const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
		if (!movies.isEmpty()) {
			config_set_string(profile, "SimpleOutput", "FilePath",
					  QDir::toNativeSeparators(movies).toUtf8().constData());
			changed = true;
		}
	}
	if (changed) {
		config_save(profile);
	}
}

// The hotkey fires the picker when idle, and stops the recording when a
// snip recording is running -- one shortcut for the whole flow.
void TogglePickerOrStop()
{
	if (!picker) {
		return;
	}

	if (picker->isVisible()) {
		picker->hide();
	} else if (recorder.IsActive() && obs_frontend_recording_active()) {
		blog(LOG_INFO, "[snip-record] Stopping recording via hotkey/menu");
		obs_frontend_recording_stop();
	} else {
		recorder.CancelPendingStart();
		recorder.RestoreCanvasIfPending();
		picker->OpenPicker();
	}
}

void HotkeyPressed(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed) {
		return;
	}
	// Marshal to the UI thread; the picker is a QWidget.
	QTimer::singleShot(0, QCoreApplication::instance(), [] { TogglePickerOrStop(); });
}

#ifdef _WIN32

std::vector<HWND> SnapshotTopWindows()
{
	std::vector<HWND> windows;
	EnumWindows(
		[](HWND hwnd, LPARAM param) {
			if (IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0) {
				reinterpret_cast<std::vector<HWND> *>(param)->push_back(hwnd);
			}
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&windows));
	return windows;
}

// Windows only lets the foreground process hand over the foreground,
// and OBS is usually in the background when a snip ends via hotkey.
// Attaching to the current foreground thread lifts that restriction
// for this one deliberate handoff.
void ForceForegroundWindow(HWND hwnd)
{
	const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
	const DWORD currentThread = GetCurrentThreadId();
	AttachThreadInput(foregroundThread, currentThread, TRUE);
	ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	AttachThreadInput(foregroundThread, currentThread, FALSE);
}

// Poll briefly for a top-level window that did not exist before the
// action (the player window, or the Explorer folder window) and pull
// it to the foreground.
void WatchForNewWindow(std::vector<HWND> before, int attemptsLeft)
{
	if (attemptsLeft <= 0) {
		return;
	}
	QTimer::singleShot(300, QCoreApplication::instance(), [before, attemptsLeft] {
		for (HWND hwnd : SnapshotTopWindows()) {
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == GetCurrentProcessId()) {
				continue;
			}
			if (std::find(before.begin(), before.end(), hwnd) == before.end()) {
				ForceForegroundWindow(hwnd);
				return;
			}
		}
		WatchForNewWindow(before, attemptsLeft - 1);
	});
}

#endif // _WIN32

// Runs the picker's "After recording" choice for a finished snip.
void DoPostRecordingAction()
{
	const int action = picker ? picker->PostAction() : 0;
	if (action == 0) {
		return;
	}

	char *rawPath = obs_frontend_get_last_recording();
	if (!rawPath) {
		return;
	}
	const QString path = QString::fromUtf8(rawPath);
	bfree(rawPath);

	if (path.isEmpty() || !QFileInfo::exists(path)) {
		return;
	}

	blog(LOG_INFO, "[snip-record] Post-recording action %d for '%s'", action, path.toUtf8().constData());
#ifdef _WIN32
	const std::vector<HWND> before = SnapshotTopWindows();
	const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
	if (action == 1) {
		ShellExecuteW(nullptr, L"open", nativePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	} else if (action == 2) {
		// Exact command line, no Qt argument re-quoting: a mangled
		// /select argument makes Explorer open Documents instead.
		std::wstring commandLine = L"explorer.exe /select,\"" + nativePath + L"\"";
		STARTUPINFOW startupInfo = {};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo = {};
		if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
				   &startupInfo, &processInfo)) {
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
		}
	}
	WatchForNewWindow(before, 16);
#endif
}

void FrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		// Only now is recording really running (window mode waits
		// for the capture to hook first), so only now show the
		// outline.
		if (recorder.IsActive() && picker) {
			picker->ShowOutlineForLastSelection();
		}
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		if (picker) {
			picker->HideOutline();
		}
		const bool wasSnipRecording = recorder.IsActive();
		recorder.HandleRecordingStopped();
		if (wasSnipRecording) {
			// Brief delay so the output file is fully closed.
			QTimer::singleShot(500, QCoreApplication::instance(), [] { DoPostRecordingAction(); });
		}
	} else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		ApplyInstallDefaults();
		LoadHotkeyBinding();
		// Undo a canvas resize left behind by a crash mid-snip.
		recorder.RestoreCanvasIfPending();
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING && launcher && launcher->ShowAtStartup()) {
			// Simple mode: the launcher replaces the main
			// window until the user asks for the full UI.
			if (QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window())) {
				mainWindow->hide();
			}
			launcher->Show();
		}
	}
}

void SaveSettings(obs_data_t *save_data, bool saving, void *)
{
	if (saving) {
		OBSDataAutoRelease obj = obs_data_create();
		obs_data_set_bool(obj, "matchCanvas", recorder.MatchCanvas());
		obs_data_set_bool(obj, "matchCanvasUserSet", recorder.MatchCanvasUserSet());
		obs_data_set_bool(obj, "instantStart", picker ? picker->InstantStart() : false);
		obs_data_set_int(obj, "postAction", picker ? picker->PostAction() : 0);
		obs_data_set_bool(obj, "desktopAudio", picker ? picker->DesktopAudio() : true);
		obs_data_set_bool(obj, "micAudio", picker ? picker->MicAudio() : true);
		obs_data_set_bool(obj, "captureCursor", picker ? picker->CaptureCursor() : true);
		obs_data_set_obj(save_data, "snip-record", obj);
	} else {
		OBSDataAutoRelease obj = obs_data_get_obj(save_data, "snip-record");
		if (picker) {
			// On by default. Settings saved by older versions
			// contain matchCanvas=false that was written
			// automatically, not chosen, so only honor an off
			// value the user deliberately set (UserSet marker).
			const bool userSet = obj && obs_data_has_user_value(obj, "matchCanvasUserSet") &&
					     obs_data_get_bool(obj, "matchCanvasUserSet");
			const bool checked = userSet ? obs_data_get_bool(obj, "matchCanvas") : true;
			picker->SetMatchCanvasChecked(checked, userSet);
			if (obj) {
				picker->SetInstantStartChecked(obs_data_get_bool(obj, "instantStart"));
				picker->SetPostAction(int(obs_data_get_int(obj, "postAction")));
				// New in this version; absent keys mean the
				// defaults (everything recorded).
				auto savedOr = [&obj](const char *key, bool fallback) {
					return obs_data_has_user_value(obj, key) ? obs_data_get_bool(obj, key)
										 : fallback;
				};
				picker->SetCaptureOptions(savedOr("desktopAudio", true), savedOr("micAudio", true),
							  savedOr("captureCursor", true));
			}
		}
	}
}

} // namespace

bool obs_module_load(void)
{
	obs_frontend_push_ui_translation(obs_module_get_string);
	QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	picker = new SnipPicker(&recorder, mainWindow);
	launcher = new SnipLauncher([] { TogglePickerOrStop(); }, ApplyHotkeyJson, CurrentShortcutDisplay, mainWindow);

	// Set installation defaults before the frontend reads the tray
	// options while creating the main window.
	ApplyInstallDefaults();

	QAction *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("SnipRecord.Menu.Open")));
	QAction *dummiesAction =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("SnipRecord.Menu.Dummies")));
	obs_frontend_pop_ui_translation();

	QObject::connect(action, &QAction::triggered, action, [] { TogglePickerOrStop(); });
	QObject::connect(dummiesAction, &QAction::triggered, dummiesAction, [] {
		// Back to simple mode: show the launcher, tuck the
		// full UI away again.
		if (QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window())) {
			mainWindow->hide();
		}
		if (launcher) {
			launcher->Show();
		}
	});

	toggleHotkey = obs_hotkey_register_frontend(kHotkeyName, obs_module_text("SnipRecord.Hotkey.Toggle"),
						    HotkeyPressed, nullptr);
	obs_frontend_add_event_callback(FrontendEvent, nullptr);
	obs_frontend_add_save_callback(SaveSettings, nullptr);

	blog(LOG_INFO, "[snip-record] Loaded. Bind '%s' under Settings > Hotkeys, or use Tools > %s.",
	     obs_module_text("SnipRecord.Hotkey.Toggle"), obs_module_text("SnipRecord.Menu.Open"));
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(SaveSettings, nullptr);
	obs_frontend_remove_event_callback(FrontendEvent, nullptr);
	if (toggleHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(toggleHotkey);
	}
	delete launcher;
	launcher = nullptr;
	delete picker;
	picker = nullptr;
}
