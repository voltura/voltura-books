#include "update.h"
#include "books_version.h"
#include "resource.h"
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace books {
namespace {
void require(bool ok) { if(!ok) throw std::runtime_error("Update verification or download failed."); }
std::array<unsigned,3> version(const std::wstring& text) {
    std::array<unsigned,3> result{}; size_t pos=0;
    for(size_t i=0;i<3;++i) {
        auto start=pos;
        while(pos<text.size() && text[pos]>=L'0' && text[pos]<=L'9') {
            require(result[i]<=6553); result[i]=result[i]*10+text[pos++]-L'0'; require(result[i]<=65535);
        }
        require(pos>start && (pos-start==1 || text[start]!=L'0'));
        if(i<2) require(pos<text.size() && text[pos++]==L'.');
    }
    require(pos==text.size()); return result;
}
struct Internet { HINTERNET value{}; ~Internet(){if(value) WinHttpCloseHandle(value);} };
std::vector<unsigned char> fetch(std::wstring url,size_t limit,bool* missing=nullptr) {
    Internet session{WinHttpOpen(L"VolturaBooks/" BOOKS_VERSION,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};
    require(session.value!=nullptr); WinHttpSetTimeouts(session.value,10000,10000,10000,10000);
    const auto deadline=GetTickCount64()+120000;
    for(int redirects=0;redirects<5;++redirects) {
        URL_COMPONENTS parts{}; parts.dwStructSize=sizeof(parts); parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
        require(WinHttpCrackUrl(url.c_str(),0,0,&parts)!=FALSE);
        std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
        require(parts.nScheme==INTERNET_SCHEME_HTTPS && parts.nPort==443);
        require(host==L"api.github.com" || host==L"github.com" || host==L"release-assets.githubusercontent.com");
        if(parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        Internet connection{WinHttpConnect(session.value,host.c_str(),443,0)}; require(connection.value!=nullptr);
        Internet request{WinHttpOpenRequest(connection.value,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
        require(request.value!=nullptr);
        DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        require(WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy))!=FALSE);
        require(WinHttpSendRequest(request.value,L"Accept: application/json\r\n",DWORD(-1),nullptr,0,0,0)!=FALSE);
        require(WinHttpReceiveResponse(request.value,nullptr)!=FALSE);
        DWORD status{},length=sizeof(status);
        require(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr)!=FALSE);
        if(status==404 && missing) { *missing=true; return {}; }
        if(status==301 || status==302 || status==303 || status==307 || status==308) {
            DWORD bytes=0; WinHttpQueryHeaders(request.value,WINHTTP_QUERY_LOCATION,nullptr,nullptr,&bytes,nullptr);
            require(bytes>0 && bytes<16384); std::wstring next(bytes/sizeof(wchar_t),L'\0');
            require(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_LOCATION,nullptr,next.data(),&bytes,nullptr)!=FALSE);
            next.resize(wcslen(next.c_str())); url=next; continue;
        }
        require(status==200); std::vector<unsigned char> body; unsigned char chunk[16384]; DWORD count=0;
        do { require(GetTickCount64()<deadline); require(WinHttpReadData(request.value,chunk,sizeof(chunk),&count)!=FALSE);
            require(count<=limit-body.size()); body.insert(body.end(),chunk,chunk+count);
        } while(count);
        return body;
    }
    throw std::runtime_error("Too many redirects.");
}
std::wstring hash(const std::vector<unsigned char>& data) {
    unsigned char digest[32]{};
    require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<PUCHAR>(data.data()),static_cast<ULONG>(data.size()),digest,sizeof(digest))>=0);
    std::wstring text; for(auto b:digest) { text+=L"0123456789abcdef"[b>>4]; text+=L"0123456789abcdef"[b&15]; } return text;
}
}
bool newerVersion(const std::wstring& candidate,const std::wstring& current) { return version(candidate)>version(current); }
VerifiedUpdate verifyUpdate(const std::string& manifest,const std::vector<unsigned char>& signature,const std::string& publicKey,const std::wstring& current) {
    require(manifest.size()<=65536 && signature.size()<=1024);
    DWORD size=0; require(CryptStringToBinaryA(publicKey.c_str(),0,CRYPT_STRING_BASE64HEADER,nullptr,&size,nullptr,nullptr)!=FALSE);
    std::vector<unsigned char> der(size); require(CryptStringToBinaryA(publicKey.c_str(),0,CRYPT_STRING_BASE64HEADER,der.data(),&size,nullptr,nullptr)!=FALSE);
    CERT_PUBLIC_KEY_INFO* info=nullptr; DWORD infoSize=0;
    require(CryptDecodeObjectEx(X509_ASN_ENCODING,X509_PUBLIC_KEY_INFO,der.data(),size,CRYPT_DECODE_ALLOC_FLAG,nullptr,&info,&infoSize)!=FALSE);
    BCRYPT_KEY_HANDLE key{}; auto imported=CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING,info,0,nullptr,&key); LocalFree(info); require(imported!=FALSE);
    unsigned char digest[32]{}; auto hashed=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(manifest.data())),static_cast<ULONG>(manifest.size()),digest,32);
    BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM,32};
    auto valid=hashed>=0 ? BCryptVerifySignature(key,&padding,digest,32,const_cast<PUCHAR>(signature.data()),static_cast<ULONG>(signature.size()),BCRYPT_PAD_PSS) : hashed;
    BCryptDestroyKey(key); require(valid>=0);
    auto json=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(manifest));
    require(json.GetNamedNumber(L"schema")==1);
    VerifiedUpdate update; update.version=json.GetNamedString(L"version"); require(newerVersion(update.version,current));
    auto assets=json.GetNamedArray(L"assets"); require(assets.Size()==1); auto asset=assets.GetObjectAt(0);
    update.name=asset.GetNamedString(L"name"); require(update.name==L"VolturaBooks-Setup-"+update.version+L"-win-x64.exe");
    auto bytes=asset.GetNamedNumber(L"size"); require(bytes>0 && bytes<=50*1024*1024 && bytes==static_cast<size_t>(bytes)); update.size=static_cast<size_t>(bytes);
    update.sha256=asset.GetNamedString(L"sha256"); require(update.sha256.size()==64 && update.sha256.find_first_not_of(L"0123456789abcdef")==std::wstring::npos);
    return update;
}
bool updateFileMatches(const std::filesystem::path& file,const std::wstring& expected) {
    std::error_code error; auto size=std::filesystem::file_size(file,error); if(error || size>50*1024*1024) return false;
    std::ifstream input(file,std::ios::binary); std::vector<unsigned char> data((std::istreambuf_iterator<char>(input)),{});
    return data.size()==size && hash(data)==expected;
}
UpdateResult downloadUpdate() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment {~Apartment(){winrt::uninit_apartment();}} apartment;
        bool missing=false; auto data=fetch(L"https://api.github.com/repos/voltura/voltura-books/releases/latest",256*1024,&missing);
        if(missing) return {L"No published release is available yet.",{}, {}};
        auto json=winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(std::string(data.begin(),data.end())));
        require(!json.GetNamedBoolean(L"draft") && !json.GetNamedBoolean(L"prerelease"));
        std::wstring tag(json.GetNamedString(L"tag_name").c_str()); require(!tag.empty() && tag[0]==L'v');
        auto next=tag.substr(1); if(!newerVersion(next,BOOKS_VERSION)) return {L"You're using the latest version.",{}, {}};
        auto base=L"https://github.com/voltura/voltura-books/releases/download/"+tag+L"/";
        auto manifest=fetch(base+L"VolturaBooks-Update-"+next+L".json",65536);
        auto signature=fetch(base+L"VolturaBooks-Update-"+next+L".sig",1024);
        auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(IDR_UPDATE_KEY),RT_RCDATA); require(resource!=nullptr);
        auto loaded=LoadResource(nullptr,resource); auto key=static_cast<const char*>(LockResource(loaded)); require(key!=nullptr);
        auto verified=verifyUpdate(std::string(manifest.begin(),manifest.end()),signature,std::string(key,SizeofResource(nullptr,resource)),BOOKS_VERSION);
        require(verified.version==next); auto installer=fetch(base+verified.name,verified.size);
        require(installer.size()==verified.size && hash(installer)==verified.sha256);
        PWSTR folder=nullptr; require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder)));
        auto destination=std::filesystem::path(folder)/L"Voltura Books"/L"Updates"; CoTaskMemFree(folder);
        std::filesystem::create_directories(destination); destination/=verified.name;
        std::ofstream output(destination,std::ios::binary|std::ios::trunc); output.write(reinterpret_cast<const char*>(installer.data()),installer.size()); output.close(); require(output.good());
        return {L"Version "+next+L" is ready to install.",destination,verified.sha256};
    } catch(...) { return {L"Couldn't check or download the update. Please try again or visit the website.",{}, {}}; }
}
}
