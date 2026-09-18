#include "update.h"
#include <winrt/base.h>
#include <fstream>
#include <iostream>
std::string read(const std::filesystem::path& p) { std::ifstream f(p,std::ios::binary); return {(std::istreambuf_iterator<char>(f)),{}}; }
int wmain(int argc,wchar_t** argv) {
    winrt::init_apartment();
    if(argc!=4) return 2;
    try {
        if(!books::newerVersion(L"1.10.0",L"1.9.9") || books::newerVersion(L"1.0.0",L"1.0.0")) return 3;
        for(auto invalid:{L"01.0.0",L"1.2",L"1.2.3.4",L"1.2.3-beta",L"999999999.0.0"}) {
            bool rejected=false; try{books::newerVersion(invalid,L"0.1.0");}catch(...){rejected=true;} if(!rejected) return 4;
        }
        auto manifest=read(argv[1]),sig=read(argv[2]),key=read(argv[3]);
        std::vector<unsigned char> signature(sig.begin(),sig.end());
        auto verified=books::verifyUpdate(manifest,signature,key,L"0.0.0");
        if(!books::newerVersion(verified.version,L"0.0.0")) return 5;
        bool rejected=false; try{books::verifyUpdate(manifest,signature,key,verified.version);}catch(...){rejected=true;} if(!rejected)return 6;
        rejected=false; try{books::verifyUpdate(manifest+" ",signature,key,L"0.0.0");}catch(...){rejected=true;} if(!rejected)return 7;
        signature[0]^=1; rejected=false; try{books::verifyUpdate(manifest,signature,key,L"0.0.0");}catch(...){rejected=true;} if(!rejected)return 8;
        std::cout<<"PASS: numeric versions, malformed versions, RSA-PSS verification, tampering, and downgrade rejection\n";
        return 0;
    } catch(...) { return 1; }
}
