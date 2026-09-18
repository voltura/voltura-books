#pragma once
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <stdexcept>

namespace books {
namespace fs = std::filesystem;
constexpr unsigned long long MaxAttachment = 50'000'000;
struct Settings {
    std::wstring kindle, sender, host;
    int port = 465;
    bool startTls = false;
    bool direct = false;
};
struct MailRoute { std::wstring host; int port = 25; };
using RouteResolver = MailRoute (*)(const std::wstring& domain);
MailRoute directMailRoute(const std::wstring& domain);
struct Result { bool success = false; std::wstring message; };
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::wstring validate(const Settings& settings);
bool validEmailAddress(const std::wstring& address);
std::wstring validateFile(const fs::path& path);
std::string encodedSubject(const std::wstring& filename);
Result sendBook(const Settings&, const std::wstring& password, const fs::path&,
                std::atomic_bool& cancel, const std::string& caFile = {}, RouteResolver resolver = directMailRoute);
fs::path localData();
Settings loadSettings();
std::wstring loadPassword();
void saveSettings(const Settings&, const std::wstring& password);
void removeSettings();
void wipe(std::wstring& value);
bool showSettings(HWND owner, Settings& settings);
void installApp();
void uninstallApp();
fs::path executablePath();
}
