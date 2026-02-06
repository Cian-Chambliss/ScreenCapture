# ScreenCapture
Utility to capture window screenshots from a legacy Win32 app via a tiny DLL.

## Build
- MinGW (x64): `make`
- MinGW (Win32): `make ARCH=32`
- CMake + Visual Studio (x64):
  - `cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64`
  - `cmake --build build-vs --config Release`
- CMake + Visual Studio (Win32):
  - `cmake -S . -B build-vs32 -G "Visual Studio 17 2022" -A Win32`
  - `cmake --build build-vs32 --config Release`

Outputs: `ScreenCapture.dll` (plus import lib for your toolchain).

## Using The DLL
- Exported API: `extern "C" __declspec(dllexport) void WINAPI RecordScreen(const char* path)`
- Call `RecordScreen` once (per UI thread) with a UTF‑8 directory path; it:
  - Stores the base output directory
  - Installs a thread‑local `WH_GETMESSAGE` hook in the calling thread
- Press hotkeys in that thread’s message loop:
  - F11: capture the active top‑level window (uses `GetAncestor(hwnd, GA_ROOT)`).
  - Shift+F11: capture a composite of the active window and the window directly beneath it in Z‑order.
    - Background: the window under the active one, found with `GetWindow(GW_HWNDPREV)` (skips invisible)
    - Foreground: the active window; overlaid on top
    - Placement: exact screen positions from `GetWindowRect` for both; bitmap size = union of both rects

### What Gets Captured
- Single window (F11):
  - Starts with a screen copy of the window’s extended frame bounds (`DWMWA_EXTENDED_FRAME_BOUNDS` when available),
  - Then tries `PrintWindow(hwnd, ..., 0)` to refine content; falls back to the screen copy if not supported.
- Composite (Shift+F11):
  - Background: screen copy of the window beneath (avoids black rectangles)
  - Foreground: `PrintWindow` of the active window overlaid; falls back to screen copy if needed

### File Naming
- Output dir: the `path` you passed to `RecordScreen`
- File name: sanitized window title (or class) with `.png`
- Conflicts: auto‑appends `-N` to keep names unique

## Example (native C++)
```cpp
using RecordScreenFn = void (WINAPI*)(const char*);

HMODULE h = LoadLibraryA("ScreenCapture.dll");
auto record = reinterpret_cast<RecordScreenFn>(GetProcAddress(h, "RecordScreen"));
if (record) {
    record("C:/captures"); // enable F11/Shift+F11 in this thread
}
```

## Example (.NET P/Invoke)
```csharp
using System.Runtime.InteropServices;

class Cap {
    [DllImport("ScreenCapture.dll", EntryPoint = "RecordScreen",
        CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    public static extern void RecordScreen(string path);
}

// Call once on the UI thread:
Cap.RecordScreen(@"C:/captures");
```

## Notes
- Scope: the hook is thread‑local; call `RecordScreen` on each UI thread you want to enable.
- 32‑bit builds export stdcall; a `.def` keeps the undecorated name `RecordScreen` for imports.
- Requires: Windows, GDI+/User32/GDI32/Shlwapi; DWM (Vista+) for extended frame bounds when available.
- PrintWindow support varies by window type; when unsupported, the screen copy path ensures you still get a usable image.
