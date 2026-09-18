#include "drop.h"
namespace books {
std::vector<fs::path> droppedBooks(HDROP drop, bool release) {
    struct Release { HDROP value; bool owned; ~Release() { if(owned) DragFinish(value); } } cleanup{drop,release};
    const UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
    if(!count) throw std::runtime_error("Choose at least one book or document.");
    std::vector<fs::path> paths;
    for(UINT i=0;i<count;++i) {
        std::wstring selected(DragQueryFileW(drop,i,nullptr,0)+1,0);
        auto length=DragQueryFileW(drop,i,selected.data(),static_cast<UINT>(selected.size()));
        if(!length) throw std::runtime_error("Could not read the selected file. Try Choose files instead.");
        selected.resize(length);
        if(auto error=validateFile(selected); !error.empty()) throw std::runtime_error(utf8(fs::path(selected).filename().wstring()+L": "+error));
        paths.emplace_back(selected);
    }
    return paths;
}
}
