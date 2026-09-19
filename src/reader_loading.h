#pragma once
#include <cstdint>

namespace books {
enum class ReaderOperation { None, Opening, Page, View };
// Clock is supplied by the owner so delays and stale results are testable
// without windows, sleeping, browser timing, or focus changes.
struct ReaderLoading {
    uint64_t identity=0,started=0;
    ReaderOperation kind=ReaderOperation::None;
    uint64_t begin(ReaderOperation next,uint64_t now){
        if(kind==ReaderOperation::Opening&&next==ReaderOperation::Opening)return identity;
        ++identity;kind=next;started=now;return identity;
    }
    bool finish(uint64_t token){if(token!=identity)return false;kind=ReaderOperation::None;return true;}
    void cancel(){++identity;kind=ReaderOperation::None;}
    bool visible(uint64_t now)const{return kind!=ReaderOperation::None&&(kind==ReaderOperation::Opening||now-started>=150);}
    bool expired(uint64_t now)const{return kind!=ReaderOperation::None&&now-started>=30000;}
    const wchar_t* text()const{return kind==ReaderOperation::Opening?L"Opening document…":kind==ReaderOperation::Page?L"Loading page…":L"Preparing view…";}
};
}
