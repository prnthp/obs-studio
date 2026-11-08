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

#include "snip-picker.hpp"
#include "snip-recorder.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("snip-record", "en-US")

namespace {

SnipRecorder recorder;
SnipPicker *picker = nullptr;
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
		return;
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
		LoadHotkeyBinding();
		// Undo a canvas resize left behind by a crash mid-snip.
		recorder.RestoreCanvasIfPending();
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

	QAction *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("SnipRecord.Menu.Open")));
	obs_frontend_pop_ui_translation();

	QObject::connect(action, &QAction::triggered, action, [] { TogglePickerOrStop(); });

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
	delete picker;
	picker = nullptr;
}
