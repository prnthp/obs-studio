#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QCoreApplication>
#include <QMainWindow>
#include <QObject>
#include <QTimer>

#include "snip-picker.hpp"
#include "snip-recorder.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("snip-record", "en-US")

namespace {

SnipRecorder recorder;
SnipPicker *picker = nullptr;
obs_hotkey_id toggleHotkey = OBS_INVALID_HOTKEY_ID;

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

void FrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		recorder.HandleRecordingStopped();
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

	toggleHotkey = obs_hotkey_register_frontend("snip_record.toggle", obs_module_text("SnipRecord.Hotkey.Toggle"),
						    HotkeyPressed, nullptr);
	obs_frontend_add_event_callback(FrontendEvent, nullptr);

	blog(LOG_INFO, "[snip-record] Loaded. Bind '%s' under Settings > Hotkeys, or use Tools > %s.",
	     obs_module_text("SnipRecord.Hotkey.Toggle"), obs_module_text("SnipRecord.Menu.Open"));
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(FrontendEvent, nullptr);
	if (toggleHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(toggleHotkey);
	}
	delete picker;
	picker = nullptr;
}
