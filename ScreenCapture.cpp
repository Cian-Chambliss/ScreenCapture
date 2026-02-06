// ScreenCapture.cpp
// 32-bit Win32 DLL that captures a window screenshot when F11 is released.
// Exports: void RecordScreen(const char* path)

#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <string>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

static HINSTANCE g_hInst = NULL;
static HHOOK g_hHook = NULL;
static std::wstring g_basePath; // directory supplied by caller
static ULONG_PTR g_gdiplusToken = 0;

// Helper: get CLSID for PNG encoder
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT  num = 0;          // number of image encoders
    UINT  size = 0;         // size of the image encoder array in bytes

    GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;  // Failure

    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL)
        return -1; // Failure

    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j; // Success
        }
    }
    free(pImageCodecInfo);
    return -1; // Failure
}

static std::wstring GenerateFileName(HWND hwnd) {
    WCHAR title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    std::wstring name = title;
    if (name.empty()) {
        // fallback to class name
        WCHAR classname[256] = {0};
        GetClassNameW(hwnd, classname, 256);
        name = classname;
    }
    // Remove characters illegal in filenames
    for (auto& ch : name) {
        if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            ch = L'_';
        }
    }
    if (name.empty()) {
        name = L"window"; // fallback generic name
    }
    // Build full path
    WCHAR fullPath[MAX_PATH];
    wcscpy_s(fullPath, g_basePath.c_str());
    // Ensure trailing backslash
    size_t len = wcslen(fullPath);
    if (len > 0 && fullPath[len - 1] != L'\\') {
        wcscat_s(fullPath, L"\\");
    }
    wcscat_s(fullPath, name.c_str());
    // Append .png (handle duplicates later)
    wcscat_s(fullPath, L".png");
    return std::wstring(fullPath);
}

static std::wstring EnsureUniquePath(const std::wstring& path) {
    if (PathFileExistsW(path.c_str())) {
        // Insert -N before extension
        size_t dotPos = path.find_last_of(L'.');
        std::wstring base = (dotPos == std::wstring::npos) ? path : path.substr(0, dotPos);
        std::wstring ext = (dotPos == std::wstring::npos) ? L"" : path.substr(dotPos);
        int idx = 1;
        std::wstring candidate;
        do {
            std::wstringstream ss;
            ss << base << L"-" << idx << ext;
            candidate = ss.str();
            idx++;
        } while (PathFileExistsW(candidate.c_str()));
        return candidate;
    }
    return path;
}

static void CaptureWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return;

    RECT rcWin;
    if (!GetWindowRect(hwnd, &rcWin)) return;
    RECT rcExt = rcWin;
    // Try to get extended frame bounds (includes DWM shadows)
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExt, sizeof(rcExt)))) {
        // rcExt now may be larger than rcWin
    }
    int extW = rcExt.right - rcExt.left;
    int extH = rcExt.bottom - rcExt.top;
    if (extW <= 0 || extH <= 0) return;

    // Strategy: capture from screen first (visible content, includes border/shadow),
    // then try PrintWindow to overlay the exact window contents (helps with occlusion).
    HBITMAP hBitmap = NULL;
    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) return;
    HDC hExtDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hExtBmp = CreateCompatibleBitmap(hScreenDC, extW, extH);
    if (hExtBmp && hExtDC) {
        HBITMAP hExtOld = (HBITMAP)SelectObject(hExtDC, hExtBmp);
        // First, copy what's on screen over the extended bounds
        BitBlt(hExtDC, 0, 0, extW, extH, hScreenDC, rcExt.left, rcExt.top, SRCCOPY);

        // Now attempt PrintWindow into a window-sized temp and overlay into extended bmp
        int winW = rcWin.right - rcWin.left;
        int winH = rcWin.bottom - rcWin.top;
        HDC hWinDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hWinBmp = CreateCompatibleBitmap(hScreenDC, winW, winH);
        if (hWinDC && hWinBmp) {
            HBITMAP hWinOld = (HBITMAP)SelectObject(hWinDC, hWinBmp);
            BOOL ok = PrintWindow(hwnd, hWinDC, 0 /* flags 0 give more consistent output */);
            int dx = rcWin.left - rcExt.left;
            int dy = rcWin.top - rcExt.top;
            if (ok) {
                BitBlt(hExtDC, dx, dy, winW, winH, hWinDC, 0, 0, SRCCOPY);
            }
            SelectObject(hWinDC, hWinOld);
        }
        if (hWinBmp) DeleteObject(hWinBmp);
        if (hWinDC) DeleteDC(hWinDC);

        SelectObject(hExtDC, hExtOld);
        hBitmap = hExtBmp;
    } else if (hExtBmp) {
        DeleteObject(hExtBmp);
    }
    if (hExtDC) DeleteDC(hExtDC);
    ReleaseDC(NULL, hScreenDC);

    if (!hBitmap) return;

    // Convert HBITMAP to GDI+ Bitmap
    Bitmap bitmap(hBitmap, NULL);
    // Build filename
    std::wstring path = GenerateFileName(hwnd);
    path = EnsureUniquePath(path);
    // Get PNG encoder CLSID
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        DeleteObject(hBitmap);
        return;
    }
    // Save to file
    bitmap.Save(path.c_str(), &pngClsid, NULL);
    DeleteObject(hBitmap);
}

static LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        MSG* pMsg = (MSG*)lParam;
        if (pMsg->message == WM_KEYUP && pMsg->wParam == VK_F11) {
            // The hwnd in the MSG is the window/control receiving the key.
            HWND target = pMsg->hwnd;
            // Prefer the root of the parent chain (ignores owner), so a control inside
            // a modal dialog resolves to the dialog itself, not the main window owner.
            HWND root = target ? GetAncestor(target, GA_ROOT) : NULL;
            if (root) {
                target = root;
            } else {
                // Fallback if hwnd is null: foreground window
                HWND fg = GetForegroundWindow();
                if (fg) target = fg;
            }
            if (target) CaptureWindow(target);
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

extern "C" __declspec(dllexport) void WINAPI RecordScreen(const char* path) {
    // Store the supplied path (UTF‑8 to UTF‑16 conversion)
    if (path == NULL) return;
    int len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (len <= 0) return;
    std::wstring wpath(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], len);
    // Remove any trailing null character that std::wstring includes
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();
    g_basePath = wpath;

    // Install a thread‑local hook for the current thread.
    if (g_hHook) UnhookWindowsHookEx(g_hHook);
    g_hHook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgProc, g_hInst, GetCurrentThreadId());
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        g_hInst = hModule;
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
        break;
    }
    case DLL_PROCESS_DETACH: {
        if (g_hHook) {
            UnhookWindowsHookEx(g_hHook);
            g_hHook = NULL;
        }
        if (g_gdiplusToken) {
            GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
        }
        break;
    }
    default:
        break;
    }
    return TRUE;
}
