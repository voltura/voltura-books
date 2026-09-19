#pragma once
#include "core.h"
#include <vector>
namespace books {
inline constexpr wchar_t ShellSelectionId[]=L"{F0D73B4A-36AE-40A5-A224-CFD826524030}";
inline constexpr wchar_t ShellSelectionKey[]=L"Software\\Classes\\CLSID\\{F0D73B4A-36AE-40A5-A224-CFD826524030}";
std::vector<fs::path> receiveShellSelection(const wchar_t* classId=ShellSelectionId,HANDLE readyEvent=nullptr);
}
