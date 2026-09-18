#include "history.h"
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <cwctype>
#include <vector>

namespace books {
namespace {
struct HashContext {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~HashContext() { if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
};
std::string field(const std::wstring& input) {
    auto value=utf8(input);
    std::string escaped;
    constexpr char hex[]="0123456789ABCDEF";
    for(unsigned char c:value) {
        if(c=='%' || c<32 || c==127) { escaped+='%'; escaped+=hex[c>>4]; escaped+=hex[c&15]; }
        else escaped+=static_cast<char>(c);
    }
    return escaped;
}
std::string key(const std::string& hash, std::wstring recipient) {
    for(auto& c:recipient) c=static_cast<wchar_t>(towlower(c));
    return hash+'\t'+field(recipient)+'\t';
}
}
BookIdentity::BookIdentity(const fs::path& path) {
    file=CreateFileW(fs::absolute(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Could not read the book to check sent history.");
    try {
        std::vector<UCHAR> object;
        HashContext context;
        DWORD size=0,received=0;
        if(BCryptOpenAlgorithmProvider(&context.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
           BCryptGetProperty(context.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&received,0)<0)
            throw std::runtime_error("Could not initialize the sent-book check.");
        object.resize(size);
        if(BCryptCreateHash(context.algorithm,&context.hash,object.data(),size,nullptr,0,0)<0)
            throw std::runtime_error("Could not initialize the sent-book check.");
        std::array<UCHAR,65536> buffer{};
        for(;;) {
            DWORD count=0;
            if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr))
                throw std::runtime_error("Could not read the book to check sent history.");
            if(!count) break;
            if(BCryptHashData(context.hash,buffer.data(),count,0)<0) throw std::runtime_error("Could not identify the book.");
        }
        std::array<UCHAR,32> digest{};
        if(BCryptFinishHash(context.hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("Could not identify the book.");
        constexpr char hex[]="0123456789abcdef";
        for(auto b:digest) { hash+=hex[b>>4]; hash+=hex[b&15]; }
    } catch(...) { CloseHandle(file); file=INVALID_HANDLE_VALUE; throw; }
}
BookIdentity::~BookIdentity() { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); }
bool sentBefore(const fs::path& history, const std::string& hash, const std::wstring& recipient) {
    if(!fs::exists(history)) return false;
    std::ifstream input(history,std::ios::binary);
    if(!input) throw std::runtime_error("Could not read sent-book history.");
    auto prefix=key(hash,recipient);
    std::string line;
    while(std::getline(input,line)) {
        // Ignore a final partial record if a previous process stopped mid-write.
        if(!input.eof() && line.starts_with(prefix) && line.ends_with("\taccepted")) return true;
    }
    if(input.bad()) throw std::runtime_error("Could not read sent-book history.");
    return false;
}
void recordSent(const fs::path& history, const std::string& hash, const std::wstring& recipient, const std::wstring& filename) {
    fs::create_directories(history.parent_path());
    SYSTEMTIME time{}; GetSystemTime(&time);
    char timestamp[32]{};
    sprintf_s(timestamp,"%04u-%02u-%02uT%02u:%02u:%02uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    // Leading newline separates a new record from any incomplete previous write.
    auto record='\n'+key(hash,recipient)+timestamp+'\t'+field(filename)+"\taccepted\n";
    HANDLE file=CreateFileW(history.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Could not save sent-book history.");
    DWORD written=0;
    bool ok=WriteFile(file,record.data(),static_cast<DWORD>(record.size()),&written,nullptr) && written==record.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if(!ok) throw std::runtime_error("Could not save sent-book history.");
}
}
