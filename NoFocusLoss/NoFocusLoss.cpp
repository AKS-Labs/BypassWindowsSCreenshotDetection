#include <UIAutomation.h>
#include <Windows.h>
#include <stdio.h>
#include <TlHelp32.h>
#include "MinHook.h"

#define NOMINMAX

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

// ── GetMainWindow ─────────────────────────────────────────────
struct EnumWndArgs { HWND hwnd = nullptr; int area = -1; };
static BOOL CALLBACK _EnumWndCb(HWND h, LPARAM lp) {
    auto* a = (EnumWndArgs*)lp;
    RECT r{}; GetWindowRect(h, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);
    if (area > a->area) { a->area = area; a->hwnd = h; }
    return TRUE;
}
static HWND GetMainWindow() {
    DWORD pid = GetCurrentProcessId();
    EnumWndArgs args{};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{ sizeof(te) };
        if (Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID == pid)
                EnumThreadWindows(te.th32ThreadID, _EnumWndCb, (LPARAM)&args);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    return args.hwnd;
}

// ── MinHook helper ────────────────────────────────────────────
template<typename T>
inline MH_STATUS MH_Hook(LPVOID t, LPVOID d, T** o)
{ return MH_CreateHook(t, d, reinterpret_cast<LPVOID*>(o)); }

// ── State ─────────────────────────────────────────────────────
static HWND    g_hwnd      = nullptr;
static BOOL    g_unfocused = FALSE;
static WNDPROC g_oldProc   = nullptr;

static decltype(GetForegroundWindow)*      real_GFW       = nullptr;
static decltype(SetCursorPos)*             real_SCP       = nullptr;
static decltype(SetWindowDisplayAffinity)* real_SWDA      = nullptr;
static decltype(EmptyClipboard)*           real_Empty     = nullptr;

static volatile LONG g_bypassActive  = 0;
static volatile LONG g_textCopyActive = 0;
static HANDLE        g_bypassThread  = nullptr;

// ── UI Automation Helper ──────────────────────────────────────
static void GetTextViaUIA(HWND owner) {
    IUIAutomation* pAutomation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&pAutomation);
    if (FAILED(hr) || !pAutomation) return;

    POINT pt;
    GetCursorPos(&pt);

    IUIAutomationElement* pElement = nullptr;
    hr = pAutomation->ElementFromPoint(pt, &pElement);
    if (SUCCEEDED(hr) && pElement) {
        BSTR text = NULL;
        // Try to get Value first (for input fields)
        IUIAutomationValuePattern* pValuePattern = nullptr;
        pElement->GetCurrentPattern(UIA_ValuePatternId, (IUnknown**)&pValuePattern);
        if (pValuePattern) {
            pValuePattern->get_CurrentValue(&text);
            pValuePattern->Release();
        }

        // Fallback to Name or HelpText if Value is empty
        if (!text || SysStringLen(text) == 0) {
            pElement->get_CurrentName(&text);
        }

        if (text && SysStringLen(text) > 0) {
            if (!OpenClipboard(owner)) {
                SysFreeString(text);
                pElement->Release();
                pAutomation->Release();
                return;
            }
            if (real_Empty) real_Empty(); else EmptyClipboard();
            
            size_t len = SysStringLen(text);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
            if (hMem) {
                wchar_t* p = (wchar_t*)GlobalLock(hMem);
                if (p) { memcpy(p, text, (len + 1) * sizeof(wchar_t)); GlobalUnlock(hMem); }
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
            CloseClipboard();
            SysFreeString(text);
        }
        pElement->Release();
    }
    pAutomation->Release();
}

// ── Clipboard helpers ─────────────────────────────────────────
static void PutTextInClipboard(HWND owner, const wchar_t* text, int len) {
    if (!OpenClipboard(owner)) return;
    if (real_Empty) real_Empty(); else EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* p = (wchar_t*)GlobalLock(hMem);
        if (p) { memcpy(p, text, len * sizeof(wchar_t)); p[len] = 0; GlobalUnlock(hMem); }
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

static void CopyWindowText(HWND target, HWND clipOwner) {
    DWORD s = 0, e = 0;
    SendMessageW(target, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
    if (s != e) return;

    int len = GetWindowTextLengthW(target);
    if (len <= 0) {
        // Standard method failed, try UI Automation (works for browsers)
        GetTextViaUIA(clipOwner ? clipOwner : target);
        return;
    }

    wchar_t* buf = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (len + 1) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(target, buf, len + 1);
    PutTextInClipboard(clipOwner ? clipOwner : target, buf, len);
    HeapFree(GetProcessHeap(), 0, buf);
}

// ── EmptyClipboard detour ─────────────────────────────────────
static BOOL WINAPI Detour_EmptyClipboard() {
    if (InterlockedCompareExchange(&g_textCopyActive, 0, 0))
        return TRUE;
    return real_Empty ? real_Empty() : TRUE;
}

// ── Child window subclass proc ────────────────────────────────
static LRESULT CALLBACK ChildCopyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = (WNDPROC)(LONG_PTR)GetPropW(h, L"NFL_Orig");

    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (wp == 'C' || wp == VK_INSERT) {
            CopyWindowText(h, h);
        }
        if (wp == 'A') {
            SendMessageW(h, EM_SETSEL, 0, -1);
        }
    }
    if (msg == WM_NCDESTROY) {
        RemovePropW(h, L"NFL_Orig");
        if (orig) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
    }
    return orig ? CallWindowProcW(orig, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK SubclassChild(HWND h, LPARAM) {
    if (GetPropW(h, L"NFL_Orig")) return TRUE;
    WNDPROC cur = (WNDPROC)(LONG_PTR)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (cur && cur != ChildCopyProc) {
        SetPropW(h, L"NFL_Orig", (HANDLE)(LONG_PTR)cur);
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ChildCopyProc);
    }
    EnumChildWindows(h, SubclassChild, 0);
    return TRUE;
}

static void EnableTextCopy() {
    InterlockedExchange(&g_textCopyActive, 1);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && !real_Empty) {
        void* t = GetProcAddress(u32, "EmptyClipboard");
        if (t) { real_Empty = EmptyClipboard; MH_Hook(t, Detour_EmptyClipboard, &real_Empty); MH_EnableHook(t); }
    }
    if (g_hwnd) {
        SubclassChild(g_hwnd, 0);
        EnumChildWindows(g_hwnd, SubclassChild, 0);
    } else {
        DWORD pid = GetCurrentProcessId();
        EnumWindows([](HWND h, LPARAM pid) -> BOOL {
            DWORD wp = 0; GetWindowThreadProcessId(h, &wp);
            if (wp == (DWORD)pid) { SubclassChild(h, 0); EnumChildWindows(h, SubclassChild, 0); }
            return TRUE;
        }, (LPARAM)pid);
    }
}

// ── Focus-loss detours ────────────────────────────────────────
static HWND WINAPI Detour_GFW()             { return g_hwnd; }
static BOOL WINAPI Detour_SCP(int X, int Y) { return g_unfocused ? TRUE : real_SCP(X, Y); }

static LRESULT CALLBACK NewWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCACTIVATE:     if (wp == FALSE) { g_unfocused = TRUE;  return 0; }
                            else               g_unfocused = FALSE; break;
    case WM_ACTIVATE:       if (wp == WA_INACTIVE)  return 0; break;
    case WM_ACTIVATEAPP:    if (wp == FALSE)         return 0; break;
    case WM_KILLFOCUS:                               return 0;
    case WM_IME_SETCONTEXT: if (wp == FALSE)         return 0; break;

    case WM_KEYDOWN:
        if (InterlockedCompareExchange(&g_textCopyActive, 0, 0)) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && (wp == 'C' || wp == VK_INSERT)) {
                HWND focused = GetFocus();
                CopyWindowText(focused ? focused : h, h);
            }
            if (ctrl && wp == 'A') {
                HWND focused = GetFocus();
                if (focused) SendMessageW(focused, EM_SETSEL, 0, -1);
            }
        }
        break;
    }
    return CallWindowProc(g_oldProc, h, msg, wp, lp);
}

static void SetupFocusFix() {
    real_GFW = GetForegroundWindow;
    real_SCP = SetCursorPos;
    MH_Hook(GetForegroundWindow, Detour_GFW, &real_GFW);
    MH_Hook(SetCursorPos,        Detour_SCP, &real_SCP);
    MH_EnableHook(MH_ALL_HOOKS);
    g_hwnd = GetMainWindow();
    if (g_hwnd)
        g_oldProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc);
}

// ── Screenshot bypass ─────────────────────────────────────────
static BOOL WINAPI Detour_SWDA(HWND hWnd, DWORD) {
    if (real_SWDA) real_SWDA(hWnd, WDA_NONE); return TRUE;
}
static BOOL CALLBACK _ResetChild(HWND h, LPARAM) { SetWindowDisplayAffinity(h, WDA_NONE); return TRUE; }
static void StripAllProtection() {
    DWORD pid = GetCurrentProcessId();
    EnumWindows([](HWND h, LPARAM pid) -> BOOL {
        DWORD wp = 0; GetWindowThreadProcessId(h, &wp);
        if (wp == (DWORD)pid) { SetWindowDisplayAffinity(h, WDA_NONE); EnumChildWindows(h, _ResetChild, 0); }
        return TRUE;
    }, (LPARAM)pid);
}
static DWORD WINAPI BypassThread(LPVOID) {
    while (InterlockedCompareExchange(&g_bypassActive, 0, 0)) { StripAllProtection(); Sleep(250); }
    return 0;
}
static void StartScreenshotBypass() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && !real_SWDA) {
        void* t = GetProcAddress(u32, "SetWindowDisplayAffinity");
        if (t) { real_SWDA = SetWindowDisplayAffinity; MH_Hook(t, Detour_SWDA, &real_SWDA); MH_EnableHook(t); }
    }
    StripAllProtection();
    if (InterlockedExchange(&g_bypassActive, 1) == 0)
        g_bypassThread = CreateThread(nullptr, 0, BypassThread, nullptr, 0, nullptr);
}
static void StopScreenshotBypass() {
    InterlockedExchange(&g_bypassActive, 0);
    if (g_bypassThread) { WaitForSingleObject(g_bypassThread, 1500); CloseHandle(g_bypassThread); g_bypassThread = nullptr; }
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) { void* t = GetProcAddress(u32, "SetWindowDisplayAffinity"); if (t) MH_DisableHook(t); }
    real_SWDA = nullptr;
}

// ── Init thread ───────────────────────────────────────────────
static DWORD WINAPI InitThread(LPVOID) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    Sleep(100);
    wchar_t name[128];

    swprintf_s(name, 128, L"Local\\NFL_Focus_%lu", GetCurrentProcessId());
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, name);
    bool doFocus = h != nullptr; if (h) CloseHandle(h);

    swprintf_s(name, 128, L"Local\\NFL_Bypass_%lu", GetCurrentProcessId());
    h = OpenEventW(SYNCHRONIZE, FALSE, name);
    bool doBypass = h != nullptr; if (h) CloseHandle(h);

    swprintf_s(name, 128, L"Local\\NFL_TextCopy_%lu", GetCurrentProcessId());
    h = OpenEventW(SYNCHRONIZE, FALSE, name);
    bool doTextCopy = h != nullptr; if (h) CloseHandle(h);

    if (doFocus)    SetupFocusFix();
    if (doBypass)   StartScreenshotBypass();
    if (doTextCopy) EnableTextCopy();
    return 0;
}

// ── DllMain ───────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        MH_Initialize();
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        StopScreenshotBypass();
        InterlockedExchange(&g_textCopyActive, 0);
        if (g_hwnd && g_oldProc) SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldProc);
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        CoUninitialize();
    }
    return TRUE;
}