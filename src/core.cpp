#include <winsock2.h>
#include <ws2tcpip.h>
#include "core.h"
#include "formats.h"
#include <curl/curl.h>
#include <algorithm>
#include <memory>
#include <vector>
#include <cwctype>
#include <windns.h>

namespace books {
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode text.");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
void wipe(std::wstring& s) { if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t)); s.clear(); }
bool validEmailAddress(const std::wstring& s) {
    auto at = s.find(L'@');
    if (s.size() > 254 || at == 0 || at == s.npos || at>64 || at + 1 == s.size() || s.find(L'@', at + 1) != s.npos) return false;
    auto letter=[](wchar_t c){return (c>=L'a' && c<=L'z') || (c>=L'A' && c<=L'Z');};
    auto alnum=[&](wchar_t c){return letter(c) || (c>=L'0' && c<=L'9');};
    const auto local=s.substr(0,at),domain=s.substr(at+1);
    if(local.front()==L'.' || local.back()==L'.' || local.find(L"..")!=local.npos) return false;
    for(auto c:local) if(!alnum(c) && std::wstring(L".!#$%&'*+-/=?^_`{|}~").find(c)==std::wstring::npos) return false;
    if(domain.size()>253 || domain.find(L'.')==domain.npos) return false;
    for(size_t start=0;start<domain.size();) {
        auto end=domain.find(L'.',start); if(end==domain.npos) end=domain.size();
        auto label=domain.substr(start,end-start);
        if(label.empty() || label.size()>63 || !alnum(label.front()) || !alnum(label.back())) return false;
        for(auto c:label) if(!alnum(c) && c!=L'-') return false;
        if(end==domain.size()) break;
        start=end+1; if(start==domain.size()) return false;
    }
    auto suffix=domain.substr(domain.rfind(L'.')+1);
    return (suffix.size()>=2 && std::all_of(suffix.begin(),suffix.end(),letter)) ||
        (suffix.size()>4 && _wcsnicmp(suffix.c_str(),L"xn--",4)==0);
}
std::wstring validate(const Settings& s) {
    if (!validEmailAddress(s.kindle)) return L"Check your Kindle email address. Use an address such as name@kindle.com.";
    if (!validEmailAddress(s.sender)) return L"Check your sender email address. Use an address such as name@example.com.";
    if (s.direct) return {};
    IN_ADDR ipv4{}; IN6_ADDR ipv6{};
    const bool ip=InetPtonW(AF_INET,s.host.c_str(),&ipv4)==1 || InetPtonW(AF_INET6,s.host.c_str(),&ipv6)==1;
    // Windows validates DNS syntax, not DNS registration or reachability.
    wchar_t asciiHost[256]{};
    const auto asciiLength=IdnToAscii(IDN_USE_STD3_ASCII_RULES,s.host.c_str(),-1,asciiHost,256);
    if(!ip && (s.host.empty() || s.host.find_first_not_of(L"0123456789.")==std::wstring::npos ||
        !asciiLength || DnsValidateName_W(asciiHost,DnsNameHostnameFull)!=ERROR_SUCCESS ||
        s.host.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")!=std::wstring::npos))
        return L"Enter a valid mail server hostname or IPv4/IPv6 address, without a URL or port.";
    if (s.port < 1 || s.port > 65535) return L"Enter a port between 1 and 65535.";
    return {};
}
struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    unsigned long long size = 0;
    explicit File(const fs::path& path) {
        auto ext = path.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (!fileFormat(ext)) throw std::runtime_error("Choose a supported book or document, such as EPUB, PDF or RTF. AZW, AZW3 and MOBI cannot be sent through this app.");
        auto absolute = fs::absolute(path).wstring();
        if (absolute.rfind(L"\\\\?\\", 0) != 0) {
            if (absolute.rfind(L"\\\\", 0) == 0) absolute = L"\\\\?\\UNC\\" + absolute.substr(2);
            else absolute = L"\\\\?\\" + absolute;
        }
        handle = CreateFileW(absolute.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("The file could not be opened. Check that it exists, is downloaded, and is not in use.");
        LARGE_INTEGER length{};
        if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &length) || length.QuadPart <= 0 || length.QuadPart > MaxAttachment) {
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Choose a non-empty file no larger than 50 MB.");
        }
        size = static_cast<unsigned long long>(length.QuadPart);
    }
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
std::wstring validateFile(const fs::path& path) {
    try { File file(path); return {}; } catch (const std::exception& e) { return wide(e.what()); }
}
static std::string base64(const std::string& s) {
    constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < s.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(s[i]) << 16;
        if (i + 1 < s.size()) n |= static_cast<unsigned char>(s[i + 1]) << 8;
        if (i + 2 < s.size()) n |= static_cast<unsigned char>(s[i + 2]);
        out += chars[n >> 18]; out += chars[(n >> 12) & 63];
        out += i + 1 < s.size() ? chars[(n >> 6) & 63] : '=';
        out += i + 2 < s.size() ? chars[n & 63] : '=';
    }
    return out;
}
std::string encodedSubject(const std::wstring& filename) {
    const auto input = utf8(filename);
    std::string output = "Subject: ";
    for (size_t i = 0; i < input.size();) {
        auto end = std::min(i + 42, input.size());
        while (end < input.size() && (static_cast<unsigned char>(input[end]) & 0xc0) == 0x80) --end;
        if (i) output += "\r\n ";
        output += "=?UTF-8?B?" + base64(input.substr(i, end - i)) + "?=";
        i = end;
    }
    return output;
}
static size_t readFile(char* buf, size_t size, size_t count, void* context) {
    DWORD read = 0;
    if (!ReadFile(static_cast<File*>(context)->handle, buf, static_cast<DWORD>(std::min<size_t>(size * count, MAXDWORD)), &read, nullptr)) return CURL_READFUNC_ABORT;
    return read;
}
static int seekFile(void* context, curl_off_t offset, int origin) {
    LARGE_INTEGER pos; pos.QuadPart = offset;
    return SetFilePointerEx(static_cast<File*>(context)->handle, pos, nullptr,
        origin == SEEK_SET ? FILE_BEGIN : origin == SEEK_CUR ? FILE_CURRENT : FILE_END) ? CURL_SEEKFUNC_OK : CURL_SEEKFUNC_FAIL;
}
static int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic_bool*>(context)->load() ? 1 : 0;
}
struct Headers {
    curl_slist* value = nullptr;
    ~Headers() { curl_slist_free_all(value); }
    void add(const std::string& s) {
        auto next = curl_slist_append(value, s.c_str());
        if (!next) throw std::bad_alloc();
        value = next;
    }
};
MailRoute directMailRoute(const std::wstring& domain) {
    PDNS_RECORD records = nullptr;
    const auto status = DnsQuery_W(domain.c_str(), DNS_TYPE_MX, DNS_QUERY_STANDARD, nullptr, &records, nullptr);
    std::unique_ptr<DNS_RECORD, void(*)(DNS_RECORD*)> owned(records, [](DNS_RECORD* p) { if(p) DnsRecordListFree(p, DnsFreeRecordList); });
    if (status != ERROR_SUCCESS) throw std::runtime_error("Could not find the Kindle email server. Check your internet connection and Kindle email address.");
    std::wstring host;
    unsigned preference = 65536;
    for (auto r = records; r; r = r->pNext) {
        if (r->wType == DNS_TYPE_MX && r->Data.MX.pNameExchange && r->Data.MX.wPreference < preference) {
            host = r->Data.MX.pNameExchange; preference = r->Data.MX.wPreference;
        }
    }
    if (!host.empty() && host.back() == L'.') host.pop_back();
    Settings check; check.kindle = L"reader@example.com"; check.sender = L"sender@example.com"; check.host = host;
    if (!validate(check).empty()) throw std::runtime_error("No receiving mail server was found for this Kindle address. Check Settings.");
    return {host, 25};
}
Result sendBook(const Settings& s, const std::wstring& password, const fs::path& path, std::atomic_bool& cancel, const std::string& caFile, RouteResolver resolver) {
    try {
        if (auto error = validate(s); !error.empty()) return {false, error};
        if (!s.direct && password.empty()) return {false, L"Open Settings and enter your email password."};
        File file(path);
        const auto route = s.direct ? resolver(s.kindle.substr(s.kindle.find(L'@') + 1)) : MailRoute{s.host, s.port};
        if(cancel) return {false, L"Sending was cancelled before connecting."};
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) throw std::runtime_error("Could not initialize email sending.");
        auto set = [&](CURLoption key, auto value) {
            if (curl_easy_setopt(curl.get(), key, value) != CURLE_OK) throw std::runtime_error("Could not configure email sending.");
        };
        const auto url = std::string(s.direct || s.startTls ? "smtp://" : "smtps://") + (route.host.find(L':')!=std::wstring::npos ? "["+utf8(route.host)+"]" : utf8(route.host)) + ":" + std::to_string(route.port);
        const auto sender = utf8(s.sender), recipient = utf8(s.kindle);
        auto secret = utf8(password);
        set(CURLOPT_URL, url.c_str()); set(CURLOPT_PROTOCOLS_STR, "smtp,smtps");
        set(CURLOPT_PROXY, "");
        if (!s.direct) {
        set(CURLOPT_USERNAME, sender.c_str());
        auto secretStatus = curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, secret.c_str());
        SecureZeroMemory(secret.data(), secret.size());
        if (secretStatus != CURLE_OK) throw std::runtime_error("Could not configure credentials.");
        }
        set(CURLOPT_USE_SSL, static_cast<long>(s.direct ? CURLUSESSL_TRY : CURLUSESSL_ALL));
        set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        if (!caFile.empty()) set(CURLOPT_CAINFO, caFile.c_str());
        set(CURLOPT_CONNECTTIMEOUT, 20L); set(CURLOPT_TIMEOUT, 300L);
        set(CURLOPT_LOW_SPEED_LIMIT, 1L); set(CURLOPT_LOW_SPEED_TIME, 45L);
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_MAIL_FROM, sender.c_str());
        Headers recipients; recipients.add(recipient); set(CURLOPT_MAIL_RCPT, recipients.value);
        Headers headers;
        headers.add("From: " + sender); headers.add("To: " + recipient);
        headers.add(encodedSubject(path.filename().wstring()));
        headers.add("MIME-Version: 1.0");
        SYSTEMTIME time{}; GetSystemTime(&time);
        constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char date[100];
        sprintf_s(date, "Date: %s, %02u %s %04u %02u:%02u:%02u +0000", days[time.wDayOfWeek], time.wDay, months[time.wMonth - 1], time.wYear, time.wHour, time.wMinute, time.wSecond);
        headers.add(date);
        GUID id{}; if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a message identifier.");
        wchar_t guid[40]; StringFromGUID2(id, guid, 40);
        headers.add("Message-ID: <" + utf8(std::wstring(guid).substr(1, 36)) + "@" + sender.substr(sender.find('@') + 1) + ">");
        set(CURLOPT_HTTPHEADER, headers.value);
        std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(curl_mime_init(curl.get()), curl_mime_free);
        if (!mime) throw std::bad_alloc();
        auto check = [](CURLcode c) { if (c != CURLE_OK) throw std::runtime_error("Could not prepare the attachment."); };
        auto body = curl_mime_addpart(mime.get());
        if (!body) throw std::bad_alloc();
        check(curl_mime_data(body, "Sent with Voltura Books.\r\n", CURL_ZERO_TERMINATED));
        check(curl_mime_type(body, "text/plain; charset=utf-8"));
        auto attachment = curl_mime_addpart(mime.get());
        if (!attachment) throw std::bad_alloc();
        check(curl_mime_data_cb(attachment, static_cast<curl_off_t>(file.size), readFile, seekFile, nullptr, &file));
        check(curl_mime_filename(attachment, utf8(path.filename().wstring()).c_str()));
        check(curl_mime_type(attachment, fileFormat(path.extension().wstring())->mime));
        check(curl_mime_encoder(attachment, "base64"));
        set(CURLOPT_MIMEPOST, mime.get());
        set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_XFERINFOFUNCTION, progress); set(CURLOPT_XFERINFODATA, &cancel);
        auto code = curl_easy_perform(curl.get());
        long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        if (code == CURLE_OK && status >= 200 && status < 300) return {true, L"Email sent. Kindle delivery may take a few minutes."};
        if (code == CURLE_LOGIN_DENIED) return {false, L"The mail server rejected your login. Open Settings and check your sender email address and password."};
        if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR || code == CURLE_SSL_CACERT_BADFILE)
            return {false, L"A verified secure connection could not be established. Check the mail server settings and your Windows date and time."};
        if (code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT)
            return {false, s.direct ? L"Could not reach the Kindle mail server. Your network may block direct sending (port 25). In Settings, try Send through my email provider."
                : L"Could not reach the mail server. Check your internet connection and server settings, then retry."};
        if (status >= 400) return {false, L"The mail server rejected the email (SMTP " + std::to_wstring(status) + (s.direct
            ? L"). Check the Kindle address and approved sender list, or choose Send through my email provider in Settings."
            : L"). Check the recipient and your provider's attachment limits.")};
        return {false, cancel ? L"Sending was cancelled. Delivery may be uncertain if the email was already submitted. Check your Kindle before retrying."
            : L"The connection ended before delivery could be confirmed. The email may already have been submitted. Check your Kindle before retrying."};
    } catch (const std::exception& e) { return {false, wide(e.what())}; }
}
}
