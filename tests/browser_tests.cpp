#include "../src/browser.cpp"
#include <fstream>
#include <iostream>
static int mode=0,failures=0;
static void fail(int line) { ++failures; std::cerr<<"Failure at line "<<line<<" in mode "<<mode<<"\n"; }
static INT_PTR CALLBACK testProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_APP+90) {
        auto state=reinterpret_cast<books::Browser*>(GetWindowLongPtrW(window,DWLP_USER));
        if(state->files.size()!=3) fail(__LINE__);
        if(mode==0) {
            if(!state->allTypes || SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_GETCHECK,0,0)!=BST_CHECKED) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST,BM_CLICK,0,0);
            if(state->files.size()!=1 || state->allTypes) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+1,BM_CLICK,0,0);
            if(state->files.size()!=2) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST,BM_CLICK,0,0);
            if(state->files.size()!=1 || state->files[0].extension()!=L".pdf") fail(__LINE__);
            SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_CLICK,0,0);
            if(state->files.size()!=3 || !state->allTypes || state->enabled[1]) fail(__LINE__);
            if((GetWindowLongPtrW(GetDlgItem(window,IDC_TYPE_FIRST+6),GWL_STYLE)&WS_VISIBLE)) fail(__LINE__);
            if(books::groupOf(L"a.png")!=books::groupOf(L"b.jpg") || books::groupOf(L"a.gif")!=books::groupOf(L"b.bmp")) fail(__LINE__);
            auto original=state->allFiles;
            state->allFiles.push_back(state->initialFolder/L"Picture.png"); state->allFiles.push_back(state->initialFolder/L"Photo.jpg");
            books::applyFilters(window,*state);
            if(!(GetWindowLongPtrW(GetDlgItem(window,IDC_TYPE_FIRST+6),GWL_STYLE)&WS_VISIBLE)) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+6,BM_CLICK,0,0);
            if(state->files.size()!=2 || state->allTypes) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_TYPE_FIRST+6,BM_CLICK,0,0);
            if(!state->files.empty() || IsWindowEnabled(GetDlgItem(window,IDOK))) fail(__LINE__);
            state->allFiles=original; SendDlgItemMessageW(window,IDC_ALL_TYPES,BM_CLICK,0,0);
            SendDlgItemMessageW(window,IDC_VIEW_THUMBS,BM_CLICK,0,0);
            if(!state->tiles || ListView_GetItemCount(GetDlgItem(window,IDC_FILE_GRID))!=3) fail(__LINE__);
            RedrawWindow(GetDlgItem(window,IDC_FILE_GRID),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            ListView_SetItemState(GetDlgItem(window,IDC_FILE_GRID),1,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            if(SendDlgItemMessageW(window,IDC_FILE_LIST,LB_GETSEL,1,0)!=1) fail(__LINE__);
            ListView_SetSelectionMark(GetDlgItem(window,IDC_FILE_GRID),0);
            books::rebuildGrid(window,*state);
            if(ListView_GetNextItem(GetDlgItem(window,IDC_FILE_GRID),-1,LVNI_FOCUSED)!=1 || ListView_GetSelectionMark(GetDlgItem(window,IDC_FILE_GRID))!=0 || !(ListView_GetItemState(GetDlgItem(window,IDC_FILE_GRID),1,LVIS_SELECTED)&LVIS_SELECTED)) fail(__LINE__);
            SendDlgItemMessageW(window,IDC_VIEW_LIST,BM_CLICK,0,0);
            if(state->tiles) fail(__LINE__);
            SetDlgItemTextW(window,IDC_SEARCH,L"B.PDF");
            if(state->files.size()!=1 || state->files[0].extension()!=L".pdf") fail(__LINE__);
            SetDlgItemTextW(window,IDC_SEARCH,L"no match");
            if(!state->files.empty() || IsWindowEnabled(GetDlgItem(window,IDOK))) fail(__LINE__);
            SetDlgItemTextW(window,IDC_SEARCH,L"");
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,1,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            if(state->files[0].filename()!=L"C.rtf") fail(__LINE__);
            state->modified[state->initialFolder/L"A.epub"]=20;
            state->modified[state->initialFolder/L"B.pdf"]=30;
            state->modified[state->initialFolder/L"C.rtf"]=10;
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,3,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            if(state->files.front().filename()!=L"B.pdf" || state->files.back().filename()!=L"C.rtf") fail(__LINE__);
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,4,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            if(state->files.front().filename()!=L"C.rtf" || state->files.back().filename()!=L"B.pdf") fail(__LINE__);
            SendDlgItemMessageW(window,IDC_SORT,CB_SETCURSEL,0,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_SORT,CBN_SELCHANGE),0);
            books::BookDetails details; details.title=L"Example"; details.author=L"Author"; details.publisher=L"Publisher";
            auto text=books::bookInformationText(state->files[0],details);
            if(!(text.find(L"\nTitle:")<text.find(L"\nAuthor:") && text.find(L"\nAuthor:")<text.find(L"\nPublisher:") && text.find(L"\nPublisher:")<text.find(L"\nFormat:") && text.find(L"\nFormat:")<text.find(L"\nSize:"))) fail(__LINE__);
        }
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,FALSE,-1);
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,0);
        if(mode==1) SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETSEL,TRUE,2);
        SendDlgItemMessageW(window,IDC_FILE_LIST,LB_SETCARETINDEX,mode==1 ? 2 : 0,FALSE);
        books::selectFile(window,*state);
        if(state->current!=state->files[mode==1 ? 2 : 0]) fail(__LINE__);
        wchar_t label[100]{}; GetDlgItemTextW(window,IDOK,label,100);
        if(std::wstring(label)!=(mode==1 ? L"Send 2 books" : L"Send book")) fail(__LINE__);
        PostMessageW(window,WM_COMMAND,mode==2 ? IDCANCEL : IDOK,0); return TRUE;
    }
    auto result=books::proc(window,message,wp,lp);
    if(message==WM_INITDIALOG) PostMessageW(window,WM_APP+90,0,0);
    return result;
}
int wmain(int argc,wchar_t** argv) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    if(argc==2) { books::browseBooks(nullptr,argv[1]); return 0; } // Preview never sends.
    auto root=books::fs::temp_directory_path()/(L"VolturaBooks-browser-"+std::to_wstring(GetCurrentProcessId())); books::fs::create_directories(root);
    for(auto name:{L"A.epub",L"B.pdf",L"C.rtf",L"Ignored.exe"}) { std::ofstream f(root/name); f<<"fixture"; }
    for(mode=0;mode<3;++mode) {
        books::Browser state; state.initialFolder=root;
        auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_BROWSER),nullptr,testProc,reinterpret_cast<LPARAM>(&state));
        if(mode==2) { if(result!=IDCANCEL || !state.selected.empty()) fail(__LINE__); }
        else if(result!=IDOK || state.selected.size()!=(mode==1 ? 2 : 1) || state.selected.front()!=root/L"A.epub" || (mode==1 && state.selected.back()!=root/L"C.rtf")) fail(__LINE__);
    }
    // Detached preview work owns its state and does not gate selection/cancellation.
    for(auto name:{L"A.epub",L"B.pdf",L"C.rtf",L"Ignored.exe"}) { std::error_code error; books::fs::remove(root/name,error); }
    std::error_code error; books::fs::remove(root,error);
    std::cout<<(failures ? "FAIL" : "PASS")<<": folder filter, navigation, single/multiple selection, cancel\n";
    CoUninitialize(); return failures ? 1 : 0;
}
