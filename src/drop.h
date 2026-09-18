#pragma once
#include "core.h"
#include <shellapi.h>
#include <vector>
namespace books {
std::vector<fs::path> droppedBooks(HDROP drop, bool release = true);
}
