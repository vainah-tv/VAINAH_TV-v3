#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <unknwn.h>
#include <objidl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wrl/client.h>
#include <oleauto.h>
#include <wininet.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static constexpr wchar_t kStreams[][160] = {
    L"https://live.smotrim.ru/vgtrk/0/russia1-hd/1080p.m3u8",
    L"https://live-gtrk.smotrim.ru/vgtrk/grozniy/russia1-sd/track_103_947c7bd7/chunklist.m3u8",
    L"https://podcast-gtrk.smotrim.ru/vgtrk/grozniy/radio_russia/track_1001_85855c03/chunklist.m3u8"
};
static constexpr wchar_t kChannelNames[][24] = {L"РОССИЯ 1HD", L"ВАЙНАХ ТВ", L"РАДИО ГРОЗНЫЙ"};
static constexpr int kChannelCount = sizeof(kChannelNames)/sizeof(kChannelNames[0]);
static constexpr int kSidebarWidth = 190;
static constexpr UINT kMediaEvent = WM_APP + 1;
static constexpr UINT kStart = WM_APP + 2;
static constexpr UINT kGuideReady = WM_APP + 3;
static constexpr UINT_PTR kHudTimer = 301;
static HWND gWindow = nullptr, gVideo = nullptr, gRadioArt = nullptr;
static HWND gGuide = nullptr, gGuideText = nullptr, gEpg = nullptr;
static HWND gPin = nullptr, gTooltip = nullptr, gHud = nullptr;
static HBRUSH gGuideBrush = nullptr;
static ULONG_PTR gGdiToken = 0;
static Gdiplus::Bitmap* gPinImage = nullptr;
static IStream* gPinStream = nullptr;
static Gdiplus::Bitmap* gRadioImage = nullptr;
static IStream* gRadioStream = nullptr;
static WNDPROC gOldVideoProc = nullptr;
static ComPtr<IMFMediaEngine> gEngine;
static std::wstring gStatus = L"Подключение к трансляции…";
static bool gFullscreen = false, gMuted = false;
static bool gAlwaysOnTop = false;
static bool gDraggingVolume = false;
static int gChannel = 0;
static double gVolume = 1.0;
static int gWheelRemainder = 0;
static RECT gRestoreRect = {};
static DWORD gRestoreStyle = 0;
static ULONGLONG gLastClick = 0;
static POINT gLastPoint = {};

static std::wstring Utf8(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),(int)bytes.size(),nullptr,0);
    if (!n) return L"";
    std::wstring result(n,L' ');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),(int)bytes.size(),&result[0],n);
    return result;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a=s.find_first_not_of(L" \t\r\n\x00a0"), b=s.find_last_not_of(L" \t\r\n\x00a0");
    return a==std::wstring::npos ? L"" : s.substr(a,b-a+1);
}

static std::wstring GuideFromHtml(const std::string& html) {
    std::wstring source=Utf8(html), plain;
    if (source.empty()) return L"";
    bool hidden=false;
    for (size_t i=0;i<source.size();) {
        if (source[i]==L'<') {
            size_t end=source.find(L'>',i);
            if (end==std::wstring::npos) break;
            std::wstring t=source.substr(i+1,end-i-1);
            std::transform(t.begin(),t.end(),t.begin(),[](wchar_t c){return (wchar_t)towlower(c);});
            if (t.compare(0,6,L"script")==0 || t.compare(0,5,L"style")==0) hidden=true;
            if (t.compare(0,7,L"/script")==0 || t.compare(0,6,L"/style")==0) hidden=false;
            if (!hidden && (t.compare(0,2,L"br")==0 ||
                t.compare(0,2,L"/p")==0 || t.compare(0,4,L"/div")==0 ||
                t.compare(0,3,L"/li")==0 || t.compare(0,2,L"/h")==0 ||
                t.compare(0,3,L"/tr")==0)) plain+=L'\n';
            else if (!hidden && !plain.empty() && plain.back()!=L' ') plain+=L' ';
            i=end+1; continue;
        }
        if (!hidden && source[i]==L'&') {
            size_t end=source.find(L';',i+1);
            if (end!=std::wstring::npos && end-i<12) {
                std::wstring e=source.substr(i+1,end-i-1);
                if (e==L"nbsp" || e==L"#160") plain+=L' ';
                else if (e==L"amp") plain+=L'&';
                else if (e==L"quot") plain+=L'"';
                else if (e==L"lt") plain+=L'<';
                else if (e==L"gt") plain+=L'>';
                else if (!e.empty() && e[0]==L'#') {
                    wchar_t* next=nullptr; unsigned long cp=wcstoul(e.c_str()+1,&next,10);
                    if (next && *next==0 && cp>0 && cp<=0xffff) plain+=(wchar_t)cp;
                }
                i=end+1; continue;
            }
        }
        if (!hidden) plain+=source[i];
        ++i;
    }
    // Reject cached pages for another week before presenting them as a live guide.
    SYSTEMTIME utc={}, moscow={}; FILETIME ft={};
    GetSystemTime(&utc);
    if (!SystemTimeToFileTime(&utc,&ft)) return L"";
    ULARGE_INTEGER ticks={}; ticks.LowPart=ft.dwLowDateTime; ticks.HighPart=ft.dwHighDateTime;
    ticks.QuadPart+=3ULL*60*60*10000000;
    ft.dwLowDateTime=ticks.LowPart; ft.dwHighDateTime=ticks.HighPart;
    if (!FileTimeToSystemTime(&ft,&moscow)) return L"";
    const wchar_t* months[]={L"января",L"февраля",L"марта",L"апреля",L"мая",L"июня",
                             L"июля",L"августа",L"сентября",L"октября",L"ноября",L"декабря"};
    wchar_t date[64];
    swprintf_s(date,L"%02u %s %04u",moscow.wDay,months[moscow.wMonth-1],moscow.wYear);
    if (plain.find(date)==std::wstring::npos) return L"";
    std::wstring result=L"РОССИЯ 1HD — программа передач (время московское)\r\nИсточник: iptvX|one\r\n\r\n";
    size_t offset=0; int entries=0; bool started=false;
    while (offset<plain.size() && entries<250) {
        size_t end=plain.find(L'\n',offset);
        std::wstring line=Trim(plain.substr(offset,end==std::wstring::npos ? std::wstring::npos : end-offset));
        offset=end==std::wstring::npos ? plain.size() : end+1;
        if (line.find(L"Понедельник,")!=std::wstring::npos ||
            line.find(L"Вторник,")!=std::wstring::npos ||
            line.find(L"Среда,")!=std::wstring::npos ||
            line.find(L"Четверг,")!=std::wstring::npos ||
            line.find(L"Пятница,")!=std::wstring::npos ||
            line.find(L"Суббота,")!=std::wstring::npos ||
            line.find(L"Воскресенье,")!=std::wstring::npos) {
            started=true; result+=L"\r\n"+line+L"\r\n"; continue;
        }
        if (!started) continue;
        // Each programme line starts with a 24-hour clock time.
        size_t p=0;
        while (p+5<line.size() && !(iswdigit(line[p]) && iswdigit(line[p+1]) &&
               line[p+2]==L':' && iswdigit(line[p+3]) && iswdigit(line[p+4]))) ++p;
        if (p+5>=line.size() || p>4) continue;
        int hh=(line[p]-L'0')*10+line[p+1]-L'0';
        int mm=(line[p+3]-L'0')*10+line[p+4]-L'0';
        if (hh>23 || mm>59) continue;
        std::wstring title=Trim(line.substr(p+5));
        if (title.empty() || title.size()>220) continue;
        result+=line.substr(p,5)+L"  "+title+L"\r\n"; ++entries;
    }
    return entries ? result : L"";
}

static DWORD WINAPI GuideWorker(LPVOID parameter) {
    HWND target=(HWND)parameter;
    std::string bytes;
    HINTERNET session=InternetOpenW(L"Russia1Player/1.0",INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr,nullptr,0);
    if (session) {
        DWORD timeout=8000;
        InternetSetOptionW(session,INTERNET_OPTION_CONNECT_TIMEOUT,&timeout,sizeof(timeout));
        InternetSetOptionW(session,INTERNET_OPTION_RECEIVE_TIMEOUT,&timeout,sizeof(timeout));
        HINTERNET request=InternetOpenUrlW(session,L"https://epg.iptvx.one/id/rossia1",
                                          nullptr,0,INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE,0);
        if (request) {
            char buffer[8192]; DWORD count=0;
            while (bytes.size()<3*1024*1024 && InternetReadFile(request,buffer,sizeof(buffer),&count) && count)
                bytes.append(buffer,count);
            InternetCloseHandle(request);
        }
        InternetCloseHandle(session);
    }
    auto* guide=new std::wstring(GuideFromHtml(bytes));
    if (guide->empty()) *guide=L"Не удалось загрузить программу передач. Проверьте подключение к интернету и повторите попытку.";
    if (!PostMessageW(target,kGuideReady,0,(LPARAM)guide)) delete guide;
    return 0;
}

static void RefreshGuide() {
    if (!gGuideText) return;
    if (gChannel!=0) {
        SetWindowTextW(gGuideText,gChannel==1 ?
            L"Для ВАЙНАХ ТВ пока нет проверенного расписания передач." :
            L"Для РАДИО ГРОЗНЫЙ пока нет проверенного расписания передач.");
        return;
    }
    SetWindowTextW(gGuideText,L"Загрузка программы передач…");
    HANDLE thread=CreateThread(nullptr,0,GuideWorker,gGuide,0,nullptr);
    if (thread) CloseHandle(thread);
    else SetWindowTextW(gGuideText,L"Не удалось начать загрузку программы передач.");
}

static void PositionGuide() {
    if (!gGuide || !gVideo) return;
    RECT video; GetWindowRect(gVideo,&video);
    int vw=video.right-video.left, vh=video.bottom-video.top;
    int width=(std::min)(620,(std::max)(1,vw-24));
    int height=(std::min)(440,(std::max)(1,vh-24));
    SetWindowPos(gGuide,nullptr,video.left+(vw-width)/2,video.top+(vh-height)/2,
                 width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}

static LRESULT CALLBACK GuideProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        gGuideBrush=CreateSolidBrush(RGB(19,30,49));
        gGuideText=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|
                 WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
                 18,60,580,350,window,nullptr,(HINSTANCE)GetWindowLongPtrW(window,GWLP_HINSTANCE),nullptr);
        HFONT font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SendMessageW(gGuideText,WM_SETFONT,(WPARAM)font,TRUE);
        return 0;
    }
    case WM_SIZE:
        if (gGuideText) MoveWindow(gGuideText,18,60,(std::max)(1,(int)LOWORD(lp)-36),
                                  (std::max)(1,(int)HIWORD(lp)-78),TRUE);
        SetWindowRgn(window,CreateRoundRectRgn(0,0,LOWORD(lp),HIWORD(lp),22,22),TRUE);
        InvalidateRect(window,nullptr,TRUE);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp;
        SetBkColor(dc,RGB(19,30,49)); SetTextColor(dc,RGB(234,242,255));
        return (LRESULT)gGuideBrush;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(window,&ps);
        RECT r; GetClientRect(window,&r);
        HBRUSH bg=CreateSolidBrush(RGB(19,30,49)); FillRect(dc,&r,bg); DeleteObject(bg);
        RECT line={18,53,r.right-18,54};
        HBRUSH border=CreateSolidBrush(RGB(65,88,125)); FillRect(dc,&line,border); DeleteObject(border);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(245,249,255));
        HFONT font=CreateFontW(-21,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        HGDIOBJ old=SelectObject(dc,font);
        RECT title={20,10,r.right-60,51};
        DrawTextW(dc,L"EPG  ·  ПРОГРАММА ПЕРЕДАЧ",-1,&title,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        SelectObject(dc,old); DeleteObject(font);
        RECT close={r.right-48,10,r.right-12,46};
        SetTextColor(dc,RGB(220,230,245));
        DrawTextW(dc,L"×",-1,&close,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
        EndPaint(window,&ps); return 0;
    }
    case WM_LBUTTONUP: {
        RECT r; GetClientRect(window,&r);
        POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (p.x>=r.right-50 && p.y<54) { ShowWindow(window,SW_HIDE); if(gEpg) InvalidateRect(gEpg,nullptr,TRUE); }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp==VK_ESCAPE) { ShowWindow(window,SW_HIDE); if(gEpg) InvalidateRect(gEpg,nullptr,TRUE); return 0; }
        break;
    case kGuideReady: {
        std::wstring* guide=(std::wstring*)lp;
        if (gChannel==0 && gGuideText) SetWindowTextW(gGuideText,guide->c_str());
        delete guide; return 0;
    }
    case WM_CLOSE: ShowWindow(window,SW_HIDE); if(gEpg) InvalidateRect(gEpg,nullptr,TRUE); return 0;
    case WM_DESTROY:
        if (gGuideText) {
            HFONT font=(HFONT)SendMessageW(gGuideText,WM_GETFONT,0,0);
            SendMessageW(gGuideText,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),FALSE);
            if (font) DeleteObject(font);
        }
        if (gGuideBrush) { DeleteObject(gGuideBrush); gGuideBrush=nullptr; }
        gGuide=nullptr; gGuideText=nullptr; return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}

static void OpenGuide() {
    if (!gGuide) return;
    PositionGuide();
    ShowWindow(gGuide,SW_SHOW);
    SetWindowPos(gGuide,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    if (gEpg) InvalidateRect(gEpg,nullptr,TRUE);
    RefreshGuide();
}


static COLORREF color(int r, int g, int b) { return RGB(r, g, b); }
static void Redraw() { if (gWindow) InvalidateRect(gWindow, nullptr, FALSE); }
static void Status(const wchar_t* value) { gStatus = value; Redraw(); }

static void SetPlayerVolume(double value) {
    gVolume=(std::max)(0.0,(std::min)(1.0,value));
    if (gMuted && gVolume>0.0) gMuted=false;
    if (gEngine) {
        gEngine->SetVolume(gVolume);
        gEngine->SetMuted(gMuted?TRUE:FALSE);
    }
    Redraw();
}

static void SetVolumeFromX(int x) { SetPlayerVolume((x-144)/86.0); }

static void LoadPinImage(HINSTANCE instance) {
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(101),RT_RCDATA);
    if (!resource) return;
    DWORD length=SizeofResource(instance,resource);
    HGLOBAL loaded=LoadResource(instance,resource);
    const void* bytes=loaded?LockResource(loaded):nullptr;
    if (!bytes || !length) return;
    HGLOBAL buffer=GlobalAlloc(GMEM_MOVEABLE,length);
    if (!buffer) return;
    void* destination=GlobalLock(buffer);
    if (!destination) { GlobalFree(buffer); return; }
    memcpy(destination,bytes,length);
    GlobalUnlock(buffer);
    if (FAILED(CreateStreamOnHGlobal(buffer,TRUE,&gPinStream))) {
        GlobalFree(buffer); return;
    }
    gPinImage=Gdiplus::Bitmap::FromStream(gPinStream,FALSE);
    if (gPinImage && gPinImage->GetLastStatus()!=Gdiplus::Ok) {
        delete gPinImage; gPinImage=nullptr;
    }
}

static void LoadRadioImage(HINSTANCE instance) {
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(102),RT_RCDATA);
    if (!resource) return;
    DWORD length=SizeofResource(instance,resource);
    HGLOBAL loaded=LoadResource(instance,resource);
    const void* bytes=loaded?LockResource(loaded):nullptr;
    if (!bytes || !length) return;
    HGLOBAL buffer=GlobalAlloc(GMEM_MOVEABLE,length);
    if (!buffer) return;
    void* destination=GlobalLock(buffer);
    if (!destination) { GlobalFree(buffer); return; }
    memcpy(destination,bytes,length);
    GlobalUnlock(buffer);
    if (FAILED(CreateStreamOnHGlobal(buffer,TRUE,&gRadioStream))) {
        GlobalFree(buffer); return;
    }
    gRadioImage=Gdiplus::Bitmap::FromStream(gRadioStream,FALSE);
    if (gRadioImage && gRadioImage->GetLastStatus()!=Gdiplus::Ok) {
        delete gRadioImage; gRadioImage=nullptr;
    }
}

class EngineNotify final : public IMFMediaEngineNotify {
    std::atomic<ULONG> refs_{1};
public:
    STDMETHODIMP QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFMediaEngineNotify)) {
            *result = static_cast<IMFMediaEngineNotify*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --refs_; if (!n) delete this; return n; }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR, DWORD param2) override {
        if (gWindow) PostMessageW(gWindow, kMediaEvent, event, param2);
        return S_OK;
    }
};

static HRESULT Connect() {
    if (!gEngine) return E_FAIL;
    Status(L"Подключение к трансляции…");
    BSTR bstr = SysAllocString(kStreams[gChannel]);
    if (!bstr) return E_OUTOFMEMORY;
    HRESULT hr = gEngine->SetSource(bstr);
    SysFreeString(bstr);
    if (SUCCEEDED(hr)) hr = gEngine->Load();
    if (SUCCEEDED(hr)) hr = gEngine->Play();
    if (FAILED(hr)) Status(L"Не удалось подключиться к эфиру");
    return hr;
}

static void Layout(int w, int h);
static void SelectChannel(int channel) {
    if (channel == gChannel || channel < 0 || channel >= kChannelCount) return;
    gChannel = channel;
    SetWindowTextW(gWindow, channel == 0 ? L"Россия 1HD — прямой эфир" :
                            channel == 1 ? L"Вайнах ТВ — прямой эфир" :
                                           L"Радио Грозный — прямой эфир");
    RECT client; GetClientRect(gWindow,&client);
    Layout(client.right,client.bottom);
    Connect();
    if (gGuide && IsWindowVisible(gGuide)) OpenGuide();
    Redraw();
}

static HRESULT InitializePlayer() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return hr;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { CoUninitialize(); return hr; }
    ComPtr<IMFAttributes> attrs;
    ComPtr<IMFMediaEngineClassFactory> factory;
    EngineNotify* callback = new EngineNotify();
    hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) hr = attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, callback);
    if (SUCCEEDED(hr)) hr = attrs->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND,
                                            reinterpret_cast<UINT64>(gVideo));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attrs.Get(), &gEngine);
    callback->Release();
    if (SUCCEEDED(hr)) hr = gEngine->SetAutoPlay(TRUE);
    if (SUCCEEDED(hr)) hr = gEngine->SetVolume(gVolume);
    if (FAILED(hr)) {
        if (gEngine) { gEngine->Shutdown(); gEngine.Reset(); }
        MFShutdown(); CoUninitialize();
    }
    return hr;
}

static void Fullscreen() {
    if (!gFullscreen) {
        gRestoreStyle = (DWORD)GetWindowLongPtrW(gWindow, GWL_STYLE);
        GetWindowRect(gWindow, &gRestoreRect);
        HMONITOR monitor = MonitorFromWindow(gWindow, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        gFullscreen = true;
        SetWindowLongPtrW(gWindow, GWL_STYLE, (gRestoreStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(gWindow, gAlwaysOnTop ? HWND_TOPMOST : HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        gFullscreen = false;
        SetWindowLongPtrW(gWindow, GWL_STYLE, gRestoreStyle);
        SetWindowPos(gWindow, nullptr, gRestoreRect.left, gRestoreRect.top,
                     gRestoreRect.right - gRestoreRect.left,
                     gRestoreRect.bottom - gRestoreRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    RECT client; GetClientRect(gWindow, &client);
    Layout(client.right, client.bottom);
    SetFocus(gWindow);
    Redraw();
}

static void DoubleClick(POINT p) {
    ULONGLONG now = GetTickCount64();
    int dx = p.x - gLastPoint.x, dy = p.y - gLastPoint.y;
    if (now - gLastClick <= GetDoubleClickTime() && dx * dx + dy * dy < 100) {
        gLastClick = 0; Fullscreen();
    } else { gLastClick = now; gLastPoint = p; }
}

static LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEWHEEL) return SendMessageW(gWindow,msg,wp,lp);
    if (msg == WM_LBUTTONUP) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &p);
        DoubleClick(p);
    }
    return CallWindowProcW(gOldVideoProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK RadioArtProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEWHEEL: return SendMessageW(gWindow,msg,wp,lp);
    case WM_LBUTTONUP: {
        POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd,&p);
        DoubleClick(p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
        RECT client; GetClientRect(hwnd,&client);
        HBRUSH background=CreateSolidBrush(RGB(3,24,83));
        FillRect(dc,&client,background); DeleteObject(background);
        if (gRadioImage) {
            int w=client.right-client.left, h=client.bottom-client.top;
            int imageW=(int)gRadioImage->GetWidth(), imageH=(int)gRadioImage->GetHeight();
            if (imageW>0 && imageH>0 && w>0 && h>0) {
                double scale=(std::min)(w/(double)imageW,h/(double)imageH);
                int drawW=(int)(imageW*scale), drawH=(int)(imageH*scale);
                Gdiplus::Graphics graphics(dc);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                graphics.DrawImage(gRadioImage,Gdiplus::Rect((w-drawW)/2,(h-drawH)/2,drawW,drawH));
            }
        }
        EndPaint(hwnd,&ps); return 0;
    }
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static void Fill(HDC dc, RECT rect, COLORREF c) {
    HBRUSH brush = CreateSolidBrush(c); FillRect(dc, &rect, brush); DeleteObject(brush);
}
static void Round(HDC dc, RECT r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ oldb = SelectObject(dc, b), oldp = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldp); SelectObject(dc, oldb); DeleteObject(b);
}
static void Text(HDC dc, const wchar_t* value, RECT rect, int size, COLORREF c,
                 UINT flags = DT_SINGLELINE | DT_VCENTER) {
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextW(dc, value, -1, &rect, flags);
    SelectObject(dc, old); DeleteObject(font);
}
static void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int width = 2) {
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
    SelectObject(dc, old); DeleteObject(pen);
}
static void Speaker(HDC dc, int x, int y) {
    COLORREF white = color(235, 241, 250);
    POINT polygon[] = {{x-10,y-5},{x-5,y-5},{x+2,y-10},{x+2,y+10},{x-5,y+5},{x-10,y+5}};
    HBRUSH b = CreateSolidBrush(white);
    HGDIOBJ oldb = SelectObject(dc,b), oldp = SelectObject(dc,GetStockObject(NULL_PEN));
    Polygon(dc,polygon,6); SelectObject(dc,oldp); SelectObject(dc,oldb); DeleteObject(b);
    if (gMuted) {
        Line(dc,x+8,y-8,x+18,y+8,color(255,95,105),3);
        Line(dc,x+18,y-8,x+8,y+8,color(255,95,105),3);
    }
    else {
        Line(dc,x+8,y-7,x+13,y-2,white,2);
        Line(dc,x+13,y-2,x+13,y+2,white,2);
        Line(dc,x+13,y+2,x+8,y+7,white,2);
        Line(dc,x+13,y-11,x+19,y-5,white,2);
        Line(dc,x+19,y-5,x+19,y+5,white,2);
        Line(dc,x+19,y+5,x+13,y+11,white,2);
    }
}

static void PositionVolumeHud() {
    if (!gHud || !gWindow) return;
    RECT client; GetClientRect(gWindow,&client);
    POINT point={(client.right-240)/2,(std::max)(12,static_cast<int>(client.bottom)-150)};
    ClientToScreen(gWindow,&point);
    SetWindowPos(gHud,HWND_TOPMOST,point.x,point.y,240,78,
                 SWP_NOACTIVATE|SWP_SHOWWINDOW);
}

static void ShowVolumeHud() {
    if (!gHud) return;
    PositionVolumeHud();
    InvalidateRect(gHud,nullptr,TRUE);
    SetTimer(gWindow,kHudTimer,1400,nullptr);
}

static LRESULT CALLBACK HudProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(window,&ps);
        RECT all={0,0,240,78}; Fill(dc,all,color(23,33,49));
        RECT title={18,8,170,39};
        Text(dc,L"ГРОМКОСТЬ",title,14,color(237,245,255));
        std::wstring value=std::to_wstring((int)(gVolume*100+0.5))+L"%";
        RECT percent={171,8,224,39};
        Text(dc,value.c_str(),percent,16,color(237,245,255),
             DT_SINGLELINE|DT_RIGHT|DT_VCENTER);
        RECT track={18,49,222,58}; Round(dc,track,color(61,78,103),9);
        RECT active={18,49,18+(int)(204*gVolume),58};
        if(active.right>active.left) Round(dc,active,color(101,169,255),9);
        EndPaint(window,&ps); return 0;
    }
    }
    return DefWindowProcW(window,msg,wp,lp);
}

static bool Inside(RECT r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static void Paint(HDC dc, int w, int h) {
    RECT all = {0,0,w,h}; Fill(dc,all,color(10,15,25));
    if (gFullscreen) return;
    RECT header = {0,0,w,62}; Fill(dc,header,color(16,24,39));
    RECT logo = {kSidebarWidth+22,16,kSidebarWidth+58,46};
    Round(dc,logo,gChannel == 0 ? color(32,101,220) :
                  gChannel == 1 ? color(29,132,125) : color(23,73,177),9);
    Text(dc,gChannel == 0 ? L"1" : gChannel == 1 ? L"В" : L"Р",
         logo,23,color(255,255,255),DT_SINGLELINE|DT_CENTER|DT_VCENTER);
    RECT title = {kSidebarWidth+70,13,w-170,48};
    Text(dc,kChannelNames[gChannel],title,20,color(243,247,255),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    RECT dot = {w-152,27,w-143,36}; Round(dc,dot,color(255,63,82),9);
    RECT live = {w-133,17,w-22,47}; Text(dc,L"ПРЯМОЙ ЭФИР",live,12,color(255,109,124));

    RECT footer = {0,h-84,w,h}; Fill(dc,footer,color(16,24,39));
    Line(dc,0,h-84,w,h-84,color(45,59,79),1);
    RECT sidebar = {0,0,kSidebarWidth,h-84}; Fill(dc,sidebar,color(20,29,45));
    Line(dc,kSidebarWidth-1,0,kSidebarWidth-1,h-84,color(45,59,79),1);
    RECT sidebarTitle = {18,19,kSidebarWidth-14,48};
    Text(dc,L"КАНАЛЫ",sidebarTitle,13,color(151,171,201));
    for (int i=0; i<kChannelCount; ++i) {
        int top=70+i*62;
        RECT item={10,top,kSidebarWidth-10,top+50};
        if (i==gChannel) Round(dc,item,color(40,76,134),12);
        RECT label={22,top,kSidebarWidth-18,top+50};
        Text(dc,kChannelNames[i],label,15,
             i==gChannel ? color(255,255,255) : color(192,207,228));
    }
    RECT play = {20,h-65,62,h-15}; Round(dc,play,color(35,95,213),14);
    if (gEngine && gEngine->IsPaused()) {
        POINT triangle[] = {{36,h-53},{36,h-27},{50,h-40}};
        HBRUSH b=CreateSolidBrush(color(255,255,255)); HGDIOBJ ob=SelectObject(dc,b);
        HGDIOBJ op=SelectObject(dc,GetStockObject(NULL_PEN));
        Polygon(dc,triangle,3); SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(b);
    } else { RECT a={34,h-52,39,h-28},b={44,h-52,49,h-28};
             Fill(dc,a,color(255,255,255)); Fill(dc,b,color(255,255,255)); }
    RECT mute = {73,h-65,123,h-15}; Round(dc,mute,color(31,43,62),12);
    Speaker(dc,96,h-40);
    RECT track={144,h-44,230,h-36}; Round(dc,track,color(62,78,101),8);
    RECT active={144,h-44,144+(int)(86*gVolume),h-36};
    if (active.right>active.left) Round(dc,active,color(93,158,255),8);
    int thumbX=144+(int)(86*gVolume);
    HBRUSH thumb=CreateSolidBrush(color(240,247,255));
    HGDIOBJ oldBrush=SelectObject(dc,thumb);
    HGDIOBJ oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
    Ellipse(dc,thumbX-6,h-46,thumbX+6,h-34);
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(thumb);
    std::wstring percent=std::to_wstring((int)(gVolume*100+0.5))+L"%";
    RECT volumeText={241,h-65,289,h-15};
    Text(dc,percent.c_str(),volumeText,13,color(190,205,226));
    RECT status = {294,h-69,w-155,h-14};
    Text(dc,gStatus.c_str(),status,14,color(183,198,219),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
}

static void Layout(int w, int h) {
    if (!gVideo) return;
    if (gPin) {
        MoveWindow(gPin,w-58,h-60,36,40,TRUE);
        ShowWindow(gPin,gFullscreen?SW_HIDE:SW_SHOW);
    }
    if (gEpg) {
        MoveWindow(gEpg,w-142,h-60,76,40,TRUE);
        ShowWindow(gEpg,gFullscreen?SW_HIDE:SW_SHOW);
    }
    if (gFullscreen) {
        MoveWindow(gVideo,0,0,w,(std::max)(1,h),TRUE);
        ShowWindow(gVideo,gChannel==2 ? SW_HIDE : SW_SHOW);
        if (gRadioArt) SetWindowPos(gRadioArt,HWND_TOP,0,0,w,(std::max)(1,h),
                                    gChannel==2 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        if (gGuide && IsWindowVisible(gGuide)) PositionGuide();
        Redraw(); return;
    }
    int areaTop=70, areaBottom=h-92;
    int availableW=(std::max)(1,w-kSidebarWidth-24), availableH=(std::max)(1,areaBottom-areaTop);
    int videoW=(std::min)(availableW,availableH*16/9);
    int videoH=(std::min)(availableH,videoW*9/16);
    int videoX=kSidebarWidth+(w-kSidebarWidth-videoW)/2;
    int videoY=areaTop+(availableH-videoH)/2;
    MoveWindow(gVideo,videoX,videoY,videoW,videoH,TRUE);
    ShowWindow(gVideo,gChannel==2 ? SW_HIDE : SW_SHOW);
    if (gRadioArt) SetWindowPos(gRadioArt,HWND_TOP,videoX,videoY,videoW,videoH,
                                gChannel==2 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    if (gGuide && IsWindowVisible(gGuide)) PositionGuide();
    Redraw();
}

static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_DRAWITEM: {
        const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlID != 201 && item->CtlID != 202) break;
        HDC dc=item->hDC; RECT r=item->rcItem;
        Fill(dc,r,color(16,24,39));
        if (item->CtlID==202) {
            Round(dc,{r.left+1,r.top+2,r.right-1,r.bottom-2},
                  gGuide && IsWindowVisible(gGuide) ? color(37,116,225) : color(39,83,151),12);
            Text(dc,L"EPG",r,16,color(255,255,255),DT_SINGLELINE|DT_CENTER|DT_VCENTER);
            return TRUE;
        }
        Round(dc,r,gAlwaysOnTop?color(95,32,43):color(31,43,62),12);
        int cx=(r.left+r.right)/2, cy=(r.top+r.bottom)/2;
        if (gPinImage) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.DrawImage(gPinImage,Gdiplus::Rect(cx-11,cy-12,22,24));
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==202 && HIWORD(wp)==BN_CLICKED) {
            if (gGuide && IsWindowVisible(gGuide)) {
                ShowWindow(gGuide,SW_HIDE); InvalidateRect(gEpg,nullptr,TRUE);
            } else OpenGuide();
            return 0;
        }
        if (LOWORD(wp)==201 && HIWORD(wp)==BN_CLICKED) {
            gAlwaysOnTop=!gAlwaysOnTop;
            SetWindowPos(window,gAlwaysOnTop?HWND_TOPMOST:HWND_NOTOPMOST,
                         0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            InvalidateRect(gPin,nullptr,TRUE);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* limits=reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMinTrackSize={760,550}; return 0;
    }
    case WM_SIZE: Layout(LOWORD(lp),HIWORD(lp)); return 0;
    case WM_MOVE:
        if (gHud && IsWindowVisible(gHud)) PositionVolumeHud();
        if (gGuide && IsWindowVisible(gGuide)) PositionGuide();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp)==WA_INACTIVE && gHud) ShowWindow(gHud,SW_HIDE);
        break;
    case WM_TIMER:
        if (wp==kHudTimer) {
            KillTimer(window,kHudTimer);
            if (gHud) ShowWindow(gHud,SW_HIDE);
            return 0;
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC screen=BeginPaint(window,&ps);
        RECT client; GetClientRect(window,&client); int w=client.right,h=client.bottom;
        HDC memory=CreateCompatibleDC(screen); HBITMAP bitmap=CreateCompatibleBitmap(screen,w,h);
        HGDIOBJ old=SelectObject(memory,bitmap); Paint(memory,w,h);
        BitBlt(screen,0,0,w,h,memory,0,0,SRCCOPY);
        SelectObject(memory,old); DeleteObject(bitmap); DeleteDC(memory);
        EndPaint(window,&ps); return 0;
    }
    case WM_LBUTTONDBLCLK: return 0;
    case WM_LBUTTONDOWN: {
        RECT client; GetClientRect(window,&client);
        int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
        if (!gFullscreen && Inside({134,client.bottom-65,240,client.bottom-15},x,y)) {
            gDraggingVolume=true;
            SetCapture(window);
            SetVolumeFromX(x);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (gDraggingVolume) { SetVolumeFromX(GET_X_LPARAM(lp)); return 0; }
        break;
    case WM_CAPTURECHANGED: gDraggingVolume=false; break;
    case WM_MOUSEWHEEL: {
        gWheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);
        bool changed=false;
        while (gWheelRemainder>=WHEEL_DELTA) {
            SetPlayerVolume(gVolume+0.05);
            gWheelRemainder-=WHEEL_DELTA;
            changed=true;
        }
        while (gWheelRemainder<=-WHEEL_DELTA) {
            SetPlayerVolume(gVolume-0.05);
            gWheelRemainder+=WHEEL_DELTA;
            changed=true;
        }
        if (changed) ShowVolumeHud();
        return 0;
    }
    case WM_LBUTTONUP: {
        RECT client; GetClientRect(window,&client); int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);
        if (gDraggingVolume) {
            SetVolumeFromX(x);
            gDraggingVolume=false;
            ReleaseCapture();
            return 0;
        }
        if (!gFullscreen && x < kSidebarWidth && y >= 70 && y < 70+kChannelCount*62) {
            int channel=(y-70)/62;
            if (y < 70+channel*62+50) SelectChannel(channel);
            return 0;
        }
        if (Inside({20,client.bottom-65,62,client.bottom-15},x,y) && gEngine) {
            if (gEngine->IsPaused()) gEngine->Play(); else gEngine->Pause();
        } else if (Inside({73,client.bottom-65,123,client.bottom-15},x,y) && gEngine) {
            gMuted=!gMuted; gEngine->SetMuted(gMuted?TRUE:FALSE);
        }
        Redraw(); return 0;
    }
    case WM_KEYDOWN:
        if (wp==VK_ESCAPE && gFullscreen) Fullscreen();
        else if (wp==VK_SPACE && gEngine) {
            if (gEngine->IsPaused()) gEngine->Play(); else gEngine->Pause(); Redraw();
        } else if (wp=='M' && gEngine) { gMuted=!gMuted; gEngine->SetMuted(gMuted); Redraw(); }
        return 0;
    case kStart:
        if (FAILED(Connect())) Status(L"Ошибка подключения — проверьте доступность эфира");
        return 0;
    case kMediaEvent:
        if (wp==MF_MEDIA_ENGINE_EVENT_ERROR) Status(L"Ошибка воспроизведения — проверьте источник и подключение");
        else if (wp==MF_MEDIA_ENGINE_EVENT_PLAYING) Status(L"Идёт прямой эфир");
        else if (wp==MF_MEDIA_ENGINE_EVENT_PAUSE) Status(L"Пауза");
        else if (wp==MF_MEDIA_ENGINE_EVENT_BUFFERINGSTARTED) Status(L"Загрузка эфира…");
        else if (wp==MF_MEDIA_ENGINE_EVENT_BUFFERINGENDED) Status(L"Идёт прямой эфир");
        Redraw(); return 0;
    case WM_DESTROY:
        if (gGuide) DestroyWindow(gGuide);
        gWindow=nullptr; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput gdiInput;
    if (Gdiplus::GdiplusStartup(&gGdiToken,&gdiInput,nullptr)==Gdiplus::Ok) {
        LoadPinImage(instance);
        LoadRadioImage(instance);
    }
    WNDCLASSEXW wc={}; wc.cbSize=sizeof(wc);
    wc.style=CS_DBLCLKS; wc.lpfnWndProc=WindowProc;
    wc.hInstance=instance; wc.lpszClassName=L"RussiaOneNativePlayer";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,
                               32,32,LR_DEFAULTCOLOR);
    wc.hIconSm=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,
                                 16,16,LR_DEFAULTCOLOR);
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassExW(&wc)) return 1;
    WNDCLASSW artClass={}; artClass.lpfnWndProc=RadioArtProc;
    artClass.hInstance=instance; artClass.lpszClassName=L"RussiaOneRadioArt";
    artClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if (!RegisterClassW(&artClass)) return 1;
    WNDCLASSW hudClass={}; hudClass.lpfnWndProc=HudProc;
    hudClass.hInstance=instance; hudClass.lpszClassName=L"RussiaOneVolumeHud";
    hudClass.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassW(&hudClass)) return 1;
    WNDCLASSW guideClass={}; guideClass.lpfnWndProc=GuideProc;
    guideClass.hInstance=instance; guideClass.lpszClassName=L"RussiaOneGuide";
    guideClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    guideClass.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassW(&guideClass)) return 1;
    gWindow=CreateWindowExW(0,wc.lpszClassName,L"Россия 1HD — прямой эфир",
                           WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,
                           1020,650,nullptr,nullptr,instance,nullptr);
    if (!gWindow) return 1;
    gGuide=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED,guideClass.lpszClassName,L"EPG",
                         WS_POPUP|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,
                         620,440,gWindow,nullptr,instance,nullptr);
    if (gGuide) SetLayeredWindowAttributes(gGuide,0,224,LWA_ALPHA);
    BOOL dark=TRUE;
    COLORREF caption=color(16,24,39), titleColor=color(245,248,255);
    DwmSetWindowAttribute(gWindow,20,&dark,sizeof(dark));
    DwmSetWindowAttribute(gWindow,35,&caption,sizeof(caption));
    DwmSetWindowAttribute(gWindow,36,&titleColor,sizeof(titleColor));
    gVideo=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_BLACKRECT|SS_NOTIFY,
                           0,0,100,100,gWindow,nullptr,instance,nullptr);
    gOldVideoProc=(WNDPROC)SetWindowLongPtrW(gVideo,GWLP_WNDPROC,(LONG_PTR)VideoProc);
    gRadioArt=CreateWindowExW(0,artClass.lpszClassName,L"",WS_CHILD,
                              0,0,100,100,gWindow,nullptr,instance,nullptr);
    if (!gRadioArt) { DestroyWindow(gWindow); return 1; }
    gHud=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|
                          WS_EX_TRANSPARENT|WS_EX_LAYERED,
                          hudClass.lpszClassName,L"",WS_POPUP,
                          0,0,240,78,gWindow,nullptr,instance,nullptr);
    if (gHud) {
        SetLayeredWindowAttributes(gHud,0,235,LWA_ALPHA);
        SetWindowRgn(gHud,CreateRoundRectRgn(0,0,240,78,18,18),TRUE);
    }
    gPin=CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                         0,0,36,40,gWindow,reinterpret_cast<HMENU>(201),instance,nullptr);
    gEpg=CreateWindowExW(0,L"BUTTON",L"EPG",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                         0,0,76,40,gWindow,reinterpret_cast<HMENU>(202),instance,nullptr);
    gTooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
                             WS_POPUP|TTS_ALWAYSTIP,CW_USEDEFAULT,CW_USEDEFAULT,
                             CW_USEDEFAULT,CW_USEDEFAULT,gWindow,nullptr,instance,nullptr);
    if (gTooltip && gPin) {
        TOOLINFOW tool={}; tool.cbSize=sizeof(tool);
        tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS; tool.hwnd=gWindow;
        tool.uId=(UINT_PTR)gPin;
        tool.lpszText=const_cast<LPWSTR>(L"Поверх всех окон");
        SendMessageW(gTooltip,TTM_ADDTOOLW,0,(LPARAM)&tool);
    }
    if (gTooltip && gEpg) {
        TOOLINFOW tool={}; tool.cbSize=sizeof(tool);
        tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS; tool.hwnd=gWindow;
        tool.uId=(UINT_PTR)gEpg;
        tool.lpszText=const_cast<LPWSTR>(L"Программа передач");
        SendMessageW(gTooltip,TTM_ADDTOOLW,0,(LPARAM)&tool);
    }
    RECT client; GetClientRect(gWindow,&client); Layout(client.right,client.bottom);
    ShowWindow(gWindow,show);
    HRESULT hr=InitializePlayer();
    if (FAILED(hr)) {
        wchar_t error[128]; swprintf_s(error,L"Не удалось открыть плеер (0x%08X).",(unsigned)hr);
        MessageBoxW(gWindow,error,L"Россия 1",MB_ICONERROR); DestroyWindow(gWindow); return 1;
    }
    PostMessageW(gWindow,kStart,0,0);
    MSG message;
    while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if(gEngine) { gEngine->Shutdown(); gEngine.Reset(); }
    MFShutdown(); CoUninitialize();
    delete gPinImage;
    if (gPinStream) gPinStream->Release();
    delete gRadioImage;
    if (gRadioStream) gRadioStream->Release();
    if (gGdiToken) Gdiplus::GdiplusShutdown(gGdiToken);
    return 0;
}
