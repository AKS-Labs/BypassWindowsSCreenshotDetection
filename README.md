# NoFocusLoss

> A Windows DLL injection utility that keeps apps running normally when unfocused, and optionally bypasses screenshot/capture protection.

![GitHub Actions](https://github.com/AKS-Labs/BypassWindowsSCreenshotDetection/actions/workflows/build.yml/badge.svg)

---

## Features

| Feature | What it does |
|---|---|
| 🎮 **Fix Focus Loss** | Prevents games and apps from pausing, muting, or losing controller input when you alt-tab |
| 📷 **Bypass Screenshot** | Removes the black-screen protection so Snipping Tool, OCR (`Win+Shift+T`), and screen recorders can capture the window |
| ✨ **Both Features** | Applies both at once |

Each feature can be injected **independently** — use only what you need.

---

## Download

Go to [**Actions**](../../actions) → latest successful build → download **`NoFocusLoss-Release`** artifact.

Extract the zip. You'll get:
```
NoFocusLossGUI.exe      ← Run this
NoFocusLoss.dll         ← 32-bit payload (don't move)
NoFocusLoss64.dll       ← 64-bit payload (don't move)
SharpestInjector.dll    ← Injector library (don't move)
```

---

## Usage

1. **Run** `NoFocusLossGUI.exe` (as Administrator if targeting elevated processes)
2. Click **Refresh** to list running windowed processes
3. Select the target process
4. Click one of the three inject buttons:
   - 🟢 **Fix Focus Loss** — stops the app from pausing/muting when unfocused
   - 🟣 **Bypass Screenshot** — removes black-screen capture protection
   - 🔵 **Both Features** — applies both
5. To revert, select the process in the bottom list and click **Unload**

---

## Use Cases

- **Multi-monitor gaming** — alt-tab to a second monitor without pausing your game or muting audio
- **Controller gaming in background** — play with a controller while the game window is not focused (XInput games only)
- **VR games with bad input handling** — bypass Unity's requirement that the window be focused for input
- **App developers / testers** — bypass screenshot protection you added to your own apps for testing purposes (OCR, screen capture, visual regression tests, etc.)
- **Cutscene skipping** — alt-tab freely without cutscenes pausing

---

## Requirements

- Windows 10 / 11 (x86 or x64)
- Administrator rights (recommended)
- Visual C++ Redistributable (usually already installed)

> **Windows 11** gets the full Mica translucent UI. Windows 10 falls back to a solid dark theme gracefully.

---

## Building from Source

### Prerequisites
- Visual Studio 2022 with:
  - **Desktop development with C++** workload
  - **.NET desktop development** workload
- Git (for submodules)

### Steps

```powershell
git clone https://github.com/AKS-Labs/BypassWindowsSCreenshotDetection.git
cd BypassWindowsSCreenshotDetection
git submodule update --init --recursive
```

Open `NoFocusLoss.sln` in Visual Studio → set configuration to **Release** → **Build Solution**.

Output will be in `NoFocusLossGUI\bin\Release\`.

### CI/CD

Every push to `main` automatically builds via GitHub Actions and uploads the artifact. See [`.github/workflows/build.yml`](.github/workflows/build.yml).

---

## ⚠ Caution

- **Do not use in multiplayer games** — anti-cheat systems detect DLL injection and will ban you
- **Use only on processes you own or have permission to modify**
- Injecting into the wrong process can cause crashes — only windowed processes are shown in the list
- This tool uses techniques that security software may flag as suspicious

---

## How it works (technical)

See **[HOW_APP_WORKS.md](HOW_APP_WORKS.md)** for a full technical breakdown including:
- LoadLibrary injection mechanism
- MinHook API hooking details
- Named Event IPC design
- Screenshot bypass background thread
- Cleanup and unloading

---

## Known Issues

- EA Origin overlay may appear frozen until you alt-tab once
- Apps using `Windows.Gaming.Input` (not XInput) may still lose controller input
- UWP / sandboxed apps cannot be injected into

## Tested On

- Teardown
- Alan Wake
- Need For Speed 2015
- I Expect You To Die 3
- The Long Dark

---

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu — inline API hooking
- [SharpestInjector](https://github.com/araghon007/SharpestInjector) — C# injection library
- Original NoFocusLoss concept and code by [@araghon007](https://github.com/araghon007)
