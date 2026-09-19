#pragma once
#include <windows.h>
namespace books {
using AboutSettingsAction = bool (*)(HWND);
bool showAbout(HWND owner, bool checkForUpdates=false, AboutSettingsAction settingsAction=nullptr);
}
