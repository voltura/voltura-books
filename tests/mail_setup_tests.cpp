#include "mail_setup.h"
#include <iostream>
int main() {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    auto config=[](const char* socket,const char* authentication,const char* user) {
        return std::string("<clientConfig><emailProvider><outgoingServer type='smtp'><hostname>smtp.example.com</hostname><port>465</port><socketType>")+socket+"</socketType><username>"+user+"</username><authentication>"+authentication+"</authentication></outgoingServer></emailProvider></clientConfig>";
    };
    int failures=0;
    auto check=[&](bool condition,const char* name){if(!condition){std::cerr<<name<<'\n';++failures;}};
    auto tls=books::parseMailSetup(config("SSL","password-cleartext","%EMAILADDRESS%"));
    check(tls.host==L"smtp.example.com" && tls.port==465 && !tls.startTls,"TLS provider");
    auto start=books::parseMailSetup(config("STARTTLS","password-cleartext","%EMAILADDRESS%"));
    check(!start.host.empty() && start.startTls,"STARTTLS provider");
    check(books::parseMailSetup(config("plain","password-cleartext","%EMAILADDRESS%")).host.empty(),"No insecure discovery");
    auto oauth=books::parseMailSetup(config("SSL","OAuth2","%EMAILADDRESS%"));
    check(oauth.host.empty() && !oauth.guidance.empty(),"OAuth-only provider needs explicit guidance");
    check(books::parseMailSetup(config("SSL","password-cleartext","%EMAILLOCALPART%")).host.empty(),"Unsupported username must not be guessed");
    check(books::parseMailSetup("<bad>").host.empty(),"Malformed XML");
    check(books::passwordHelp(L"send.one.com").label==L"Email password", "Normal email password label");
    check(books::passwordHelp(L"SMTP.GMAIL.COM").label==L"Google app password", "Gmail password label");
    check(!books::passwordHelp(L"smtp.mail.yahoo.com").url.empty(), "Yahoo help link");
    check(!books::passwordHelp(L"smtp.mail.me.com").url.empty(), "Apple help link");
    CoUninitialize();
    if(!failures) std::cout<<"Mail setup checks passed\n";
    return failures ? 1 : 0;
}
