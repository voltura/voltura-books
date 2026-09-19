#include "reader_extract.h"
#include <miniz.h>
#include <objbase.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <io.h>
#include <fcntl.h>

namespace books {
namespace {
struct Archive {
    mz_zip_archive zip{};
    FILE* file=nullptr;
    ~Archive(){if(zip.m_pState)mz_zip_reader_end(&zip);if(file)fclose(file);}
};
std::shared_ptr<ReaderTextFile> temporaryText(){
    wchar_t folder[32768]{},name[32768]{};
    if(!GetTempPathW(32768,folder)||!GetTempFileNameW(folder,L"vbd",0,name))return {};
    auto result=std::make_shared<ReaderTextFile>();result->path=name;return result;
}
}
// Stream ZIP inflation to a temporary file, then XML text to UTF-16. Neither the
// uncompressed document nor a DOM is held in the application's address space.
std::shared_ptr<ReaderTextFile> extractDocxText(const std::filesystem::path& source,const std::function<bool()>& cancelled){
    try {
        const auto deadline=GetTickCount64()+30000;
        auto stopped=[&]{return cancelled()||GetTickCount64()>deadline;};
        if(stopped())return {};
        Archive archive;
        auto input=CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        if(input==INVALID_HANDLE_VALUE)return {};
        int descriptor=_open_osfhandle(reinterpret_cast<intptr_t>(input),_O_RDONLY|_O_BINARY);
        if(descriptor<0){CloseHandle(input);return {};}
        archive.file=_fdopen(descriptor,"rb");if(!archive.file){_close(descriptor);return {};}
        if(!archive.file||!mz_zip_reader_init_cfile(&archive.zip,archive.file,0,0)||mz_zip_reader_get_num_files(&archive.zip)>10000)return {};
        int index=mz_zip_reader_locate_file(&archive.zip,"word/document.xml",nullptr,MZ_ZIP_FLAG_CASE_SENSITIVE);
        mz_zip_archive_file_stat entry{};
        constexpr uint64_t limit=128ull*1024*1024;
        if(index<0||!mz_zip_reader_file_stat(&archive.zip,index,&entry)||entry.m_is_encrypted||entry.m_uncomp_size>limit)return {};
        auto xmlFile=temporaryText(),result=temporaryText();if(!xmlFile||!result)return {};
        std::ofstream xmlOutput(xmlFile->path,std::ios::binary|std::ios::trunc);
        struct Sink{std::ofstream& output;std::function<bool()> stopped;uint64_t bytes=0;} sink{xmlOutput,stopped};
        auto write=[](void* context,mz_uint64 offset,const void* data,size_t size)->size_t{
            auto& sink=*static_cast<Sink*>(context);
            if(sink.stopped()||offset!=sink.bytes||size>128ull*1024*1024-sink.bytes)return 0;
            sink.output.write(static_cast<const char*>(data),size);sink.bytes+=size;return sink.output?size:0;
        };
        if(!mz_zip_reader_extract_to_callback(&archive.zip,index,write,&sink,0))return {};
        xmlOutput.close();if(!xmlOutput||stopped())return {};
        Microsoft::WRL::ComPtr<IStream> stream;
        Microsoft::WRL::ComPtr<IXmlReader> reader;
        if(FAILED(SHCreateStreamOnFileEx(xmlFile->path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&stream))||
           FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(reader.GetAddressOf()),nullptr)))return {};
        reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
        reader->SetProperty(XmlReaderProperty_MaxElementDepth,128);
        if(FAILED(reader->SetInput(stream.Get())))return {};
        std::ofstream output(result->path,std::ios::binary|std::ios::trunc);
        uint64_t bytes=0;bool wrote=false,inText=false,sawDocument=false;
        auto emit=[&](const wchar_t* value,size_t length){bytes+=length*sizeof(wchar_t);if(bytes>limit)return false;output.write(reinterpret_cast<const char*>(value),length*sizeof(wchar_t));return static_cast<bool>(output);};
        if(!emit(L"\xfeff",1))return {};
        XmlNodeType type;HRESULT hr;
        while((hr=reader->Read(&type))==S_OK){
            if(stopped())return {};
            if(type==XmlNodeType_Element||type==XmlNodeType_EndElement){
                const wchar_t *name=nullptr,*ns=nullptr;reader->GetLocalName(&name,nullptr);reader->GetNamespaceUri(&ns,nullptr);
                bool word=ns&&(wcscmp(ns,L"http://schemas.openxmlformats.org/wordprocessingml/2006/main")==0||wcscmp(ns,L"http://purl.oclc.org/ooxml/wordprocessingml/main")==0);
                if(word&&wcscmp(name,L"document")==0)sawDocument=true;
                inText=word&&wcscmp(name,L"t")==0&&type==XmlNodeType_Element&&!reader->IsEmptyElement();
                if(word&&((type==XmlNodeType_EndElement&&(wcscmp(name,L"p")==0||wcscmp(name,L"tr")==0))||(type==XmlNodeType_Element&&wcscmp(name,L"br")==0))){if(!emit(L"\n",1))return {};}
                if(word&&((type==XmlNodeType_Element&&wcscmp(name,L"tab")==0)||(type==XmlNodeType_EndElement&&wcscmp(name,L"tc")==0))){if(!emit(L"\t",1))return {};}
            }else if(inText&&(type==XmlNodeType_Text||type==XmlNodeType_Whitespace||type==XmlNodeType_CDATA)){
                wchar_t buffer[4096];UINT count=0;HRESULT valueHr;
                do{valueHr=reader->ReadValueChunk(buffer,4096,&count);if(FAILED(valueHr)||stopped()||!emit(buffer,count))return {};wrote|=count>0;}while(count);
            }
        }
        output.flush();if(hr!=S_FALSE||!sawDocument||!wrote||!output||stopped())return {};
        output.close();return result;
    }catch(...){return {};}
}
}
