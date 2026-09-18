// Read-only enumeration of the actual Shell menu; never invokes a command.
#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <string>
static bool list(HMENU menu) {
    bool found = false;
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        wchar_t text[512]{};
        GetMenuStringW(menu, i, text, 512, MF_BYPOSITION);
        std::wstring label(text);
        if (label.find(L"Kindle") != label.npos) { std::wcout << label << L'\n'; found = true; }
        if (auto sub = GetSubMenu(menu, i)) found = list(sub) || found;
    }
    return found;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE absolute = nullptr;
    HRESULT hr = SHParseDisplayName(argv[1], nullptr, &absolute, 0, nullptr);
    IShellFolder* parent = nullptr; PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(hr)) hr = SHBindToParent(absolute, IID_PPV_ARGS(&parent), &child);
    IContextMenu* context = nullptr;
    if (SUCCEEDED(hr)) hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&context));
    HMENU menu = CreatePopupMenu();
    if (SUCCEEDED(hr)) hr = context->QueryContextMenu(menu, 0, 1, 0x7fff, CMF_NORMAL);
    bool found = SUCCEEDED(hr) && list(menu);
    if (!found) std::cout << "Send to Kindle absent from Shell menu\n";
    DestroyMenu(menu); if (context) context->Release(); if (parent) parent->Release();
    CoTaskMemFree(absolute); CoUninitialize(); return found ? 0 : 1;
}
