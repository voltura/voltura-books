#include "core.h"
#include <curl/curl.h>
#include <iostream>
static int fixturePort;
static books::MailRoute fixtureRoute(const std::wstring& domain) {
    if(domain != L"kindle.com") throw std::runtime_error("Wrong recipient domain");
    return {L"localhost",fixturePort};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    books::Settings s; s.host = L"localhost"; s.port = _wtoi(argv[1]);
    s.sender = L"sender@example.com"; s.kindle = L"reader@kindle.com";
    s.startTls = std::wstring(argv[4]) == L"starttls";
    s.direct = std::wstring(argv[4]) == L"direct";
    fixturePort = s.port;
    std::atomic_bool cancel = false;
    auto result = books::sendBook(s, s.direct ? L"" : L"fixture-password", argv[2], cancel, books::utf8(argv[3]), fixtureRoute);
    std::cout << books::utf8(result.message) << '\n';
    curl_global_cleanup(); return result.success ? 0 : 1;
}
