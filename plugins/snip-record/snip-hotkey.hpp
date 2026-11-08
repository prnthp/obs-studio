#pragma once

#include <string>

namespace snip {

// Builds one OBS hotkey binding in the JSON shape OBS itself writes to
// the profile config (Hotkeys section) and accepts via obs_hotkey_load.
// `obsKeyName` is a libobs key name without the OBS_KEY_ prefix
// (e.g. "S", "F5", "SPACE"). Only set modifiers are emitted.
inline std::string BindingJson(bool shift, bool control, bool alt, bool meta, const std::string &obsKeyName)
{
	std::string json = "{\"bindings\":[{";
	bool first = true;
	auto addModifier = [&](bool set, const char *name) {
		if (!set) {
			return;
		}
		if (!first) {
			json += ",";
		}
		first = false;
		json += "\"";
		json += name;
		json += "\":true";
	};
	addModifier(shift, "shift");
	addModifier(control, "control");
	addModifier(alt, "alt");
	addModifier(meta, "meta");
	if (!first) {
		json += ",";
	}
	json += "\"key\":\"OBS_KEY_" + obsKeyName + "\"}]}";
	return json;
}

// Factory default for the snip toggle hotkey: Alt+Shift+S.
inline const char *DefaultBindingJson()
{
	return "{\"bindings\":[{\"shift\":true,\"alt\":true,\"key\":\"OBS_KEY_S\"}]}";
}

} // namespace snip
