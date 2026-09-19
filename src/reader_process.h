#pragma once
#include "reader_memory.h"
#include <filesystem>
#include <functional>
#include <vector>
#include <string>

namespace books {
struct ReaderHandle {
    HANDLE value=nullptr;
    ReaderHandle()=default;
    explicit ReaderHandle(HANDLE h):value(h){}
    ~ReaderHandle(){reset();}
    ReaderHandle(const ReaderHandle&)=delete;
    ReaderHandle& operator=(const ReaderHandle&)=delete;
    void reset(HANDLE next=nullptr){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=next;}
};
// Start every private renderer suspended, then contain the entire process tree
// before Windows or WebView2 can create child processes or parse document data.
class ReaderProcess {
public:
    ReaderHandle job,process,input,output;
    ~ReaderProcess(){close();}
    void close(){if(job.value)TerminateJobObject(job.value,1);process.reset();input.reset();output.reset();job.reset();}
    bool start(const std::filesystem::path& path,const std::wstring& mode=L"",uint64_t budget=readerMemoryBudget(),const wchar_t* executable=L"VolturaBooksReader.exe") {
        close();if(!budget)return false;
        job.reset(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit=static_cast<SIZE_T>(budget);
        if(!job.value||!SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return false;
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        ReaderHandle childInput,childOutput;
        if(!CreatePipe(&childInput.value,&input.value,&security,0)||!CreatePipe(&output.value,&childOutput.value,&security,0))return false;
        SetHandleInformation(input.value,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output.value,HANDLE_FLAG_INHERIT,0);
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<BYTE> attributes(size);
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&size))return false;
        HANDLE inherited[]={childInput.value,childOutput.value};
        bool configured=UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE;
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=childInput.value;startup.StartupInfo.hStdOutput=childOutput.value;startup.StartupInfo.hStdError=childOutput.value;
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        auto helper=std::filesystem::path(exe).parent_path()/executable;
        auto command=L"\""+helper.wstring()+L"\" "+mode+L"\""+path.wstring()+L"\"";
        PROCESS_INFORMATION child{};
        bool created=configured&&CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&child);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if(!created)return false;
        process.reset(child.hProcess);ReaderHandle thread(child.hThread);
        if(!AssignProcessToJobObject(job.value,process.value)){TerminateProcess(process.value,1);close();return false;}
        if(ResumeThread(thread.value)==static_cast<DWORD>(-1)){close();return false;}
        return true;
    }
    bool read(void* buffer,DWORD length,const std::function<bool()>& cancelled) {
        auto deadline=GetTickCount64()+30000;
        auto bytes=static_cast<BYTE*>(buffer);
        while(length) {
            if(cancelled()||readerMemoryPressure()||GetTickCount64()>deadline)return false;
            DWORD available=0,count=0;
            if(!PeekNamedPipe(output.value,nullptr,0,nullptr,&available,nullptr))return false;
            if(!available){if(WaitForSingleObject(process.value,10)!=WAIT_TIMEOUT)return false;continue;}
            if(!ReadFile(output.value,bytes,(std::min)(length,available),&count,nullptr)||!count)return false;
            bytes+=count;length-=count;
        }
        return true;
    }
};
}
