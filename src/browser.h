#pragma once
#include "core.h"
#include <vector>
namespace books { std::vector<fs::path> browseBooks(HWND owner,const fs::path& initialFolder={},bool previewOnly=false); }
