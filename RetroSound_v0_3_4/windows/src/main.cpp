#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <avrt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avrt.lib")

struct Config { int rate=22050; int bits=16; int channels=2; int bufferMs=100; bool atomMode=true; std::string ip="192.168.1.35"; int port=49152; };
#pragma pack(push,1)
struct Header { char magic[4]={'R','S','N','D'}; uint16_t version=1; uint32_t rate=22050; uint8_t bits=16; uint8_t channels=2; uint16_t bufferMs=100; };
#pragma pack(pop)
static_assert(sizeof(Header)==14, "RetroSound header must be 14 bytes");

static HWND gIpControl, gQualityButton, gBufferButton, gCheckAtom, gButton, gStatus;
static int gQualityIndex=1;
static int gBufferIndex=1;
static std::atomic<bool> gRunning{false};
static std::thread gWorker;
static constexpr UINT WM_STATUS = WM_APP+1;

static void DebugLine(const char* text){
    std::printf("%s\n", text);
    std::fflush(stdout);

    wchar_t temp[MAX_PATH]{};
    if(GetTempPathW(MAX_PATH,temp)){
        std::wstring path=temp;
        path += L"RetroSoundSender-debug.log";
        HANDLE f=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                             nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f!=INVALID_HANDLE_VALUE){
            DWORD written=0;
            WriteFile(f,text,(DWORD)strlen(text),&written,nullptr);
            const char eol[]="\r\n";
            WriteFile(f,eol,2,&written,nullptr);
            CloseHandle(f);
        }
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep){
    char buf[256]{};
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    std::snprintf(buf,sizeof(buf),"FATAL EXCEPTION code=0x%08lX address=%p",
                  (unsigned long)code,addr);
    DebugLine(buf);

    wchar_t wbuf[320]{};
    wsprintfW(wbuf,L"RetroSound crashed. Exception 0x%08lX at %p.\n\nOpen %%TEMP%%\\RetroSoundSender-debug.log and send its contents.",
              (unsigned long)code,addr);
    MessageBoxW(nullptr,wbuf,L"RetroSound v0.3.4 crash diagnostic",
                MB_OK|MB_ICONERROR|MB_SYSTEMMODAL);
    return EXCEPTION_EXECUTE_HANDLER;
}

static HWND MakeControl(DWORD exStyle, const wchar_t* cls, const wchar_t* text,
                        DWORD style, int x, int y, int w, int h,
                        HWND parent, HMENU id=nullptr){
    HWND c=CreateWindowExW(exStyle,cls,text,style,x,y,w,h,parent,id,
                           GetModuleHandleW(nullptr),nullptr);
    if(!c){
        char buf[256]{};
        std::snprintf(buf,sizeof(buf),"CreateWindowExW control failed, class=%ls error=%lu",
                      cls,(unsigned long)GetLastError());
        DebugLine(buf);
    }
    return c;
}

static void PostStatus(HWND hwnd, const std::wstring& s){ auto* p=new std::wstring(s); PostMessage(hwnd, WM_STATUS, 0, (LPARAM)p); }

static bool SendAll(SOCKET s, const char* data, int len){
    while(len>0){ int n=send(s,data,len,0); if(n<=0) return false; data+=n; len-=n; } return true;
}

static float SampleToFloat(const BYTE* p, WORD formatTag, WORD bits){
    if(formatTag==WAVE_FORMAT_IEEE_FLOAT && bits==32) return *(const float*)p;
    if(bits==16){ return (float)(*(const int16_t*)p)/32768.0f; }
    if(bits==24){ int32_t v=(int32_t)(p[0]|(p[1]<<8)|(p[2]<<16)); if(v&0x800000) v|=~0xFFFFFF; return (float)v/8388608.0f; }
    if(bits==32){ return (float)(*(const int32_t*)p)/2147483648.0f; }
    if(bits==8){ return ((int)*p-128)/128.0f; }
    return 0.f;
}

static WORD ResolveFormatTag(WAVEFORMATEX* wf){
    if(wf->wFormatTag!=WAVE_FORMAT_EXTENSIBLE) return wf->wFormatTag;
    auto* ex=(WAVEFORMATEXTENSIBLE*)wf;
    if(ex->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) return WAVE_FORMAT_IEEE_FLOAT;
    return WAVE_FORMAT_PCM;
}

static void ConvertBlock(const BYTE* src, UINT32 frames, WAVEFORMATEX* wf, const Config& cfg,
                         double& phase, std::vector<char>& out){
    const int inRate=wf->nSamplesPerSec;
    const int inCh=wf->nChannels;
    const int inBps=wf->wBitsPerSample/8;
    const int frameBytes=wf->nBlockAlign;
    WORD tag=ResolveFormatTag(wf);
    double step=(double)inRate/(double)cfg.rate;
    if(step<1.0) step=1.0; // v0.1 intentionally optimized for downsampling/equal rate
    size_t maxOut=(size_t)(frames/step + 4)*cfg.channels*(cfg.bits/8);
    out.clear(); out.reserve(maxOut);
    while((UINT32)phase<frames){
        UINT32 i=(UINT32)phase;
        const BYTE* f=src + (size_t)i*frameBytes;
        float l=SampleToFloat(f,tag,wf->wBitsPerSample);
        float r=(inCh>1)?SampleToFloat(f+inBps,tag,wf->wBitsPerSample):l;
        l=std::max(-1.f,std::min(1.f,l)); r=std::max(-1.f,std::min(1.f,r));
        auto put8=[&](float v){ uint8_t q=(uint8_t)std::clamp((int)std::lround((v*0.5f+0.5f)*255.f),0,255); out.push_back((char)q); };
        auto put16=[&](float v){ int16_t q=(int16_t)std::clamp((int)std::lround(v*32767.f),-32768,32767); out.push_back((char)(q&0xff)); out.push_back((char)((q>>8)&0xff)); };
        if(cfg.channels==1){ float m=(l+r)*0.5f; if(cfg.bits==8) put8(m); else put16(m); }
        else { if(cfg.bits==8){ put8(l); put8(r);} else {put16(l); put16(r);} }
        phase += step;
    }
    phase -= frames;
}

static const wchar_t* QualityText(){
    static const wchar_t* q[]={L"44.1 kHz / 16-bit Stereo",L"22.05 kHz / 16-bit Stereo",L"22.05 kHz / 8-bit Stereo",L"16 kHz / 8-bit Mono"};
    return q[gQualityIndex];
}
static const wchar_t* BufferText(){
    static const wchar_t* b[]={L"Low (50 ms)",L"Normal (100 ms)",L"Safe (250 ms)"};
    return b[gBufferIndex];
}
static Config ReadConfig(){
    Config c;
    DWORD ip=0;
    if(gIpControl && SendMessageW(gIpControl,IPM_GETADDRESS,0,(LPARAM)&ip)==4){
        char buf[32]{};
        std::snprintf(buf,sizeof(buf),"%u.%u.%u.%u",
            (unsigned)FIRST_IPADDRESS(ip),(unsigned)SECOND_IPADDRESS(ip),
            (unsigned)THIRD_IPADDRESS(ip),(unsigned)FOURTH_IPADDRESS(ip));
        c.ip=buf;
    } else {
        c.ip="192.168.1.35";
    }
    if(gQualityIndex==0){ c.rate=44100;c.bits=16;c.channels=2; }
    else if(gQualityIndex==1){ c.rate=22050;c.bits=16;c.channels=2; }
    else if(gQualityIndex==2){ c.rate=22050;c.bits=8;c.channels=2; }
    else { c.rate=16000;c.bits=8;c.channels=1; }
    c.bufferMs=(gBufferIndex==0?50:(gBufferIndex==1?100:250));
    c.atomMode=(SendMessageW(gCheckAtom,BM_GETCHECK,0,0)==BST_CHECKED);
    return c;
}

static void AudioWorker(HWND hwnd, Config cfg){
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if(FAILED(hr)){PostStatus(hwnd,L"COM init error");gRunning=false;return;}
    WSADATA wd{}; if(WSAStartup(MAKEWORD(2,2),&wd)!=0){PostStatus(hwnd,L"Winsock error");CoUninitialize();gRunning=false;return;}
    SOCKET sock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    BOOL one=TRUE; setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,(char*)&one,sizeof(one));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons((u_short)cfg.port); inet_pton(AF_INET,cfg.ip.c_str(),&addr.sin_addr);
    PostStatus(hwnd,L"Connecting...");
    if(connect(sock,(sockaddr*)&addr,sizeof(addr))==SOCKET_ERROR){PostStatus(hwnd,L"Connection failed");closesocket(sock);WSACleanup();CoUninitialize();gRunning=false;return;}
    IMMDeviceEnumerator* en=nullptr; IMMDevice* dev=nullptr; IAudioClient* client=nullptr; IAudioCaptureClient* cap=nullptr; WAVEFORMATEX* wf=nullptr;
    Header h; h.rate=cfg.rate; h.bits=(uint8_t)cfg.bits; h.channels=(uint8_t)cfg.channels; h.bufferMs=(uint16_t)cfg.bufferMs;
    if(!SendAll(sock,(char*)&h,sizeof(h))){PostStatus(hwnd,L"Header send failed"); goto cleanup0;}

    hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&en);
    if(FAILED(hr)) {PostStatus(hwnd,L"Audio device enumerator failed"); goto cleanup1;}
    hr=en->GetDefaultAudioEndpoint(eRender,eConsole,&dev); if(FAILED(hr)){PostStatus(hwnd,L"No default playback device");goto cleanup2;}
    hr=dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&client); if(FAILED(hr)){PostStatus(hwnd,L"WASAPI activate failed");goto cleanup3;}
    hr=client->GetMixFormat(&wf); if(FAILED(hr)){PostStatus(hwnd,L"Cannot read Windows mix format");goto cleanup4;}
    REFERENCE_TIME dur=(REFERENCE_TIME)cfg.bufferMs*10000;
    hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,dur,0,wf,nullptr); if(FAILED(hr)){PostStatus(hwnd,L"WASAPI loopback init failed");goto cleanup5;}
    hr=client->GetService(__uuidof(IAudioCaptureClient),(void**)&cap); if(FAILED(hr)){PostStatus(hwnd,L"Capture service failed");goto cleanup5;}
    hr=client->Start(); if(FAILED(hr)){PostStatus(hwnd,L"Audio start failed");goto cleanup6;}
    PostStatus(hwnd,L"Connected - streaming");

    { DWORD taskIndex=0; HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Audio",&taskIndex); double phase=0.0; std::vector<char> converted;
      while(gRunning){
        UINT32 packet=0; if(FAILED(cap->GetNextPacketSize(&packet))) break;
        if(packet==0){ Sleep(cfg.atomMode?5:2); continue; }
        BYTE* data=nullptr; UINT32 frames=0; DWORD flags=0;
        if(SUCCEEDED(cap->GetBuffer(&data,&frames,&flags,nullptr,nullptr))){
            if(flags&AUDCLNT_BUFFERFLAGS_SILENT){ std::vector<BYTE> z((size_t)frames*wf->nBlockAlign); ConvertBlock(z.data(),frames,wf,cfg,phase,converted); }
            else ConvertBlock(data,frames,wf,cfg,phase,converted);
            cap->ReleaseBuffer(frames);
            if(!converted.empty() && !SendAll(sock,converted.data(),(int)converted.size())) break;
        }
      }
      if(mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }
    client->Stop();
cleanup6: if(cap) cap->Release();
cleanup5: if(wf) CoTaskMemFree(wf);
cleanup4: if(client) client->Release();
cleanup3: if(dev) dev->Release();
cleanup2: if(en) en->Release();
cleanup1:
cleanup0: closesocket(sock); WSACleanup(); CoUninitialize(); gRunning=false; PostStatus(hwnd,L"Disconnected");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        DebugLine("[GUI 1/7] main window entered");
        HFONT font=(HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND ipLabel=CreateWindowW(L"STATIC",L"Tablet IP:",WS_CHILD|WS_VISIBLE,
            15,15,80,22,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(ipLabel && font) SendMessageW(ipLabel,WM_SETFONT,(WPARAM)font,TRUE);
        if(!ipLabel){ DebugLine("[GUI 2/7] IP label FAILED"); return -1; }
        DebugLine("[GUI 2/7] IP label OK");

        gIpControl=CreateWindowExW(WS_EX_CLIENTEDGE,WC_IPADDRESSW,L"",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            95,11,180,26,hwnd,(HMENU)101,GetModuleHandleW(nullptr),nullptr);
        if(!gIpControl){
            char b[160]{};
            std::snprintf(b,sizeof(b),"[GUI 3/7] IP address control FAILED error=%lu",(unsigned long)GetLastError());
            DebugLine(b);
            return -1;
        }
        if(font) SendMessageW(gIpControl,WM_SETFONT,(WPARAM)font,TRUE);
        SendMessageW(gIpControl,IPM_SETADDRESS,0,MAKEIPADDRESS(192,168,1,35));
        DebugLine("[GUI 3/7] native IP address control OK");

        HWND qLabel=CreateWindowW(L"STATIC",L"Quality (click to change):",WS_CHILD|WS_VISIBLE,
            15,48,180,22,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(qLabel && font) SendMessageW(qLabel,WM_SETFONT,(WPARAM)font,TRUE);
        gQualityButton=CreateWindowW(L"BUTTON",QualityText(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            15,70,300,30,hwnd,(HMENU)102,GetModuleHandleW(nullptr),nullptr);
        if(gQualityButton && font) SendMessageW(gQualityButton,WM_SETFONT,(WPARAM)font,TRUE);
        if(!gQualityButton){ DebugLine("[GUI 4/7] quality button FAILED"); return -1; }
        DebugLine("[GUI 4/7] quality OK");

        HWND bLabel=CreateWindowW(L"STATIC",L"Buffer (click to change):",WS_CHILD|WS_VISIBLE,
            15,108,180,22,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(bLabel && font) SendMessageW(bLabel,WM_SETFONT,(WPARAM)font,TRUE);
        gBufferButton=CreateWindowW(L"BUTTON",BufferText(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            15,130,220,30,hwnd,(HMENU)103,GetModuleHandleW(nullptr),nullptr);
        if(gBufferButton && font) SendMessageW(gBufferButton,WM_SETFONT,(WPARAM)font,TRUE);
        if(!gBufferButton){ DebugLine("[GUI 5/7] buffer button FAILED"); return -1; }
        DebugLine("[GUI 5/7] buffer OK");

        gCheckAtom=CreateWindowW(L"BUTTON",L"Low CPU / Atom Mode",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            15,170,200,24,hwnd,(HMENU)104,GetModuleHandleW(nullptr),nullptr);
        if(gCheckAtom){ if(font) SendMessageW(gCheckAtom,WM_SETFONT,(WPARAM)font,TRUE); SendMessageW(gCheckAtom,BM_SETCHECK,BST_CHECKED,0); }

        gButton=CreateWindowW(L"BUTTON",L"CONNECT",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            15,205,130,34,hwnd,(HMENU)1,GetModuleHandleW(nullptr),nullptr);
        if(gButton && font) SendMessageW(gButton,WM_SETFONT,(WPARAM)font,TRUE);
        if(!gCheckAtom || !gButton){ DebugLine("[GUI 6/7] connect controls FAILED"); return -1; }
        DebugLine("[GUI 6/7] connect controls OK");

        gStatus=CreateWindowW(L"STATIC",L"READY - enter tablet IP, then CONNECT",WS_CHILD|WS_VISIBLE,
            15,250,360,25,hwnd,(HMENU)105,GetModuleHandleW(nullptr),nullptr);
        if(gStatus && font) SendMessageW(gStatus,WM_SETFONT,(WPARAM)font,TRUE);
        if(!gStatus){ DebugLine("[GUI 7/7] status FAILED"); return -1; }
        DebugLine("[GUI 7/7] READY");
        return 0; }
    case WM_COMMAND:
        if(LOWORD(wp)==102 && !gRunning){ gQualityIndex=(gQualityIndex+1)%4; SetWindowTextW(gQualityButton,QualityText()); return 0; }
        if(LOWORD(wp)==103 && !gRunning){ gBufferIndex=(gBufferIndex+1)%3; SetWindowTextW(gBufferButton,BufferText()); return 0; }
        if(LOWORD(wp)==1){
            if(!gRunning){ Config cfg=ReadConfig(); gRunning=true; SetWindowTextW(gButton,L"STOP"); gWorker=std::thread(AudioWorker,hwnd,cfg); }
            else { gRunning=false; SetWindowTextW(gButton,L"CONNECT"); }
            return 0;
        }
        break;
    case WM_STATUS:{ auto* st=(std::wstring*)lp; SetWindowTextW(gStatus,st->c_str()); if(!gRunning) SetWindowTextW(gButton,L"CONNECT"); delete st; return 0; }
    case WM_DESTROY: gRunning=false; if(gWorker.joinable()) gWorker.join(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int main(){
    SetUnhandledExceptionFilter(CrashFilter);
    DebugLine("[MAIN 1/8] RetroSound Sender v0.3.4 DEBUG starting");

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize=sizeof(icc);
    icc.dwICC=ICC_STANDARD_CLASSES | ICC_INTERNET_CLASSES;
    InitCommonControlsEx(&icc);
    DebugLine("[MAIN 2/8] common controls initialized");

    HINSTANCE hi=GetModuleHandleW(nullptr);
    DebugLine("[MAIN 3/8] module handle acquired");
    WNDCLASSEXW wc{};
    wc.cbSize=sizeof(wc);
    wc.lpfnWndProc=WndProc;
    wc.hInstance=hi;
    wc.lpszClassName=L"RetroSoundSenderWindowV034";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
    wc.hIconSm=wc.hIcon;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);

    DebugLine("[MAIN 4/8] registering main window class");
    ATOM a=RegisterClassExW(&wc);
    if(!a){
        DWORD e=GetLastError();
        std::printf("RegisterClassExW failed: %lu\n",(unsigned long)e);
        wchar_t msg[160]{};
        wsprintfW(msg,L"RegisterClassExW failed. Windows error %lu.",e);
        MessageBoxW(nullptr,msg,L"RetroSound startup error",MB_OK|MB_ICONERROR|MB_SYSTEMMODAL);
        return 10;
    }

    DebugLine("[MAIN 5/8] window class registered");

    const int width=390, height=285;
    int x=(GetSystemMetrics(SM_CXSCREEN)-width)/2;
    int y=(GetSystemMetrics(SM_CYSCREEN)-height)/2;
    if(x<0) x=0; if(y<0) y=0;

    DebugLine("[MAIN 6/8] calling CreateWindowExW (WM_CREATE will run now)");
    HWND hwnd=CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"RetroSound Sender v0.3.4 DEBUG - Windows 8.1 / Atom",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        x,y,width,height,
        nullptr,nullptr,hi,nullptr);

    if(!hwnd){
        DWORD e=GetLastError();
        std::printf("CreateWindowExW failed: %lu\n",(unsigned long)e);
        wchar_t msg[160]{};
        wsprintfW(msg,L"CreateWindowExW failed. Windows error %lu.",e);
        MessageBoxW(nullptr,msg,L"RetroSound startup error",MB_OK|MB_ICONERROR|MB_SYSTEMMODAL);
        return 11;
    }

    DebugLine("[MAIN 7/8] CreateWindowExW returned a valid HWND");
    ShowWindow(hwnd,SW_SHOW);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd,HWND_TOPMOST,x,y,width,height,SWP_SHOWWINDOW);
    SetWindowPos(hwnd,HWND_NOTOPMOST,x,y,width,height,SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);

    DebugLine("[MAIN 8/8] GUI visible - entering message loop");

    // DEBUG BUILD: keep the console visible intentionally.
    MSG m{};
    while(true){
        BOOL r=GetMessageW(&m,nullptr,0,0);
        if(r==0) break;
        if(r==-1){
            MessageBoxW(hwnd,L"Windows message loop failed.",L"RetroSound error",MB_OK|MB_ICONERROR|MB_SYSTEMMODAL);
            return 12;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}
