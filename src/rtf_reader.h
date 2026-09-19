#pragma once
#include "reader_process.h"
#include "rtf_protocol.h"
#include <set>

namespace books {
class RtfReader {
    ReaderProcess renderer;
    std::filesystem::path document;
    std::set<int32_t> anchors;
    int width=0,height=0,dpi=0;
public:
    void close(){renderer.close();document.clear();anchors.clear();width=height=dpi=0;}
    bool open(const std::filesystem::path& path) {
        if(document==path&&renderer.process.value&&WaitForSingleObject(renderer.process.value,0)==WAIT_TIMEOUT)return true;
        close();if(!renderer.start(path))return false;document=path;return true;
    }
    HBITMAP render(const std::filesystem::path& path,RtfRequest request,RtfResponse& response,const std::function<bool()>& cancelled) {
        if(cancelled()||!open(path))return nullptr;
        if(width!=request.width||height!=request.height||dpi!=request.dpi){anchors.clear();width=request.width;height=request.height;dpi=request.dpi;}
        if(request.previous){auto before=anchors.lower_bound(request.anchor);if(before!=anchors.begin()){request.anchor=*std::prev(before);request.previous=0;}}
        DWORD written=0;
        if(!WriteFile(renderer.input.value,&request,sizeof(request),&written,nullptr)||written!=sizeof(request)||!renderer.read(&response,sizeof(response),cancelled)) {close();return nullptr;}
        if(response.width!=request.width||response.height!=request.height||response.anchor<0||response.next<=response.anchor||response.next>response.length||response.length<=0) {close();return nullptr;}
        const auto count=static_cast<uint64_t>(response.width)*response.height*4;
        if(response.width<=0||response.height<=0||count>64*1024*1024){close();return nullptr;}
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),response.width,-response.height,1,32,BI_RGB};
        void* pixels=nullptr;auto bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap||!renderer.read(pixels,static_cast<DWORD>(count),cancelled)){if(bitmap)DeleteObject(bitmap);close();return nullptr;}
        anchors.insert(response.anchor);if(anchors.size()>256)anchors.erase(anchors.begin());
        return bitmap;
    }
};
}
