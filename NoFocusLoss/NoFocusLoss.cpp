#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "MinHook.h"
#include <TlHelp32.h>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

// ============================================================
//  GetMainWindow helper
//  (finds the largest window belonging to this process)
// ============================================================
#pragma region GetMainWindow

struct EnumWindowsCallbackArgs
{
	HWND hwnd = nullptr;
	int  area = -1;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hnd, LPARAM lParam)
{
	auto* args = reinterpret_cast<EnumWindowsCallbackArgs*>(lParam);
	RECT rect = {};
	::GetWindowRect(hnd, &rect);
	int area = (rect.right - rect.left) * (rect.bottom - rect.top);
	if (area > args->area) { args->area = area; args->hwnd = hnd; }
	return TRUE;
}

template<class CB>
static bool VisitProcessThreads(CB visitor)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) return false;
	THREADENTRY32 te = { sizeof(te) };
	if (Thread32First(snap, &te)) do { visitor(te); } while (Thread32Next(snap, &te));
	CloseHandle(snap);
	return true;
}

static HWND GetMainWindow()
{
	DWORD pid = GetCurrentProcessId();
	EnumWindowsCallbackArgs args{};
	VisitProcessThreads([&](THREADENTRY32 te) {
		if (te.th32OwnerProcessID == pid)
			EnumThreadWindows(te.th32ThreadID, EnumWindowsCallback, (LPARAM)&args);
	});
	return args.hwnd;
}
#pragma endregion

// ============================================================
//  MinHook helper
// ============================================================
template<typename T>
inline MH_STATUS MH_CreateHookEx(LPVOID pTarget, LPVOID pDetour, T** ppOriginal)
{
	return MH_CreateHook(pTarget, pDetour, reinterpret_cast<LPVOID*>(ppOriginal));
}

// ============================================================
//  State
// ============================================================
static HWND     g_hwnd        = nullptr;
static BOOL     g_unfocused   = FALSE;
static WNDPROC  g_OldWndProc  = nullptr;

// Screenshot bypass state
static volatile LONG  g_bypassActive = 0;
static HANDLE         g_bypassThread = nullptr;

// Original function pointers
static decltype(GetForegroundWindow)*       real_GetForegroundWindow = nullptr;
static decltype(SetCursorPos)*              real_SetCursorPos        = nullptr;
static decltype(SetWindowDisplayAffinity)*  real_SetWindowDisplayAffinity = nullptr;

// ============================================================
//  Detours
// ============================================================
static HWND WINAPI DetourGetForegroundWindow()
{
	return g_hwnd;
}

static BOOL WINAPI DetourSetCursorPos(int X, int Y)
{
	if (g_unfocused) return TRUE;
	return real_SetCursorPos(X, Y);
}

// Intercept every call the app makes to protect its window and silently ignore it
static BOOL WINAPI DetourSetWindowDisplayAffinity(HWND hWnd, DWORD /*dwAffinity*/)
{
	// Always tell the caller it succeeded, but set WDA_NONE
	if (real_SetWindowDisplayAffinity)
		real_SetWindowDisplayAffinity(hWnd, WDA_NONE);
	return TRUE;
}

// ============================================================
//  Window Procedure
// ============================================================
static LRESULT CALLBACK NewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_NCACTIVATE:
		if (wParam == TRUE)  { g_unfocused = FALSE; break; }
		if (wParam == FALSE) { g_unfocused = TRUE;  return 0; }
		break;
	case WM_ACTIVATE:
		if (wParam == WA_INACTIVE) return 0;
		break;
	case WM_ACTIVATEAPP:
		if (wParam == FALSE) return 0;
		break;
	case WM_KILLFOCUS:
		return 0;
	case WM_IME_SETCONTEXT:
		if (wParam == FALSE) return 0;
		break;
	}
	return CallWindowProc(g_OldWndProc, hWnd, msg, wParam, lParam);
}

// ============================================================
//  Screenshot bypass helpers
// ============================================================

// Called for every child window – strips protection
static BOOL CALLBACK StripAffinityChild(HWND hWnd, LPARAM)
{
	SetWindowDisplayAffinity(hWnd, WDA_NONE);
	return TRUE;
}

// Enumerate all top-level windows of this process and strip protection
static void StripAllWindowProtection()
{
	DWORD pid = GetCurrentProcessId();
	EnumWindows([](HWND hWnd, LPARAM pid) -> BOOL {
		DWORD wndPid = 0;
		GetWindowThreadProcessId(hWnd, &wndPid);
		if (wndPid == (DWORD)pid)
		{
			SetWindowDisplayAffinity(hWnd, WDA_NONE);
			EnumChildWindows(hWnd, StripAffinityChild, 0);
		}
		return TRUE;
	}, (LPARAM)pid);
}

// Background thread: repeatedly re-strips protection in case the app fights back
static DWORD WINAPI BypassThreadProc(LPVOID)
{
	while (InterlockedCompareExchange(&g_bypassActive, 0, 0) != 0)
	{
		StripAllWindowProtection();
		Sleep(200); // check 5x per second
	}
	return 0;
}

// ============================================================
//  Exported API  (no parameters – avoids CallExport quirks)
// ============================================================

// Enable screenshot bypass. Safe to call multiple times.
extern "C" __declspec(dllexport) void EnableScreenshotBypass()
{
	// Install SetWindowDisplayAffinity hook if not already done
	if (real_SetWindowDisplayAffinity == nullptr)
	{
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32)
		{
			void* target = GetProcAddress(user32, "SetWindowDisplayAffinity");
			if (target)
			{
				real_SetWindowDisplayAffinity = SetWindowDisplayAffinity; // default fallback
				MH_STATUS s = MH_CreateHookEx(target, DetourSetWindowDisplayAffinity,
				                              &real_SetWindowDisplayAffinity);
				if (s == MH_OK)
					MH_EnableHook(target);
			}
		}
	}

	// Do an immediate strip
	StripAllWindowProtection();

	// Start background thread if not running
	if (InterlockedExchange(&g_bypassActive, 1) == 0)
	{
		g_bypassThread = CreateThread(nullptr, 0, BypassThreadProc, nullptr, 0, nullptr);
	}
}

// Disable screenshot bypass
extern "C" __declspec(dllexport) void DisableScreenshotBypass()
{
	InterlockedExchange(&g_bypassActive, 0);
	if (g_bypassThread)
	{
		WaitForSingleObject(g_bypassThread, 1000);
		CloseHandle(g_bypassThread);
		g_bypassThread = nullptr;
	}

	// Unhook SetWindowDisplayAffinity
	if (real_SetWindowDisplayAffinity)
	{
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32)
		{
			void* target = GetProcAddress(user32, "SetWindowDisplayAffinity");
			if (target) MH_DisableHook(target);
		}
		real_SetWindowDisplayAffinity = nullptr;
	}
}

// ============================================================
//  DLL Entry Point
// ============================================================
BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD fdwReason, LPVOID /*lpReserved*/)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
	{
		MH_Initialize();

		g_hwnd = GetMainWindow();

		real_GetForegroundWindow = GetForegroundWindow;
		real_SetCursorPos        = SetCursorPos;

		MH_CreateHookEx(GetForegroundWindow, DetourGetForegroundWindow, &real_GetForegroundWindow);
		MH_CreateHookEx(SetCursorPos,        DetourSetCursorPos,        &real_SetCursorPos);

		if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
			return FALSE;

		if (g_hwnd)
			g_OldWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)NewWndProc);

		break;
	}

	case DLL_PROCESS_DETACH:
	{
		// Stop bypass thread cleanly
		DisableScreenshotBypass();

		if (g_hwnd && g_OldWndProc)
			SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_OldWndProc);

		MH_DisableHook(MH_ALL_HOOKS);
		MH_Uninitialize();
		break;
	}
	}
	return TRUE;
}