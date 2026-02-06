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
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExt, sizeof(rcExt)))) {
        // Prefer extended frame for seeding from screen (modern NC)
    }
    int extW = rcExt.right - rcExt.left;
    int extH = rcExt.bottom - rcExt.top;
    if (extW <= 0 || extH <= 0) return;

    // Seed with what is on screen (gets modern NC/title)
    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) return;
    HDC hExtDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hExtBmp = CreateCompatibleBitmap(hScreenDC, extW, extH);
    if (!hExtDC || !hExtBmp) {
        if (hExtBmp) DeleteObject(hExtBmp);
        if (hExtDC) DeleteDC(hExtDC);
        ReleaseDC(NULL, hScreenDC);
        return;
    }
    HBITMAP hExtOld = (HBITMAP)SelectObject(hExtDC, hExtBmp);
    BitBlt(hExtDC, 0, 0, extW, extH, hScreenDC, rcExt.left, rcExt.top, SRCCOPY);

    // Overlay client area only using PrintWindow to avoid legacy NC rendering
    RECT rcClient; GetClientRect(hwnd, &rcClient);
    POINT ptClient = {0, 0}; ClientToScreen(hwnd, &ptClient);
    int cW = rcClient.right - rcClient.left;
    int cH = rcClient.bottom - rcClient.top;
    if (cW > 0 && cH > 0) {
        HDC hCliDC = CreateCompatibleDC(hScreenDC);
        HBITMAP hCliBmp = CreateCompatibleBitmap(hScreenDC, cW, cH);
        if (hCliDC && hCliBmp) {
            HBITMAP hCliOld = (HBITMAP)SelectObject(hCliDC, hCliBmp);
            if (PrintWindow(hwnd, hCliDC, PW_CLIENTONLY)) {
                int dx = ptClient.x - rcExt.left;
                int dy = ptClient.y - rcExt.top;
                BitBlt(hExtDC, dx, dy, cW, cH, hCliDC, 0, 0, SRCCOPY);
            }
            SelectObject(hCliDC, hCliOld);
        }
        if (hCliBmp) DeleteObject(hCliBmp);
        if (hCliDC) DeleteDC(hCliDC);
    }

    SelectObject(hExtDC, hExtOld);
    ReleaseDC(NULL, hScreenDC);

    // Save
    Bitmap bitmap(hExtBmp, NULL);
    std::wstring path = GenerateFileName(hwnd);
    path = EnsureUniquePath(path);
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) >= 0) {
        bitmap.Save(path.c_str(), &pngClsid, NULL);
    }
    DeleteObject(hExtBmp);
    DeleteDC(hExtDC);
}

// Helper to get extended frame bounds; falls back to GetWindowRect
static BOOL GetExtendedRect(HWND hwnd, RECT* prc) {
    if (!IsWindow(hwnd) || !prc) return FALSE;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return FALSE;
    RECT rex = r;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rex, sizeof(rex)))) {
        *prc = rex;
        return TRUE;
    }
    *prc = r;
    return TRUE;
}

static HWND ResolveTopLevel(HWND h) {
    if (!IsWindow(h)) return NULL;
    return GetAncestor(h, GA_ROOT);
}

static HWND FindLikelyParentByPoint(HWND child) {
    if (!IsWindow(child)) return NULL;
    RECT rc{};
    if (!GetWindowRect(child, &rc)) return NULL;
    int cx = rc.left + (rc.right - rc.left) / 2;
    // Try a couple of offsets above the top edge to get the window under the dialog
    const int offsets[] = { 6, 14, 24 };
    for (int d : offsets) {
        POINT pt{ cx, rc.top - d };
        HWND h = WindowFromPoint(pt);
        if (h && IsWindowVisible(h)) {
            HWND top = ResolveTopLevel(h);
            if (top && top != ResolveTopLevel(child)) return top;
        }
    }
    // Fallback: owner chain
    return GetAncestor(child, GA_ROOTOWNER);
}

// Render a single window to a bitmap sized to its extended bounds. Includes modern NC/shadows
// by seeding from the screen, then overlays the client via PrintWindow to remove occlusions.
static HBITMAP RenderWindowBitmap(HWND hwnd, RECT* outExt) {
    if (outExt) SetRectEmpty(outExt);
    if (!IsWindow(hwnd)) return NULL;
    RECT rcWin{}; if (!GetWindowRect(hwnd, &rcWin)) return NULL;
    RECT rcExt = rcWin; GetExtendedRect(hwnd, &rcExt);
    int w = rcExt.right - rcExt.left;
    int h = rcExt.bottom - rcExt.top;
    if (w <= 0 || h <= 0) return NULL;

    HDC hScreen = GetDC(NULL);
    if (!hScreen) return NULL;
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    if (!hMem || !hBmp) {
        if (hBmp) DeleteObject(hBmp);
        if (hMem) DeleteDC(hMem);
        ReleaseDC(NULL, hScreen);
        return NULL;
    }
    HBITMAP hOld = (HBITMAP)SelectObject(hMem, hBmp);
    // Seed with what's on screen (modern NC and shadows)
    BitBlt(hMem, 0, 0, w, h, hScreen, rcExt.left, rcExt.top, SRCCOPY);

    // Overlay client content
    RECT rcCli{}; GetClientRect(hwnd, &rcCli);
    POINT ptCli{0,0}; ClientToScreen(hwnd, &ptCli);
    int cw = rcCli.right - rcCli.left;
    int ch = rcCli.bottom - rcCli.top;
    if (cw > 0 && ch > 0) {
        HDC hCli = CreateCompatibleDC(hScreen);
        HBITMAP hCliBmp = CreateCompatibleBitmap(hScreen, cw, ch);
        if (hCli && hCliBmp) {
            HBITMAP hCliOld = (HBITMAP)SelectObject(hCli, hCliBmp);
            if (PrintWindow(hwnd, hCli, PW_CLIENTONLY)) {
                int dx = ptCli.x - rcExt.left;
                int dy = ptCli.y - rcExt.top;
                BitBlt(hMem, dx, dy, cw, ch, hCli, 0, 0, SRCCOPY);
            } else {
                int dx = ptCli.x - rcExt.left;
                int dy = ptCli.y - rcExt.top;
                BitBlt(hMem, dx, dy, cw, ch, hScreen, ptCli.x, ptCli.y, SRCCOPY);
            }
            SelectObject(hCli, hCliOld);
        }
        if (hCliBmp) DeleteObject(hCliBmp);
        if (hCli) DeleteDC(hCli);
    }

    SelectObject(hMem, hOld);
    ReleaseDC(NULL, hScreen);
    DeleteDC(hMem);
    if (outExt) *outExt = rcExt;
    return hBmp;
}

// Blit helper: draw a bitmap at a given destination offset
static void BlitBitmap(HDC dst, int dx, int dy, HBITMAP bmp, int w, int h) {
    HDC src = CreateCompatibleDC(dst);
    if (!src) return;
    HBITMAP old = (HBITMAP)SelectObject(src, bmp);
    BitBlt(dst, dx, dy, w, h, src, 0, 0, SRCCOPY);
    SelectObject(src, old);
    DeleteDC(src);
}

// Capture union of owner (as background) then child (overlay)
static void CaptureWindowUnion(HWND child, HWND behind) {
    if (!IsWindow(child) || !IsWindow(behind)) return;

    RECT rcChildExt{}, rcBehindExt{};
    HBITMAP bmpChild = RenderWindowBitmap(child, &rcChildExt);
    HBITMAP bmpBehind = RenderWindowBitmap(behind, &rcBehindExt);
    if (!bmpChild || !bmpBehind) {
        if (bmpChild) DeleteObject(bmpChild);
        if (bmpBehind) DeleteObject(bmpBehind);
        return;
    }

    RECT rcU;
    rcU.left   = min(rcChildExt.left,   rcBehindExt.left);
    rcU.top    = min(rcChildExt.top,    rcBehindExt.top);
    rcU.right  = max(rcChildExt.right,  rcBehindExt.right);
    rcU.bottom = max(rcChildExt.bottom, rcBehindExt.bottom);
    int w = rcU.right - rcU.left;
    int h = rcU.bottom - rcU.top;
    if (w <= 0 || h <= 0) {
        DeleteObject(bmpChild);
        DeleteObject(bmpBehind);
        return;
    }

    HDC hScreen = GetDC(NULL);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hOut = CreateCompatibleBitmap(hScreen, w, h);
    if (!hMem || !hOut) {
        if (hOut) DeleteObject(hOut);
        if (hMem) DeleteDC(hMem);
        ReleaseDC(NULL, hScreen);
        DeleteObject(bmpChild);
        DeleteObject(bmpBehind);
        return;
    }
    HBITMAP old = (HBITMAP)SelectObject(hMem, hOut);
    RECT rFill = {0,0,w,h};
    FillRect(hMem, &rFill, (HBRUSH)GetStockObject(WHITE_BRUSH));

    // Draw parent then child at their offsets relative to union
    int pw = rcBehindExt.right - rcBehindExt.left;
    int ph = rcBehindExt.bottom - rcBehindExt.top;
    int cw = rcChildExt.right - rcChildExt.left;
    int ch = rcChildExt.bottom - rcChildExt.top;
    BlitBitmap(hMem, rcBehindExt.left - rcU.left, rcBehindExt.top - rcU.top, bmpBehind, pw, ph);
    BlitBitmap(hMem, rcChildExt.left - rcU.left, rcChildExt.top - rcU.top, bmpChild, cw, ch);

    SelectObject(hMem, old);

    // Save final composite
    Bitmap bitmap(hOut, NULL);
    std::wstring path = GenerateFileName(child);
    path = EnsureUniquePath(path);
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) >= 0) {
        bitmap.Save(path.c_str(), &pngClsid, NULL);
    }
    DeleteObject(hOut);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);
    DeleteObject(bmpChild);
    DeleteObject(bmpBehind);
}

static LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        MSG* pMsg = (MSG*)lParam;
        if (pMsg->message == WM_KEYUP && pMsg->wParam == VK_F11) {
            // The hwnd in the MSG is the window/control receiving the key.
            HWND hwndMsg = pMsg->hwnd;
            HWND root = hwndMsg ? GetAncestor(hwndMsg, GA_ROOT) : NULL;
            HWND rootOwner = hwndMsg ? GetAncestor(hwndMsg, GA_ROOTOWNER) : NULL;
            if (!root) {
                // Fallback if hwnd is null: foreground window
                root = GetForegroundWindow();
                rootOwner = root ? GetAncestor(root, GA_ROOTOWNER) : NULL;
            }

            // If Shift is down and the target is not the main application window,
            // include the parent (owner) as background.
            bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shiftDown && root) {
                // Determine parent by WindowFromPoint a few pixels above the centered top of the child
                HWND parentByPoint = FindLikelyParentByPoint(root);
                if (parentByPoint && parentByPoint != root) {
                    CaptureWindowUnion(root, parentByPoint);
                } else {
                    CaptureWindow(root);
                }
            } else if (root) {
                CaptureWindow(root);
            }
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
