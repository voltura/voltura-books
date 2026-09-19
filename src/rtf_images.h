#pragma once
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

namespace books {
// The system Rich Edit accepts DIB pictures but drops PNG/JPEG RTF pictures.
// Normalize just those pictures inside the memory-limited helper. Everything
// else is copied through to a delete-on-close disk stream, never to a full-file
// string. The helper's job also bounds codec allocations.
class RtfImageStream {
    HANDLE file=INVALID_HANDLE_VALUE;
    std::vector<char> buffer;
    bool good=true;
    void flush(){DWORD written=0;if(!buffer.empty()&&(!WriteFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&written,nullptr)||written!=buffer.size()))good=false;buffer.clear();}
    void put(char c){buffer.push_back(c);if(buffer.size()==32768)flush();}
    void put(const std::string& value){for(char c:value)put(c);}
    bool picture(const std::vector<BYTE>& encoded,const std::map<std::string,int>& attributes){
        using Microsoft::WRL::ComPtr;
        if(encoded.empty()||encoded.size()>UINT_MAX)return false;
        ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(encoded.data(),static_cast<UINT>(encoded.size())));
        ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
        if(!stream||FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder))||FAILED(decoder->GetFrame(0,&frame)))return false;
        UINT width=0,height=0;if(FAILED(frame->GetSize(&width,&height))||!width||!height)return false;
        auto originalWidth=width,originalHeight=height;
        double scale=(std::min)(1.0,4096.0/(std::max)(width,height));width=(std::max)(1U,static_cast<UINT>(width*scale));height=(std::max)(1U,static_cast<UINT>(height*scale));
        ComPtr<IWICBitmapScaler> scaler;ComPtr<IWICFormatConverter> converter;
        if(FAILED(factory->CreateBitmapScaler(&scaler))||FAILED(scaler->Initialize(frame.Get(),width,height,WICBitmapInterpolationModeFant))||FAILED(factory->CreateFormatConverter(&converter))||FAILED(converter->Initialize(scaler.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
        UINT stride=(width*3+3)&~3U;std::vector<BYTE> pixels(static_cast<size_t>(stride)*height);
        if(FAILED(converter->CopyPixels(nullptr,stride,static_cast<UINT>(pixels.size()),pixels.data())))return false;
        BITMAPINFOHEADER header{sizeof(BITMAPINFOHEADER),static_cast<LONG>(width),static_cast<LONG>(height),1,24,BI_RGB,static_cast<DWORD>(pixels.size())};
        put("{\\pict\\dibitmap0\\picw"+std::to_string(width)+"\\pich"+std::to_string(height));
        auto goal=[&](const char* name,const char* scaling,UINT original){
            auto value=attributes.contains(name)?static_cast<int64_t>(attributes.at(name)):static_cast<int64_t>(original)*15;
            auto percent=attributes.contains(scaling)?attributes.at(scaling):100;
            return (std::max)(int64_t{1},value*percent/100);
        };
        // Bake the RTF picture scale into its display goals. Large unscaled
        // goals can overflow Rich Edit's picture sizing even at 20% scale.
        put("\\picwgoal"+std::to_string(goal("picwgoal","picscalex",originalWidth))+"\\pichgoal"+std::to_string(goal("pichgoal","picscaley",originalHeight))+"\\picscalex100\\picscaley100");
        for(auto name:{"piccropl","piccropr","piccropt","piccropb"}){
            auto it=attributes.find(name);if(it==attributes.end())continue;
            const char* axis=(name[7]=='l'||name[7]=='r')?"picscalex":"picscaley";
            auto percent=attributes.contains(axis)?attributes.at(axis):100;
            put(std::string("\\")+name+std::to_string(static_cast<int64_t>(it->second)*percent/100));
        }
        put(' ');constexpr char hex[]="0123456789abcdef";
        auto bytes=[&](const BYTE* p,size_t size){for(size_t i=0;i<size;++i){put(hex[p[i]>>4]);put(hex[p[i]&15]);}};
        bytes(reinterpret_cast<const BYTE*>(&header),sizeof(header));
        for(UINT y=height;y>0;--y)bytes(pixels.data()+static_cast<size_t>(y-1)*stride,stride);
        put('}');return good;
    }
public:
    int pageWidth=12240,pageHeight=15840; // Letter when the document omits paper dimensions.
    ~RtfImageStream(){close();}
    void close(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);file=INVALID_HANDLE_VALUE;}
    HANDLE get()const{return file;}
    bool prepare(const std::filesystem::path& path){
        wchar_t folder[32768]{},name[32768]{};if(!GetTempPathW(32768,folder)||!GetTempFileNameW(folder,L"vbi",0,name))return false;
        file=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(file==INVALID_HANDLE_VALUE){DeleteFileW(name);return false;}
        std::ifstream input(path,std::ios::binary);if(!input)return false;
        char signature[5]{};input.read(signature,5);if(input.gcount()!=5||memcmp(signature,"{\\rtf",5))return false;input.seekg(0);
        auto hex=[](int c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
        for(int c=input.get();c>=0&&good;c=input.get()){
            if(c=='{'){
                auto begin=input.tellg();std::string prefix;for(int i=0;i<32&&input;++i){int v=input.get();if(v<0)break;prefix+=static_cast<char>(v);if(prefix.find("\\pict")!=std::string::npos)break;}
                input.clear();input.seekg(begin);
                auto word=prefix.find_first_not_of(" \r\n\t");
                if(word!=std::string::npos&&prefix.substr(word)=="\\pict"){
                    bool compressed=false;int depth=1,nibble=-1;std::vector<BYTE> encoded;std::map<std::string,int> attributes;
                    for(int v=input.get();v>=0&&depth;v=input.get()){
                        if(v=='{'){++depth;continue;}if(v=='}'){if(--depth==0)break;continue;}
                        if(v=='\\'){
                            std::string control;int next=input.get();while(next>=0&&isalpha(static_cast<unsigned char>(next))){if(control.size()<32)control+=static_cast<char>(next);next=input.get();}
                            bool negative=next=='-';if(negative)next=input.get();int64_t number=0;
                            while(next>='0'&&next<='9'){number=number*10+next-'0';if(number>INT_MAX)return false;next=input.get();}if(negative)number=-number;
                            if(next>=0&&next!=' ')input.unget();
                            if(control=="bin"){if(number<0)return false;for(int64_t i=0;i<number;++i){int byte=input.get();if(byte<0)return false;if(compressed&&depth==1)encoded.push_back(static_cast<BYTE>(byte));}}
                            else if(depth==1){if(control=="pngblip"||control=="jpegblip")compressed=true;else if(control=="picwgoal"||control=="pichgoal"||control=="picscalex"||control=="picscaley"||control=="piccropl"||control=="piccropr"||control=="piccropt"||control=="piccropb")attributes[control]=static_cast<int>(number);}
                        }else if(compressed&&depth==1&&hex(v)>=0){if(nibble<0)nibble=hex(v);else{encoded.push_back(static_cast<BYTE>(nibble*16+hex(v)));nibble=-1;}}
                    }
                    if(depth||nibble>=0)return false;auto end=input.tellg();
                    if(compressed){if(!picture(encoded,attributes))return false;}
                    else {input.seekg(begin-std::streamoff(1));for(auto n=end-(begin-std::streamoff(1));n>0;--n)put(static_cast<char>(input.get()));}
                    continue;
                }
            }
            put(static_cast<char>(c));
            if(c=='\\'){
                // Escaped braces and binary runs are opaque to the group scanner.
                std::string control;int next=input.get();if(next<0)return false;
                while(isalpha(static_cast<unsigned char>(next))){if(control.size()<32)control+=static_cast<char>(next);put(static_cast<char>(next));next=input.get();if(next<0)return false;}
                if(control.empty()){put(static_cast<char>(next));continue;}
                bool negative=next=='-';if(negative){put('-');next=input.get();}int64_t number=0;
                while(next>='0'&&next<='9'){number=number*10+next-'0';if(number>INT_MAX)return false;put(static_cast<char>(next));next=input.get();}
                if(next==' ')put(' ');else if(next>=0)input.unget();
                if(!negative&&number>=1440&&number<=144000){if(control=="paperw")pageWidth=static_cast<int>(number);if(control=="paperh")pageHeight=static_cast<int>(number);}
                if(control=="bin"){if(negative)return false;for(int64_t i=0;i<number;++i){int byte=input.get();if(byte<0)return false;put(static_cast<char>(byte));}}
            }
        }
        if(input.bad())return false;flush();SetFilePointer(file,0,nullptr,FILE_BEGIN);return good;
    }
};
}
