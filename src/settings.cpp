#include "core.h"
#include <shlobj.h>
#include <wincred.h>
#include <vector>

namespace books {
static constexpr wchar_t CredentialName[] = L"VolturaBooks/SMTP";
fs::path localData() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) throw std::runtime_error("Could not locate application data.");
    fs::path path(value); CoTaskMemFree(value); return path / L"Voltura Books";
}
Settings loadSettings() {
    Settings s;
    const auto path = localData() / L"settings.ini";
    auto read = [&](const wchar_t* key, const wchar_t* fallback) {
        wchar_t text[1024]{};
        GetPrivateProfileStringW(L"Mail", key, fallback, text, 1024, path.c_str());
        return std::wstring(text);
    };
    s.kindle = read(L"Kindle", L""); s.sender = read(L"Sender", L"");
    s.host = read(L"Host", L"");
    s.port = static_cast<int>(GetPrivateProfileIntW(L"Mail", L"Port", 465, path.c_str()));
    s.startTls = read(L"Security", L"TLS") == L"STARTTLS";
    s.direct = read(L"Method", L"Provider") == L"Direct";
    return s;
}
std::wstring loadPassword() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(CredentialName, CRED_TYPE_GENERIC, 0, &credential)) {
        if (GetLastError() == ERROR_NOT_FOUND) return {};
        throw std::runtime_error("Could not read the saved password from Windows Credential Manager.");
    }
    std::wstring password(reinterpret_cast<wchar_t*>(credential->CredentialBlob), credential->CredentialBlobSize / sizeof(wchar_t));
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize); CredFree(credential);
    return password;
}
static void writePassword(const std::wstring& sender, const std::wstring& password) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(CredentialName);
    credential.UserName = const_cast<wchar_t*>(sender.c_str());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(password.data()));
    credential.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) throw std::runtime_error("Could not save the password in Windows Credential Manager.");
}
void saveSettings(const Settings& s, const std::wstring& password) {
    if (!validate(s).empty() || password.size() * sizeof(wchar_t) > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        throw std::runtime_error("Check your settings. Email passwords can contain up to 1,280 characters.");
    auto dir = localData(); fs::create_directories(dir);
    auto target = dir / L"settings.ini", temp = dir / L"settings.pending.ini";
    auto old = loadSettings(); auto oldPassword = s.direct ? std::wstring{} : loadPassword();
    try {
        // A Unicode BOM makes the Windows INI API preserve non-ASCII text.
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not write settings.");
        const wchar_t bom = 0xfeff; DWORD written;
        bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
        CloseHandle(file);
        if (!ok) throw std::runtime_error("Could not write settings.");
        auto write = [&](const wchar_t* key, const std::wstring& value) {
            if (!WritePrivateProfileStringW(L"Mail", key, value.c_str(), temp.c_str())) throw std::runtime_error("Could not write settings.");
        };
        write(L"Kindle", s.kindle); write(L"Sender", s.sender); write(L"Host", s.host);
        write(L"Port", std::to_wstring(s.port)); write(L"Security", s.startTls ? L"STARTTLS" : L"TLS");
        write(L"Method", s.direct ? L"Direct" : L"Provider");
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temp.c_str());
        if (!s.direct) {
            if(password.empty()) {
                if(!CredDeleteW(CredentialName,CRED_TYPE_GENERIC,0) && GetLastError()!=ERROR_NOT_FOUND)
                    throw std::runtime_error("Could not remove the saved password.");
            } else writePassword(s.sender,password);
        }
        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (!s.direct) {
                if (oldPassword.empty()) CredDeleteW(CredentialName, CRED_TYPE_GENERIC, 0);
                else writePassword(old.sender, oldPassword);
            }
            throw std::runtime_error("Could not replace saved settings.");
        }
        wipe(oldPassword);
        // An old provider password must never be reused for a different sender.
        if (s.direct && old.sender != s.sender) CredDeleteW(CredentialName, CRED_TYPE_GENERIC, 0);
    } catch (...) { wipe(oldPassword); DeleteFileW(temp.c_str()); throw; }
}
void removeSettings() {
    if (!CredDeleteW(CredentialName, CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
        throw std::runtime_error("Could not remove the saved password.");
    for (auto name : {L"settings.ini", L"settings.pending.ini", L"sent-books.tsv"}) {
        auto path = localData() / name;
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Could not remove saved settings.");
    }
    std::error_code error;
    fs::remove_all(localData() / L"Updates",error);
    if(error) throw std::runtime_error("Could not remove downloaded updates.");
    RemoveDirectoryW(localData().c_str());
}
}
