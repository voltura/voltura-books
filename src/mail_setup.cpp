#include "mail_setup.h"
#include <winhttp.h>
#include <windns.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

namespace books {
PasswordHelp passwordHelp(std::wstring host) {
    for(auto& c:host) c=static_cast<wchar_t>(towlower(c));
    if(host==L"smtp.gmail.com" || host==L"smtp.googlemail.com")
        return {L"Google app password", L"Google needs a special password for this app. Turn on 2-Step Verification in your Google Account, then use the link below to create a password and paste it here.", L"Create a Google app password", L"https://myaccount.google.com/apppasswords"};
    if(host==L"smtp.mail.yahoo.com")
        return {L"Yahoo app password", L"Yahoo needs a special password for this app. Follow the instructions below to create one, then paste it here.", L"How to create a Yahoo app password", L"https://help.yahoo.com/kb/SLN15241.html"};
    if(host==L"smtp.mail.me.com")
        return {L"Apple app-specific password", L"Apple needs a special password for this app. Follow the instructions below to create one, then paste it here.", L"How to create an Apple app-specific password", L"https://support.apple.com/102654"};
    return {L"Email password", L"Use the password you normally use to sign in to your sender email account.", L"", L""};
}
namespace {
using Microsoft::WRL::ComPtr;
struct Internet {
    HINTERNET value;
    ~Internet() { if(value) WinHttpCloseHandle(value); }
};
bool domainName(const std::wstring& value) {
    return !value.empty() && value.size()<=253 && value.front()!=L'.' &&
        std::all_of(value.begin(),value.end(),[](wchar_t c) { return (c>=L'a' && c<=L'z') || (c>=L'A' && c<=L'Z') || (c>=L'0' && c<=L'9') || c==L'.' || c==L'-'; });
}
std::string fetch(const std::wstring& domain) {
    if(!domainName(domain)) return {};
    Internet session{WinHttpOpen(L"VolturaBooks/0.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.value) return {};
    WinHttpSetTimeouts(session.value,3000,3000,3000,3000);
    Internet connection{WinHttpConnect(session.value,L"autoconfig.thunderbird.net",INTERNET_DEFAULT_HTTPS_PORT,0)};
    if(!connection.value) return {};
    auto path=L"/v1.1/"+domain;
    Internet request{WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!request.value) return {};
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
    if(!WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request.value,nullptr)) return {};
    DWORD status=0,length=sizeof(status);
    if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX) || status!=200) return {};
    std::string xml;
    char buffer[4096]; DWORD count=0;
    do {
        if(!WinHttpReadData(request.value,buffer,sizeof(buffer),&count) || xml.size()+count>128*1024) return {};
        xml.append(buffer,count);
    } while(count);
    return xml;
}
std::wstring mxProvider(const std::wstring& domain) {
    PDNS_RECORD records=nullptr;
    if(DnsQuery_W(domain.c_str(),DNS_TYPE_MX,DNS_QUERY_STANDARD,nullptr,&records,nullptr)!=ERROR_SUCCESS) return {};
    std::wstring provider;
    for(auto record=records;record;record=record->pNext) {
        if(record->wType!=DNS_TYPE_MX || !record->Data.MX.pNameExchange) continue;
        std::wstring mx=record->Data.MX.pNameExchange;
        for(auto& c:mx) c=static_cast<wchar_t>(towlower(c));
        if(!mx.empty() && mx.back()==L'.') mx.pop_back();
        // Only recognize known provider domains; MX records alone are not SMTP settings.
        if(mx.ends_with(L".one.com")) provider=L"one.com";
        else if(mx.ends_with(L".google.com") || mx.ends_with(L".googlemail.com")) provider=L"gmail.com";
        else if(mx.ends_with(L".outlook.com")) provider=L"outlook.com";
        if(!provider.empty()) break;
    }
    DnsRecordListFree(records,DnsFreeRecordList); return provider;
}
}
MailSetup parseMailSetup(const std::string& xml) {
    MailSetup result;
    if(xml.empty() || xml.size()>128*1024) return result;
    ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(xml.data()),static_cast<UINT>(xml.size())));
    ComPtr<IXmlReader> reader;
    if(!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(reader.GetAddressOf()),nullptr))) return result;
    reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
    reader->SetProperty(XmlReaderProperty_MaxElementDepth,32);
    if(FAILED(reader->SetInput(stream.Get()))) return result;
    bool outgoing=false, password=false; std::wstring field;
    std::map<std::wstring,std::wstring> fields;
    XmlNodeType type; HRESULT hr;
    while((hr=reader->Read(&type))==S_OK) {
        const wchar_t* raw=nullptr;
        if(type==XmlNodeType_Element) {
            reader->GetLocalName(&raw,nullptr); field=raw;
            if(field==L"outgoingServer") {
                outgoing=false; password=false; fields.clear();
                if(reader->MoveToAttributeByName(L"type",nullptr)==S_OK) {
                    reader->GetValue(&raw,nullptr); outgoing=std::wstring(raw)==L"smtp"; reader->MoveToElement();
                }
            }
        } else if(outgoing && (type==XmlNodeType_Text || type==XmlNodeType_CDATA)) {
            reader->GetValue(&raw,nullptr); fields[field]+=raw;
            if(field==L"authentication" && (std::wstring(raw)==L"password-cleartext" || std::wstring(raw)==L"password-encrypted")) password=true;
        } else if(type==XmlNodeType_EndElement) {
            reader->GetLocalName(&raw,nullptr);
            if(std::wstring(raw)==L"outgoingServer" && outgoing) {
                if(result.host.empty() && password && fields[L"username"]==L"%EMAILADDRESS%" && domainName(fields[L"hostname"]) &&
                    (fields[L"socketType"]==L"SSL" || fields[L"socketType"]==L"STARTTLS")) {
                    auto portText=fields[L"port"];
                    if(!portText.empty() && portText.size()<=5 && std::all_of(portText.begin(),portText.end(),[](wchar_t c){return c>=L'0' && c<=L'9';})) {
                        int port=std::stoi(portText);
                        if(port>0 && port<=65535) {
                            result.host=fields[L"hostname"]; result.port=port; result.startTls=fields[L"socketType"]==L"STARTTLS";
                            result.guidance=L"Email server found: "+result.host+L". Use the password for your sender email account.";
                            if(result.host==L"smtp.gmail.com") result.guidance=L"Gmail server found. Use a Google app password, not your normal account password.";
                        }
                    }
                } else if(!password && fields[L"authentication"].find(L"OAuth2")!=std::wstring::npos) {
                    result.guidance=L"This provider requires browser sign-in, which this version does not support. Use another sender email address.";
                }
                outgoing=false;
            }
            field.clear();
        }
    }
    if(hr!=S_FALSE) return {};
    return result;
}
MailSetup discoverMailSetup(const std::wstring& domain) noexcept {
    try {
        if(!domainName(domain)) return {};
        auto result=parseMailSetup(fetch(domain));
        if(!result.host.empty() || !result.guidance.empty()) return result;
        auto provider=mxProvider(domain);
        if(!provider.empty() && provider!=domain) return parseMailSetup(fetch(provider));
    } catch(...) {}
    return {};
}
}
