#pragma once
#include <windows.h>
#include <filesystem>
namespace books {
// Caller owns the returned icon.
HICON loadFileTypeIcon(const std::filesystem::path& path);
}
