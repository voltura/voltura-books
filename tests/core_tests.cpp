#include "core.h"
#include "formats.h"
#include <iostream>
#include <fstream>
int main() {
    int failures = 0;
    auto check = [&](bool pass, const char* name) { if (!pass) { std::cerr << "FAIL: " << name << '\n'; ++failures; } };
    check(books::readableFileSize(123000)==L"123 KB","readable kilobytes");
    check(books::readableFileSize(2000000)==L"2 MB","whole megabytes");
    check(books::readableFileSize(2400000)==L"2.4 MB","fractional megabytes");
    check(books::readableFileSize(500)==L"500 B","small sizes");
    books::Settings s; s.kindle = L"reader@kindle.com"; s.sender = L"sender@example.com"; s.host = L"smtp.example.com";
    check(books::validate(s).empty(), "valid settings");
    for(auto value:{L"name+tag@example.co.uk",L"first.last@example.com",L"o'connor@example.com",L"user@sub-domain.example.com",L"user@example.xn--p1ai"})
        check(books::validEmailAddress(value),"valid public email syntax");
    for(auto value:{L"name@voltura.se123232",L"name@@example.com",L"name@example",L".name@example.com",L"name.@example.com",L"first..last@example.com",L"name@-example.com",L"name@example-.com",L"name@exam_ple.com",L"name@example..com",L"name@example.com.",L"name @example.com",L"name@example.123",L"name\\@example.com"})
        check(!books::validEmailAddress(value),"invalid public email syntax");
    check(!books::validEmailAddress(std::wstring(65,L'a')+L"@example.com"),"local part length limit");
    check(!books::validEmailAddress(L"name@"+std::wstring(64,L'a')+L".com"),"domain label length limit");
    for(auto host:{L"send.one.com�",L"send..one.com",L"-send.one.com",L"send-.one.com",L"send_one.com",L"https://send.one.com",L"send.one.com:465"}) {
        s.host=host; check(!books::validate(s).empty(),"invalid server name rejected");
    }
    for(auto host:{L"send.one.com",L"localhost",L"192.0.2.1",L"2001:db8::1",L"::1",L"x.com23234234234234234"}) {
        s.host=host; check(books::validate(s).empty(),"Windows hostname or IP syntax");
    }
    for(auto host:{L"256.256.256.256",L"2001:db8:::1",L"[::1]:465"}) {
        s.host=host; check(!books::validate(s).empty(),"invalid IP address");
    }
    s.host=L"send.one.com";
    s.sender = L"sender@example.com\r\nBcc: victim@example.com";
    check(!books::validate(s).empty(), "header injection rejected");
    s.sender = L"sender@example.com"; s.host = L"send.one.com/evil";
    check(!books::validate(s).empty(), "host URL rejected");
    s.host = L"send.one.com"; s.port = 0;
    check(!books::validate(s).empty(), "port range");
    s.direct = true; s.host.clear();
    check(books::validate(s).empty(), "direct needs no provider host or port");
    s.sender.clear();
    check(!books::validate(s).empty(), "direct still needs valid sender");
    auto unicode = L"Böcker 日本語 📖.epub";
    check(books::wide(books::utf8(unicode)) == unicode, "Unicode roundtrip");
    check(books::encodedSubject(L"test.epub") == "Subject: =?UTF-8?B?dGVzdC5lcHVi?=", "subject encoding");
    auto root = books::fs::temp_directory_path() / (L"VolturaBooks-tests-" + std::to_wstring(GetCurrentProcessId()));
    books::fs::create_directories(root);
    auto path = root / unicode;
    { std::ofstream file(path, std::ios::binary); file << "PK test"; }
    check(books::validateFile(path).empty(), "Unicode file readable");
    check(!books::validateFile(root / L"absent.epub").empty(), "missing file");
    check(!books::validateFile(root / L"book.pdf").empty(), "wrong extension");
    for(const auto& format:books::FileFormats) {
        auto document=root/(std::wstring(L"sample")+format.extension);
        { std::ofstream file(document); file<<"fixture"; }
        check(books::validateFile(document).empty(),"supported format accepted"); books::fs::remove(document);
    }
    for(auto extension:{L".azw",L".azw3",L".mobi",L".exe"}) check(!books::fileFormat(extension),"unsupported format rejected");
    check(books::fileFormat(L".PDF")!=nullptr,"case insensitive formats");
    books::fs::resize_file(path, 0);
    check(!books::validateFile(path).empty(), "empty file");
    books::fs::resize_file(path, books::MaxAttachment);
    check(books::validateFile(path).empty(), "size boundary");
    books::fs::resize_file(path, books::MaxAttachment + 1);
    check(!books::validateFile(path).empty(), "oversize rejected");
    books::fs::remove(path); books::fs::remove(root);
    std::cout << (failures ? "FAILED" : "All core checks passed") << '\n';
    return failures ? 1 : 0;
}
