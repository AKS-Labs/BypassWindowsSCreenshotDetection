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

static BOOL CALLBACK _EnumWndCb(HWND h, LPARAM lp)
{
    auto* a = (EnumWndArgs*)lp;
    RECT r{}; GetWindowRect(h, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);
    if (area > a->area) { a->area = area; a->hwnd = h; }
    return TRUE;
}

static HWND GetMainWindow()
{
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

// ── MinHook wrapper ──────────────────────────────────────────
template<typename T>
inline MH_STATUS MH_Hook(LPVOID target, LPVOID detour, T** original)
{ return MH_CreateHook(target, detour, reinterpret_cast<LPVOID*>(original)); }

// ── Globals ──────────────────────────────────────────────────
static HWND    g_hwnd      = nullptr;
static BOOL    g_unfocused = FALSE;
static WNDPROC g_oldProc   = nullptr;

static decltype(GetForegroundWindow)*      real_GFW  = nullptr;
static decltype(SetCursorPos)*             real_SCP  = nullptr;
static decltype(SetWindowDisplayAffinity)* real_SWDA = nullptr;

static volatile LONG g_bypassActive  = 0;
static HANDLE        g_bypassThread  = nullptr;
static HMODULE       g_hModule       = nullptr;

// ── Detours ──────────────────────────────────────────────────
static HWND WINAPI Detour_GFW()               { return g_hwnd; }
static BOOL WINAPI Detour_SCP(int X, int Y)   { return g_unfocused ? TRUE : real_SCP(X, Y); }

static BOOL WINAPI Detour_SWDA(HWND hWnd, DWORD /*affinity*/)
{
    // Swallow ANY protection request – always keep window capturable
    if (real_SWDA) real_SWDA(hWnd, WDA_NONE);
    return TRUE;
}

// ── WndProc ──────────────────────────────────────────────────
static LRESULT CALLBACK NewWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCACTIVATE:
        if (wp == TRUE)  { g_unfocused = FALSE; break; }
        if (wp == FALSE) { g_unfocused = TRUE;  return 0; }
        break;
    case WM_ACTIVATE:    if (wp == WA_INACTIVE) return 0; break;
    case WM_ACTIVATEAPP: if (wp == FALSE)       return 0; break;
    case WM_KILLFOCUS:                          return 0;
    case WM_IME_SETCONTEXT: if (wp == FALSE)    return 0; break;
    }
    return CallWindowProc(g_oldProc, h, msg, wp, lp);
}

// ── Screenshot bypass helpers ─────────────────────────────────
static BOOL CALLBACK _ResetChild(HWND h, LPARAM)
{
    SetWindowDisplayAffinity(h, WDA_NONE);
    return TRUE;
}

static void StripAllProtection()
{
    DWORD pid = GetCurrentProcessId();
    EnumWindows([](HWND h, LPARAM pid) -> BOOL {
        DWORD wp = 0;
        GetWindowThreadProcessId(h, &wp);
        if (wp == (DWORD)pid) {
            SetWindowDisplayAffinity(h, WDA_NONE);
            EnumChildWindows(h, _ResetChild, 0);
        }
        return TRUE;
    }, (LPARAM)pid);
}

// Background thread – re-strips every 250 ms in case app fights back
static DWORD WINAPI BypassThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_bypassActive, 0, 0))
    {
        StripAllProtection();
        Sleep(250);
    }
    return 0;
}

static void StartScreenshotBypass()
{
    // Hook SetWindowDisplayAffinity
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        void* target = GetProcAddress(u32, "SetWindowDisplayAffinity");
        if (target && real_SWDA == nullptr) {
            real_SWDA = SetWindowDisplayAffinity; // fallback
            if (MH_Hook(target, Detour_SWDA, &real_SWDA) == MH_OK)
                MH_EnableHook(target);
        }
    }

    // Immediately strip existing protection
    StripAllProtection();

    // Start the keep-alive thread
    if (InterlockedExchange(&g_bypassActive, 1) == 0)
        g_bypassThread = CreateThread(nullptr, 0, BypassThread, nullptr, 0, nullptr);
}

static void StopScreenshotBypass()
{
    InterlockedExchange(&g_bypassActive, 0);
    if (g_bypassThread) {
        WaitForSingleObject(g_bypassThread, 1500);
        CloseHandle(g_bypassThread);
        g_bypassThread = nullptr;
    }
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        void* target = GetProcAddress(u32, "SetWindowDisplayAffinity");
        if (target) MH_DisableHook(target);
    }
    real_SWDA = nullptr;
}

// ── Init thread (safe DllMain alternative) ───────────────────
//  Reads the named flag written by the GUI before injection.
static DWORD WINAPI InitThread(LPVOID)
{
    // Give the loader a moment to finish
    Sleep(50);

    // GUI creates this named event ONLY when bypass is requested
    wchar_t evtName[128];
    swprintf_s(evtName, L"Local\\NFL_Bypass_%lu", GetCurrentProcessId());
    HANDLE hEvt = OpenEventW(SYNCHRONIZE, FALSE, evtName);
    bool doBypass = (hEvt != nullptr);
    if (hEvt) CloseHandle(hEvt);

    if (doBypass)
        StartScreenshotBypass();

    return 0;
}

// ── DllMain ──────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        g_hModule = hMod;
        DisableThreadLibraryCalls(hMod);

        MH_Initialize();

        g_hwnd = GetMainWindow();

        // Focus-loss hooks
        real_GFW = GetForegroundWindow;
        real_SCP = SetCursorPos;
        MH_Hook(GetForegroundWindow, Detour_GFW, &real_GFW);
        MH_Hook(SetCursorPos,        Detour_SCP, &real_SCP);
        MH_EnableHook(MH_ALL_HOOKS);

        if (g_hwnd)
            g_oldProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc);

        // Screenshot bypass decision is made in a separate thread
        // (avoids loader-lock issues in DllMain)
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        break;
    }
    case DLL_PROCESS_DETACH:
    {
        StopScreenshotBypass();

        if (g_hwnd && g_oldProc)
            SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldProc);

        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        break;
    }
    }
    return TRUE;
}