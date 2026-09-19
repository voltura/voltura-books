#include "cover.h"
#include "formats.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <shcore.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <miniz.h>
#include <wincodec.h>
#include <xmllite.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace books {
using Microsoft::WRL::ComPtr;
namespace {
using Bytes = std::vector<BYTE>;
using Attributes = std::map<std::wstring, std::wstring>;
struct Zip {
    mz_zip_archive archive{};
    FILE* file = nullptr;
    ~Zip() { if (archive.m_pState) mz_zip_reader_end(&archive); if (file) fclose(file); }
    Bytes read(const std::string& path, size_t limit) {
        int index = mz_zip_reader_locate_file(&archive, path.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
        mz_zip_archive_file_stat entry{};
        if (index < 0 || !mz_zip_reader_file_stat(&archive, index, &entry) || entry.m_uncomp_size > limit || !entry.m_uncomp_size) return {};
        Bytes bytes(static_cast<size_t>(entry.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&archive, index, bytes.data(), bytes.size(), 0)) return {};
        return bytes;
    }
};
bool xml(const Bytes& data, const std::function<void(const std::wstring&, const Attributes&)>& visit) {
    if (data.empty()) return false;
    ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(data.data(), static_cast<UINT>(data.size())));
    ComPtr<IXmlReader> reader;
    if (!stream || FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr))) return false;
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64);
    if (FAILED(reader->SetInput(stream.Get()))) return false;
    XmlNodeType type; HRESULT hr;
    while ((hr = reader->Read(&type)) == S_OK) {
        if (type != XmlNodeType_Element) continue;
        const wchar_t* raw = nullptr;
        if (FAILED(reader->GetLocalName(&raw, nullptr))) return false;
        std::wstring name(raw); Attributes attrs;
        if (reader->MoveToFirstAttribute() == S_OK) {
            do {
                const wchar_t *key = nullptr, *value = nullptr;
                if (FAILED(reader->GetLocalName(&key, nullptr)) || FAILED(reader->GetValue(&value, nullptr))) return false;
                attrs[key] = value;
            } while (reader->MoveToNextAttribute() == S_OK);
            reader->MoveToElement();
        }
        visit(name, attrs);
    }
    return hr == S_FALSE;
}
std::wstring attr(const Attributes& a, const wchar_t* key) {
    auto it = a.find(key); return it == a.end() ? L"" : it->second;
}
std::string utf8(const std::wstring& s) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, 0);
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), count, nullptr, nullptr);
    return result;
}
// Resolve archive-relative URI paths only. Nothing is extracted to disk or fetched.
std::string resolve(const std::string& base, const std::wstring& href) {
    auto ref = utf8(href);
    ref = ref.substr(0, ref.find_first_of("?#"));
    std::string decoded;
    auto hex = [](char c) -> int { if(c >= '0' && c <= '9') return c-'0'; if(c >= 'a' && c <= 'f') return c-'a'+10; if(c >= 'A' && c <= 'F') return c-'A'+10; return -1; };
    for (size_t i=0; i<ref.size(); ++i) {
        if (ref[i]=='%' && i+2<ref.size() && hex(ref[i+1])>=0 && hex(ref[i+2])>=0) { decoded += static_cast<char>(hex(ref[i+1])*16+hex(ref[i+2])); i+=2; }
        else decoded += ref[i];
    }
    if (decoded.empty() || decoded.front()=='/' || decoded.find_first_of(":\\")!=std::string::npos || decoded.find('\0')!=std::string::npos) return {};
    auto slash=base.find_last_of('/');
    std::istringstream input((slash==std::string::npos ? "" : base.substr(0,slash+1))+decoded);
    std::vector<std::string> parts; std::string part;
    while(std::getline(input,part,'/')) {
        if(part.empty() || part==".") continue;
        if(part=="..") { if(parts.empty()) return {}; parts.pop_back(); }
        else parts.push_back(part);
    }
    std::string path;
    for(const auto& p:parts) { if(!path.empty()) path+='/'; path+=p; }
    return path;
}
Bytes coverBytes(Zip& zip,std::string* selectedPath=nullptr) {
    constexpr size_t XmlLimit=1024*1024, ImageLimit=8*1024*1024;
    std::string package;
    if (!xml(zip.read("META-INF/container.xml",XmlLimit), [&](const auto& name,const auto& a) {
        if(name==L"rootfile" && package.empty()) package=resolve("",attr(a,L"full-path"));
    }) || package.empty()) return {};
    struct Item { std::wstring id, href, type, properties; };
    std::vector<Item> items; std::wstring coverId, guide;
    if(!xml(zip.read(package,XmlLimit),[&](const auto& name,const auto& a) {
        if(name==L"meta" && attr(a,L"name")==L"cover") coverId=attr(a,L"content");
        if(name==L"item") items.push_back({attr(a,L"id"),attr(a,L"href"),attr(a,L"media-type"),attr(a,L"properties")});
        if(name==L"reference" && attr(a,L"type")==L"cover") guide=attr(a,L"href");
    })) return {};
    auto getImage=[&](const std::wstring& href) { auto path=resolve(package,href);auto bytes=zip.read(path,ImageLimit);if(selectedPath&&!bytes.empty())*selectedPath=path;return bytes; };
    for(const auto& item:items) {
        std::wistringstream properties(item.properties); std::wstring token;
        while(properties>>token) if(token==L"cover-image") { auto bytes=getImage(item.href); if(!bytes.empty()) return bytes; }
    }
    for(const auto& item:items) if(!coverId.empty() && item.id==coverId) { auto bytes=getImage(item.href); if(!bytes.empty()) return bytes; }
    if(!guide.empty()) {
        auto page=resolve(package,guide); std::wstring image;
        xml(zip.read(page,XmlLimit),[&](const auto& name,const auto& a) {
            if(image.empty() && name==L"img") image=attr(a,L"src");
            if(image.empty() && name==L"image") image=attr(a,L"href");
        });
        if(!image.empty()){auto path=resolve(page,image);auto bytes=zip.read(path,ImageLimit);if(selectedPath&&!bytes.empty())*selectedPath=path;return bytes;}
    }
    return {};
}
}
BookDetails loadBookDetails(const std::filesystem::path& file) noexcept {
    BookDetails details;
    try {
        auto format=fileFormat(file.extension().wstring()); if(!format) return details;
        // Cached/fast Windows properties only: never start an external editor.
        ComPtr<IPropertyStore> properties;
        if(SUCCEEDED(SHGetPropertyStoreFromParsingName(file.c_str(),nullptr,GPS_FASTPROPERTIESONLY,IID_PPV_ARGS(&properties)))) {
            auto read=[&](REFPROPERTYKEY key) {
                PROPVARIANT value{}; std::wstring result;
                if(SUCCEEDED(properties->GetValue(key,&value))) {
                    PWSTR text=nullptr;
                    if(SUCCEEDED(PropVariantToStringAlloc(value,&text))) { result=text; CoTaskMemFree(text); }
                }
                PropVariantClear(&value); return result;
            };
            details.title=read(PKEY_Title); details.author=read(PKEY_Author);
        }
        if(std::wstring(format->extension)==L".pdf") {
            auto storage=winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(file.wstring()).get();
            details.pages=winrt::Windows::Data::Pdf::PdfDocument::LoadFromFileAsync(storage).get().PageCount();
            return details;
        }
        if(std::wstring(format->extension)!=L".epub" && std::wstring(format->extension)!=L".docx") return details;
        Zip zip; zip.file=_wfopen(file.c_str(),L"rb");
        if(!zip.file || !mz_zip_reader_init_cfile(&zip.archive,zip.file,0,0) || mz_zip_reader_get_num_files(&zip.archive)>10000) return details;
        auto readText=[&](const Bytes& bytes,bool pageCount) {
            if(bytes.empty()) return;
            ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(bytes.data(),static_cast<UINT>(bytes.size())));
            ComPtr<IXmlReader> reader;
            if(!stream || FAILED(CreateXmlReader(IID_PPV_ARGS(&reader),nullptr))) return;
            reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
            reader->SetProperty(XmlReaderProperty_MaxElementDepth,64);
            if(FAILED(reader->SetInput(stream.Get()))) return;
            XmlNodeType type; std::wstring current,value;
            auto apply=[&] {
                if(current==L"title" && !value.empty()) details.title=value;
                if(current==L"creator" && !value.empty()) { if(!details.author.empty()) details.author+=L", "; details.author+=value; }
                if(current==L"publisher" && !value.empty()) details.publisher=value;
                if(current==L"Pages" && pageCount) { try { auto pages=std::stoul(value); if(pages && pages<1000000) { details.pages=static_cast<unsigned>(pages); details.savedPageCount=true; } } catch(...) {} }
                current.clear(); value.clear();
            };
            while(reader->Read(&type)==S_OK) {
                const wchar_t* text=nullptr;
                if(type==XmlNodeType_Element) {
                    reader->GetLocalName(&text,nullptr);
                    if(text && (wcscmp(text,L"title")==0 || wcscmp(text,L"creator")==0 || wcscmp(text,L"publisher")==0 || (pageCount && wcscmp(text,L"Pages")==0))) current=text;
                } else if((type==XmlNodeType_Text || type==XmlNodeType_CDATA) && !current.empty()) {
                    UINT length=0; if(SUCCEEDED(reader->GetValue(&text,&length)) && value.size()<512) value.append(text,(std::min)(static_cast<size_t>(length),512-value.size()));
                } else if(type==XmlNodeType_EndElement && !current.empty()) apply();
            }
        };
        if(std::wstring(format->extension)==L".epub") {
            std::string package;
            xml(zip.read("META-INF/container.xml",1024*1024),[&](const auto& name,const auto& attrs){if(name==L"rootfile" && package.empty()) package=resolve("",attr(attrs,L"full-path"));});
            if(!package.empty()) { details.author.clear(); readText(zip.read(package,1024*1024),false); }
        } else {
            details.author.clear(); readText(zip.read("docProps/core.xml",1024*1024),false);
            readText(zip.read("docProps/app.xml",1024*1024),true);
        }
    } catch(...) {}
    return details;
}

static HBITMAP loadImage(const std::filesystem::path& epub, int width, int height, unsigned pageIndex) noexcept {
    try {
        if(width<=0 || height<=0 || width>16384 || height>16384 || static_cast<uint64_t>(width)*height>64000000) return nullptr;
        auto format=fileFormat(epub.extension().wstring()); if(!format) return nullptr;
        ComPtr<IWICImagingFactory> factory;
        if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))) return nullptr;
        ComPtr<IStream> stream; ComPtr<IWICBitmapDecoder> decoder;
        Bytes bytes;
        if(std::string(format->mime).starts_with("image/")) {
            if(FAILED(factory->CreateDecoderFromFilename(epub.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))) return nullptr;
        } else if(std::wstring(format->extension)==L".pdf") {
            using namespace winrt::Windows;
            auto file=Storage::StorageFile::GetFileFromPathAsync(epub.wstring()).get();
            auto document=Data::Pdf::PdfDocument::LoadFromFileAsync(file).get();
            if(pageIndex>=document.PageCount() || document.IsPasswordProtected()) return nullptr;
            auto page=document.GetPage(pageIndex); auto size=page.Size();
            if(size.Width<=0 || size.Height<=0) return nullptr;
            const double scale=(std::min)(width/size.Width,height/size.Height);
            Data::Pdf::PdfPageRenderOptions options;
            options.DestinationWidth((std::max)(1u,static_cast<unsigned>(size.Width*scale)));
            options.DestinationHeight((std::max)(1u,static_cast<unsigned>(size.Height*scale)));
            Storage::Streams::InMemoryRandomAccessStream rendered;
            page.RenderToStreamAsync(rendered,options).get(); rendered.Seek(0); page.Close();
            if(FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(rendered),IID_PPV_ARGS(&stream)))) return nullptr;
        } else if(std::wstring(format->extension)==L".epub" || std::wstring(format->extension)==L".docx") {
            Zip zip; zip.file=_wfopen(epub.c_str(),L"rb");
            if(!zip.file || !mz_zip_reader_init_cfile(&zip.archive,zip.file,0,0) || mz_zip_reader_get_num_files(&zip.archive)>10000) return nullptr;
            if(std::wstring(format->extension)==L".epub") bytes=coverBytes(zip);
            else {
                for(auto name:{"docProps/thumbnail.jpeg","docProps/thumbnail.png","docProps/thumbnail.jpg"}) {
                    bytes=zip.read(name,8*1024*1024); if(!bytes.empty()) break;
                }
            }
            if(bytes.empty()) return nullptr;
            stream.Attach(SHCreateMemStream(bytes.data(),static_cast<UINT>(bytes.size())));
        } else return nullptr;
        if(!decoder && (!stream || FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder)))) return nullptr;
        ComPtr<IWICBitmapFrameDecode> frame; UINT w=0,h=0;
        if(FAILED(decoder->GetFrame(0,&frame)) || FAILED(frame->GetSize(&w,&h)) || !w || !h || static_cast<uint64_t>(w)*h>64000000) return nullptr;
        bool widthLimited=static_cast<uint64_t>(width)*h<=static_cast<uint64_t>(height)*w;
        UINT tw=widthLimited ? width : (std::max)(1u,static_cast<UINT>(static_cast<uint64_t>(w)*height/h));
        UINT th=widthLimited ? (std::max)(1u,static_cast<UINT>(static_cast<uint64_t>(h)*width/w)) : height;
        ComPtr<IWICBitmapScaler> scaler; ComPtr<IWICFormatConverter> converter;
        if(FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(),tw,th,WICBitmapInterpolationModeFant)) || FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) return nullptr;
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=tw; info.bmiHeader.biHeight=-static_cast<LONG>(th); info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;
        HBITMAP bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap) return nullptr;
        if(FAILED(converter->CopyPixels(nullptr,tw*4,tw*th*4,static_cast<BYTE*>(pixels)))) { DeleteObject(bitmap); return nullptr; }
        if(std::wstring(format->extension)==L".docx") {
            // Some document generators package an empty white-page thumbnail.
            // Keep the file-type fallback instead of presenting that as content.
            const auto data=static_cast<const BYTE*>(pixels);
            bool blank=true;
            for(size_t i=0;i<static_cast<size_t>(tw)*th;++i) {
                const auto pixel=data+i*4;
                const int background=255-pixel[3]; // Premultiplied alpha over white.
                if(pixel[0]+background<250 || pixel[1]+background<250 || pixel[2]+background<250) { blank=false; break; }
            }
            if(blank) { DeleteObject(bitmap); return nullptr; }
        }
        return bitmap;
    } catch(...) { return nullptr; }
}
HBITMAP loadCover(const std::filesystem::path& file,int width,int height) noexcept {
    auto format=fileFormat(file.extension().wstring());
    const bool image=format&&std::string(format->mime).starts_with("image/");
    // Image previews also serve fullscreen monitors. loadImage enforces the
    // bounded dimensions and decoded-pixel budget for those larger requests.
    if(!image&&(width>2048 || height>2048)) return nullptr;
    return loadImage(file,width,height,0);
}
HBITMAP loadPdfPage(const std::filesystem::path& file,unsigned page,int width,int height) noexcept {
    return loadImage(file,width,height,page);
}
std::shared_ptr<EpubResources> loadEpubResources(const std::filesystem::path& file) noexcept {
    try {
        if(std::filesystem::file_size(file)>50000000) return {};
        Zip zip; zip.file=_wfopen(file.c_str(),L"rb");
        if(!zip.file || !mz_zip_reader_init_cfile(&zip.archive,zip.file,0,0)) return {};
        auto count=mz_zip_reader_get_num_files(&zip.archive);
        if(!count || count>10000) return {};
        auto result=std::make_shared<EpubResources>(); size_t total=0;
        for(unsigned i=0;i<count;++i) {
            mz_zip_archive_file_stat entry{};
            if(!mz_zip_reader_file_stat(&zip.archive,i,&entry) || entry.m_is_encrypted || mz_zip_reader_get_filename(&zip.archive,i,nullptr,0)>sizeof(entry.m_filename)) return {};
            if(entry.m_is_directory) continue;
            std::string name=entry.m_filename;
            if(name.empty() || name.front()=='/' || name.find_first_of("\\:")!=std::string::npos || name.find("../")!=std::string::npos || name==".." || result->files.contains(name)) return {};
            if(name=="META-INF/encryption.xml") return {}; // No DRM or obfuscated fonts in this reader.
            if(entry.m_uncomp_size>16*1024*1024 || total+entry.m_uncomp_size>128*1024*1024) return {};
            total+=static_cast<size_t>(entry.m_uncomp_size);
            auto& bytes=result->files[name]; bytes.resize(static_cast<size_t>(entry.m_uncomp_size));
            if(!bytes.empty() && !mz_zip_reader_extract_to_mem(&zip.archive,i,bytes.data(),bytes.size(),0)) return {};
        }
        auto container=result->files.find("META-INF/container.xml");
        if(container==result->files.end() || !xml(container->second,[&](const auto& name,const auto& a){if(name==L"rootfile" && result->package.empty()) result->package=resolve("",attr(a,L"full-path"));})) return {};
        auto package=result->files.find(result->package);
        bool spine=false;
        if(package==result->files.end() || !xml(package->second,[&](const auto& name,const auto&){if(name==L"itemref") spine=true;}) || !spine) return {};
        coverBytes(zip,&result->coverImage);
        return result;
    } catch(...) { return {}; }
}
}
