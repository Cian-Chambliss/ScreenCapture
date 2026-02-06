// ScreenCapture.cpp
// 32-bit Win32 DLL that captures a window screenshot when F11 is released.
// Exports: void RecordScreen(const char* path)

#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <string>
#include <sstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

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

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    // Capture relative to the window's own DC so origin is its top-left
    HDC hWndDC = GetWindowDC(hwnd);
    if (!hWndDC) return;
    HDC hMemDC = CreateCompatibleDC(hWndDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hWndDC, width, height);
    if (!hBitmap) {
        DeleteDC(hMemDC);
        ReleaseDC(hwnd, hWndDC);
        return;
    }
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hBitmap);
    BitBlt(hMemDC, 0, 0, width, height, hWndDC, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hOldBmp);
    DeleteDC(hMemDC);
    ReleaseDC(hwnd, hWndDC);

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
