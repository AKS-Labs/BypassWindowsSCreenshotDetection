#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <stdio.h>
#include "MinHook.h"
#include <TlHelp32.h>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

// ── GetMainWindow ────────────────────────────────────────────
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

// ── MinHook helper ───────────────────────────────────────────
template<typename T>
inline MH_STATUS MH_Hook(LPVOID t, LPVOID d, T** o)
{ return MH_CreateHook(t, d, reinterpret_cast<LPVOID*>(o)); }

// ── State ────────────────────────────────────────────────────
static HWND    g_hwnd     = nullptr;
static BOOL    g_unfocused = FALSE;
static WNDPROC g_oldProc  = nullptr;

static decltype(GetForegroundWindow)*      real_GFW  = nullptr;
static decltype(SetCursorPos)*             real_SCP  = nullptr;
static decltype(SetWindowDisplayAffinity)* real_SWDA = nullptr;

static volatile LONG g_bypassActive = 0;
static HANDLE        g_bypassThread = nullptr;

// ── Focus-loss detours ───────────────────────────────────────
static HWND WINAPI Detour_GFW()             { return g_hwnd; }
static BOOL WINAPI Detour_SCP(int X, int Y) { return g_unfocused ? TRUE : real_SCP(X, Y); }

static LRESULT CALLBACK NewWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCACTIVATE:    if (wp == FALSE) { g_unfocused = TRUE;  return 0; }
                           else               g_unfocused = FALSE; break;
    case WM_ACTIVATE:      if (wp == WA_INACTIVE)  return 0; break;
    case WM_ACTIVATEAPP:   if (wp == FALSE)         return 0; break;
    case WM_KILLFOCUS:                              return 0;
    case WM_IME_SETCONTEXT: if (wp == FALSE)        return 0; break;
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

// ── Screenshot bypass ────────────────────────────────────────
static BOOL WINAPI Detour_SWDA(HWND hWnd, DWORD) {
    if (real_SWDA) real_SWDA(hWnd, WDA_NONE);
    return TRUE;
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

// ── Init thread: reads named events set by GUI before injection ──
// Local\NFL_Focus_{PID}  → enable focus loss fix
// Local\NFL_Bypass_{PID} → enable screenshot bypass
static DWORD WINAPI InitThread(LPVOID) {
    Sleep(100); // let LoadLibrary finish
    wchar_t name[128];
    swprintf_s(name, 128, L"Local\\NFL_Focus_%lu", GetCurrentProcessId());
    HANDLE hF = OpenEventW(SYNCHRONIZE, FALSE, name);
    bool doFocus = (hF != nullptr); if (hF) CloseHandle(hF);

    swprintf_s(name, 128, L"Local\\NFL_Bypass_%lu", GetCurrentProcessId());
    HANDLE hB = OpenEventW(SYNCHRONIZE, FALSE, name);
    bool doBypass = (hB != nullptr); if (hB) CloseHandle(hB);

    if (doFocus)  SetupFocusFix();
    if (doBypass) StartScreenshotBypass();
    return 0;
}

// ── DllMain ──────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        MH_Initialize();
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        StopScreenshotBypass();
        if (g_hwnd && g_oldProc) SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldProc);
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}