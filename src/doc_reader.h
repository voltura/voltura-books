#pragma once
#include "reader_process.h"
#include <objbase.h>
#include <memory>
#include <wrl.h>

namespace books {
// Shared by the session, worker and response streams. Windows removes storage
// even on process exit; every consumer opens with FILE_SHARE_DELETE.
struct ReaderDocument {
    std::filesystem::path path;
    ReaderHandle file;
    static std::shared_ptr<ReaderDocument> create() {
        wchar_t folder[32768]{},name[40]{};GUID id{};
        if(!GetTempPathW(32768,folder)||FAILED(CoCreateGuid(&id)))return {};
        StringFromGUID2(id,name,40);
        auto result=std::make_shared<ReaderDocument>();result->path=std::filesystem::path(folder)/(std::wstring(L"VolturaBooks-")+name+L".docx");
        result->file.reset(CreateFileW(result->path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
        return result->file.value==INVALID_HANDLE_VALUE?nullptr:result;
    }
};
struct DocConversion {
    std::shared_ptr<ReaderDocument> document;
    DWORD error=4;
};
inline DocConversion convertDoc(const std::filesystem::path& source,const std::function<bool()>& cancelled,
    DWORD timeout=30000,uint64_t limit=128ull*1024*1024,uint64_t budget=readerMemoryBudget(),const wchar_t* helper=L"VolturaBooksDoc.exe") {
    DocConversion result;auto output=ReaderDocument::create();ReaderProcess process;
    if(cancelled()||!output||!process.start(source,L"",budget,helper))return result;
    const auto deadline=GetTickCount64()+timeout;uint64_t total=0;
    for(;;){
        if(cancelled())return result;
        if(GetTickCount64()>=deadline){result.error=6;return result;}
        if(readerMemoryPressure()){result.error=7;return result;}
        DWORD available=0;
        bool pipe=PeekNamedPipe(process.output.value,nullptr,0,nullptr,&available,nullptr)!=FALSE;
        if(pipe&&available){
            BYTE buffer[65536];DWORD count=0,written=0;
            if(!ReadFile(process.output.value,buffer,(std::min)(available,static_cast<DWORD>(sizeof(buffer))),&count,nullptr)||!count)return result;
            if(count>limit-total){result.error=5;return result;}
            if(!WriteFile(output->file.value,buffer,count,&written,nullptr)||written!=count)return result;
            total+=count;continue;
        }
        if(WaitForSingleObject(process.process.value,10)==WAIT_OBJECT_0){
            // Exit may race the preceding empty peek. Drain the final ZIP
            // directory before interpreting a successful exit as completion.
            if(PeekNamedPipe(process.output.value,nullptr,0,nullptr,&available,nullptr)&&available)continue;
            DWORD code=4;GetExitCodeProcess(process.process.value,&code);result.error=code;
            if(code==0&&total>4){FlushFileBuffers(output->file.value);result.document=std::move(output);}
            return result;
        }
    }
}
// File-backed WebView response with delete-sharing and explicit session lifetime.
class ReaderDocumentStream final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IStream> {
    std::shared_ptr<ReaderDocument> document;
    ReaderHandle file;
public:
    HRESULT open(std::shared_ptr<ReaderDocument> source){document=std::move(source);file.reset(CreateFileW(document->path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));return file.value==INVALID_HANDLE_VALUE?HRESULT_FROM_WIN32(GetLastError()):S_OK;}
    HRESULT STDMETHODCALLTYPE Read(void* data,ULONG size,ULONG* read) override{DWORD count=0;if(!ReadFile(file.value,data,size,&count,nullptr))return STG_E_READFAULT;if(read)*read=count;return count==size?S_OK:S_FALSE;}
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER offset,DWORD origin,ULARGE_INTEGER* position) override{LARGE_INTEGER next{};if(!SetFilePointerEx(file.value,offset,&next,origin))return STG_E_SEEKERROR;if(position)position->QuadPart=next.QuadPart;return S_OK;}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat,DWORD) override{if(!stat)return E_POINTER;*stat={};LARGE_INTEGER size{};if(!GetFileSizeEx(file.value,&size))return STG_E_READFAULT;stat->type=STGTY_STREAM;stat->cbSize.QuadPart=size.QuadPart;return S_OK;}
    HRESULT STDMETHODCALLTYPE Clone(IStream** next) override{if(!next)return E_POINTER;*next=nullptr;auto clone=Microsoft::WRL::Make<ReaderDocumentStream>();auto hr=clone->open(document);if(FAILED(hr))return hr;LARGE_INTEGER zero{};ULARGE_INTEGER pos{};hr=Seek(zero,FILE_CURRENT,&pos);if(FAILED(hr))return hr;zero.QuadPart=pos.QuadPart;hr=clone->Seek(zero,FILE_BEGIN,nullptr);if(FAILED(hr))return hr;return clone.CopyTo(next);}
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG*) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* destination,ULARGE_INTEGER amount,ULARGE_INTEGER* read,ULARGE_INTEGER* written) override{
        if(read)read->QuadPart=0;if(written)written->QuadPart=0;if(!destination)return E_POINTER;
        BYTE buffer[65536];uint64_t remaining=amount.QuadPart;
        while(remaining){ULONG count=0,sent=0;auto hr=Read(buffer,static_cast<ULONG>((std::min)(remaining,static_cast<uint64_t>(sizeof(buffer)))),&count);if(FAILED(hr))return hr;
            if(read)read->QuadPart+=count;auto write=destination->Write(buffer,count,&sent);if(written)written->QuadPart+=sent;
            if(FAILED(write))return write;if(sent!=count)return STG_E_MEDIUMFULL;remaining-=count;if(hr==S_FALSE)return S_FALSE;
        }return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Revert() override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return E_NOTIMPL;}
};
}
