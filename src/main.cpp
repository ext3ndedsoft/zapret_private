#include "zapret_manager.h"
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <fstream>

#include "resource.h"
#include "native_bridge.h"
#include "webview_window.h"

namespace fs = std::filesystem;

static HWND g_hWnd = NULL;
static WebViewWindow* g_pWebViewWindow = nullptr;
static NOTIFYICONDATAW g_nid = { sizeof(g_nid) };
static bool g_trayIconActive = false;
static HICON g_hIcon = NULL;

static void AddTrayIcon(HWND hWnd) {
    if (!g_trayIconActive && hWnd) {
        g_nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY_ICON;
        g_nid.hIcon = g_hIcon;
        wcscpy_s(g_nid.szTip, L"Zapret Private");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_trayIconActive = true;
    }
}

static void RemoveTrayIcon() {
    if (g_trayIconActive) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconActive = false;
    }
}

static void ShowTrayContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();

    bool isRunning = ZapretManager::Instance().IsRunning();
    std::string mode = ZapretManager::Instance().GetRunMode();
    std::wstring statusStr = std::wstring(L"Статус: ") + (isRunning ? (mode == "service" ? L"Служба активна" : L"Процесс активен") : L"Отключен");

    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"Zapret Private");
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, statusStr.c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    if (!isRunning) {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_START, L"Запустить обход");
    } else {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_STOP, L"Остановить обход");
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_RESTART, L"Перезапустить");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"Открыть панель");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

static void LogMain(const std::string& msg) {
    // Debug logging disabled in release
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_CLOSE) {
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    switch (msg) {
        case WM_SIZE: {
            if (g_pWebViewWindow) {
                g_pWebViewWindow->Resize();
            }
            break;
        }

        case WM_TRAY_ICON: {
            if (lParam == WM_RBUTTONUP) {
                ShowTrayContextMenu(hWnd);
            } else if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            }
            break;
        }

        case WM_COMMAND: {
            WORD cmd = LOWORD(wParam);
            if (cmd == ID_TRAY_SHOW) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            } else if (cmd == ID_TRAY_START) {
                std::string err;
                ZapretManager::Instance().StartProcess("", err);
            } else if (cmd == ID_TRAY_STOP) {
                std::string err;
                ZapretManager::Instance().StopProcess(err);
            } else if (cmd == ID_TRAY_RESTART) {
                std::string err;
                ZapretManager::Instance().RestartProcess("", err);
            } else if (cmd == ID_TRAY_EXIT) {
                DestroyWindow(hWnd);
            }
            break;
        }

        case WM_POST_WEBVIEW_JSON: {
            std::string* pStr = reinterpret_cast<std::string*>(lParam);
            if (pStr) {
                if (g_pWebViewWindow) {
                    g_pWebViewWindow->PostWebMessageDirect(*pStr);
                }
                delete pStr;
            }
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pEx) {
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool IsProcessElevated() {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            fRet = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return fRet != FALSE;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    SetUnhandledExceptionFilter(CrashHandler);

    // Auto-elevate to Administrator immediately if not already elevated
    if (!IsProcessElevated()) {
        wchar_t szPath[MAX_PATH];
        if (GetModuleFileNameW(NULL, szPath, MAX_PATH)) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"runas";
            sei.lpFile = szPath;
            sei.lpParameters = pCmdLine;
            sei.hwnd = NULL;
            sei.nShow = (pCmdLine && wcsstr(pCmdLine, L"--minimized")) ? SW_HIDE : SW_NORMAL;
            if (ShellExecuteExW(&sei)) {
                return 0; // successfully launched elevated process, terminate this non-elevated instance
            }
        }
    }

    // Single instance mutex
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"ZapretPrivate_SingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingWnd = FindWindowW(L"ZapretPrivateWindowClass", NULL);
        if (existingWnd) {
            ShowWindow(existingWnd, SW_RESTORE);
            SetForegroundWindow(existingWnd);
        }
        return 0;
    }

    // Get current module path
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    fs::path rootPath = fs::path(exePath).parent_path();

    // Standalone Single-EXE check:
    // If running standalone without local folders, extract embedded payload to %LOCALAPPDATA%\ZapretPrivate
    if (!fs::exists(rootPath / "resources" / "index.html")) {
        wchar_t localApp[MAX_PATH];
        if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp) == S_OK) {
            fs::path appDataPath = fs::path(localApp) / "ZapretPrivate";
            ZapretManager::ExtractEmbeddedPayload(hInstance, appDataPath.wstring());
            if (fs::exists(appDataPath / "resources" / "index.html")) {
                rootPath = appDataPath;
            }
        }
    }
    SetCurrentDirectoryW(rootPath.c_str());

    // Register Window Class
    g_hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_hIcon) {
        g_hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    }

    const wchar_t CLASS_NAME[] = L"ZapretPrivateWindowClass";
    WNDCLASS wc = { };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_hIcon;
    wc.hbrBackground = CreateSolidBrush(RGB(7, 8, 10)); // Deep obsidian

    RegisterClass(&wc);

    // Determine window size and center
    int windowWidth = 1180;
    int windowHeight = 780;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - windowWidth) / 2;
    int posY = (screenHeight - windowHeight) / 2;

    // Create Main Window
    g_hWnd = CreateWindowEx(
        0,
        CLASS_NAME,
        L"Zapret Private",
        WS_OVERLAPPEDWINDOW,
        posX, posY, windowWidth, windowHeight,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hWnd) {
        return 1;
    }

    // Set Windows 11 Immersive Dark Mode
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    // Initialize System Tray
    AddTrayIcon(g_hWnd);

    // Initialize Zapret Core Engine
    ZapretManager::Instance().Initialize(rootPath.wstring());

    // Initialize WebView Window
    g_pWebViewWindow = new WebViewWindow(g_hWnd, rootPath.wstring());
    NativeBridge::Instance().Initialize(g_hWnd, [](const std::string& msg) {
        if (g_pWebViewWindow) {
            g_pWebViewWindow->PostWebMessage(msg);
        }
    });

    bool startMinimized = (pCmdLine && wcsstr(pCmdLine, L"--minimized") != nullptr);
    if (!startMinimized) {
        LogMain("Showing window SW_SHOW");
        ShowWindow(g_hWnd, SW_SHOW);
        UpdateWindow(g_hWnd);
    }

    LogMain("Calling g_pWebViewWindow->Init");
    g_pWebViewWindow->Init([](bool success) {
        LogMain(std::string("Init callback completed: ") + (success ? "TRUE" : "FALSE"));
    });

    LogMain("Entering GetMessage loop");
    // Message Loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogMain("Exited GetMessage loop");

    delete g_pWebViewWindow;
    g_pWebViewWindow = nullptr;

    ZapretManager::Instance().Shutdown();

    if (hMutex) {
        CloseHandle(hMutex);
    }

    return (int)msg.wParam;
}
