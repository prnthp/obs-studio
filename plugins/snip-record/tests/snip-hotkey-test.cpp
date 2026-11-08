// Standalone checks for the hotkey binding JSON builder. Compile with:
//   cl /std:c++17 /EHsc snip-hotkey-test.cpp
#include <cstdio>
#include <cstring>

#include "../snip-hotkey.hpp"

static int failures = 0;

static void Expect(const char *actual, const char *expected, const char *label)
{
	if (std::strcmp(actual, expected) != 0) {
		std::printf("FAIL %s\n  actual:   %s\n  expected: %s\n", label, actual, expected);
		failures++;
	}
}

int main()
{
	// The factory default must be exactly Alt+Shift+S.
	Expect(snip::DefaultBindingJson(), "{\"bindings\":[{\"shift\":true,\"alt\":true,\"key\":\"OBS_KEY_S\"}]}",
	       "default");

	// Builder with the same modifiers must reproduce the default.
	Expect(snip::BindingJson(true, false, true, false, "S").c_str(), snip::DefaultBindingJson(), "builder-default");

	// No modifiers: just the key.
	Expect(snip::BindingJson(false, false, false, false, "F5").c_str(), "{\"bindings\":[{\"key\":\"OBS_KEY_F5\"}]}",
	       "no-modifiers");

	// All modifiers, in OBS's field order.
	Expect(snip::BindingJson(true, true, true, true, "SPACE").c_str(),
	       "{\"bindings\":[{\"shift\":true,\"control\":true,\"alt\":true,\"meta\":true,\"key\":\"OBS_KEY_SPACE\"}]}",
	       "all-modifiers");

	if (failures == 0) {
		std::printf("ALL HOTKEY TESTS PASSED\n");
		return 0;
	}
	std::printf("%d HOTKEY TEST(S) FAILED\n", failures);
	return 1;
}
