#include "core.h"
#include "formats.h"
#include "shell_selection.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

namespace books {
static constexpr wchar_t VerbKey[] = L"Software\\Classes\\*\\shell\\VolturaBooks.Send";
static constexpr wchar_t BrowseVerbKey[] = L"Software\\Classes\\Directory\\shell\\VolturaBooks.Browse";
static constexpr wchar_t LegacyVerbKey[] = L"Software\\Classes\\SystemFileAssociations\\.epub\\shell\\VolturaBooks.Send";
static constexpr wchar_t UninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VolturaBooks";
fs::path executablePath() {
    std::wstring path(32768, 0);
    DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) throw std::runtime_error("Could not locate the application.");
    path.resize(count); return path;
}
static fs::path folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) throw std::runtime_error("Could not locate your Windows folders.");
    fs::path path(raw); CoTaskMemFree(raw); return path;
}
static fs::path installDir() { return folder(FOLDERID_LocalAppData) / L"Programs" / L"Voltura Books"; }
static void reg(const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    HKEY handle;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Could not register Voltura Books for your Windows account.");
    auto code = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    if (code != ERROR_SUCCESS) throw std::runtime_error("Could not save Windows registration.");
}
static void regNumber(const wchar_t* key, const wchar_t* name, DWORD value) {
    if (RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof(value)) != ERROR_SUCCESS)
        throw std::runtime_error("Could not save Windows uninstall registration.");
}
static void shortcut(const fs::path& destination, const fs::path& executable, const wchar_t* arguments = L"--settings") {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) throw std::runtime_error("Could not create the Start menu shortcut.");
    HRESULT hr = link->SetPath(executable.c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(arguments);
    if (SUCCEEDED(hr)) hr = link->SetDescription(wcscmp(arguments, L"--settings") == 0 ? L"Configure Send to Kindle" : L"Send books to your Kindle");
    IPersistFile* file = nullptr;
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) { hr = file->Save(destination.c_str(), TRUE); file->Release(); }
    link->Release();
    if (FAILED(hr)) throw std::runtime_error("Could not save the Start menu shortcut.");
}
void installApp() {
    auto source = executablePath().parent_path(), destination = installDir();
    // Check package completeness before registering anything.
    for (auto name : {L"VolturaBooks.exe", L"VolturaBooksReader.exe", L"VolturaBooksDoc.exe", L"DocSharp.Binary.Doc.dll", L"DocSharp.Binary.Common.dll", L"System.IO.Compression.dll", L"uninstall.ps1", L"THIRD-PARTY-NOTICES.txt", L"README.md", L"LICENSE"})
        if (!fs::is_regular_file(source / name)) throw std::runtime_error("Install from the complete dist package. Run scripts/build.ps1 first.");
    fs::create_directories(destination);
    if (!fs::equivalent(source, destination)) {
        for (auto name : {L"VolturaBooks.exe", L"VolturaBooksReader.exe", L"VolturaBooksDoc.exe", L"DocSharp.Binary.Doc.dll", L"DocSharp.Binary.Common.dll", L"System.IO.Compression.dll", L"uninstall.ps1", L"THIRD-PARTY-NOTICES.txt", L"README.md", L"LICENSE"})
            fs::copy_file(source / name, destination / name, fs::copy_options::overwrite_existing);
    }
    auto executable = destination / L"VolturaBooks.exe";
    auto quoted = L"\"" + executable.wstring() + L"\"";
    reg(VerbKey, nullptr, L"Send to Kindle with Voltura Books"); reg(VerbKey, L"Icon", quoted + L",0");
    reg(VerbKey, L"MultiSelectModel", L"Player");
    // Filter the general file verb instead of depending on the reader's
    // extension association being included in Explorer's displayed menu.
    std::wstring filter;
    for(const auto& format:FileFormats) {
        if(!filter.empty()) filter+=L" OR ";
        filter+=L"System.FileExtension:="; filter+=format.extension;
    }
    reg(VerbKey,L"AppliesTo",filter);
    reg((std::wstring(VerbKey) + L"\\command").c_str(), nullptr, quoted + L" --send \"%1\"");
    reg(ShellSelectionKey,nullptr,L"Voltura Books selection handler");
    reg((std::wstring(ShellSelectionKey)+L"\\LocalServer32").c_str(),nullptr,quoted+L" --shell");
    reg((std::wstring(VerbKey)+L"\\DropTarget").c_str(),L"CLSID",ShellSelectionId);
    reg(BrowseVerbKey,nullptr,L"Browse with Voltura Books"); reg(BrowseVerbKey,L"Icon",quoted+L",0");
    reg(BrowseVerbKey,L"MultiSelectModel",L"Single");
    reg((std::wstring(BrowseVerbKey)+L"\\command").c_str(),nullptr,quoted+L" --browse \"%1\"");
    auto legacyStatus = RegDeleteTreeW(HKEY_CURRENT_USER, LegacyVerbKey);
    if (legacyStatus != ERROR_SUCCESS && legacyStatus != ERROR_FILE_NOT_FOUND)
        throw std::runtime_error("Could not remove the previous menu registration.");
    shortcut(folder(FOLDERID_Programs) / L"Voltura Books - Settings.lnk", executable);
    shortcut(folder(FOLDERID_Programs) / L"Voltura Books - Send a book.lnk", executable, L"--drop");
    shortcut(folder(FOLDERID_Programs) / L"Voltura Books - Browse books.lnk", executable, L"--browse");
    for(auto name : {L"Voltura Books.lnk", L"Voltura Books Settings.lnk"}) {
        auto link = folder(FOLDERID_Programs) / name;
        if(!DeleteFileW(link.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("Could not replace the previous Start menu shortcut.");
    }
    reg(UninstallKey, L"DisplayName", L"Voltura Books"); reg(UninstallKey, L"DisplayVersion", L"0.1.4");
    reg(UninstallKey, L"Publisher", L"Voltura AB"); reg(UninstallKey, L"InstallLocation", destination.wstring());
    reg(UninstallKey, L"UninstallString", quoted + L" --uninstall");
    reg(UninstallKey, L"DisplayIcon", quoted + L",0");
    regNumber(UninstallKey, L"NoModify", 1); regNumber(UninstallKey, L"NoRepair", 1);
    uintmax_t installedBytes = 0;
    for (const auto& item : fs::directory_iterator(destination))
        if (item.is_regular_file()) installedBytes += item.file_size();
    regNumber(UninstallKey, L"EstimatedSize", static_cast<DWORD>((installedBytes + 1023) / 1024));
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
}
void uninstallApp() {
    auto directory = installDir();
    auto script = directory / L"uninstall.ps1";
    if (!fs::is_regular_file(script)) throw std::runtime_error("The installed uninstall script is missing. Reinstall the package, then uninstall again.");
    for (const auto key : {VerbKey, BrowseVerbKey, LegacyVerbKey, UninstallKey, ShellSelectionKey}) {
        auto code = RegDeleteTreeW(HKEY_CURRENT_USER, key);
        if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND) throw std::runtime_error("Could not remove Windows registration.");
    }
    removeSettings();
    for(auto name : {L"Voltura Books - Browse books.lnk", L"Voltura Books - Send a book.lnk", L"Voltura Books - Settings.lnk", L"Voltura Books.lnk", L"Voltura Books Settings.lnk"}) {
        auto link=folder(FOLDERID_Programs)/name;
        if(!DeleteFileW(link.c_str()) && GetLastError()!=ERROR_FILE_NOT_FOUND) throw std::runtime_error("Could not remove the Start menu shortcut.");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    auto shell = folder(FOLDERID_System) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    std::wstring command = L"\"" + shell.wstring() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -CleanupPid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW info{}; info.cb = sizeof(info); info.dwFlags = STARTF_USESHOWWINDOW; info.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &info, &process))
        throw std::runtime_error("Settings were removed, but file cleanup could not start. Run the installed uninstall.ps1 to finish.");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
}
