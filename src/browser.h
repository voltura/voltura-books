#pragma once
#include "core.h"
#include <vector>
namespace books {
struct BrowserMenuActions {
    bool (*settings)(HWND)=nullptr;
    bool (*about)(HWND,bool)=nullptr;
};
std::vector<fs::path> browseBooks(HWND owner,const fs::path& initialFolder={},bool previewOnly=false,bool rememberInitialFolder=false,BrowserMenuActions menuActions={});
bool navigateBrowseBooks(const fs::path& folder);
}
