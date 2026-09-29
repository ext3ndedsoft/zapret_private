#include "webview_window.h"
#include "native_bridge.h"
#include "resource.h"
#include "../third_party/webview2/build/native/include/WebView2EnvironmentOptions.h"
#include <shlobj.h>
#include <filesystem>
#include <thread>
#include <fstream>

using namespace Microsoft::WRL;
namespace fs = std::filesystem;

static void LogWV(const std::string& msg) {
    std::ofstream f("C:\\Users\\ecst4ssy\\Desktop\\Zapret Private\\debug.log", std::ios::app);
    if (f.is_open()) {
        f << msg << "\n";
        f.flush();
    }
}

WebViewWindow::WebViewWindow(HWND hWnd, const std::wstring& rootDir)
    : m_hWnd(hWnd), m_rootDir(rootDir) {
}

WebViewWindow::~WebViewWindow() {
    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webView = nullptr;
}

std::wstring WebViewWindow::GetResourcesDir() const {
    return m_rootDir + L"\\resources";
}

std::wstring WebViewWindow::GetUserDataDir() const {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        fs::path p(localAppData);
        p /= L"ZapretPrivate";
        p /= L"WebView2Profile";
        fs::create_directories(p);
        return p.wstring();
    }
    return m_rootDir + L"\\.wv2data";
}

bool WebViewWindow::Init(std::function<void(bool success)> onCompleted) {
    LogWV("Init started");
    m_uiThreadId = GetCurrentThreadId();
    std::wstring userDataDir = GetUserDataDir();
    auto options = Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        options->put_AdditionalBrowserArguments(
            L"--no-sandbox "
            L"--disable-features=msSmartScreenProtection "
            L"--renderer-process-limit=1"
        );
    }

    LogWV("Calling CreateCoreWebView2EnvironmentWithOptions with options and userDataDir");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataDir.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, onCompleted](HRESULT envResult, ICoreWebView2Environment* env) -> HRESULT {
                LogWV("Environment callback: result = " + std::to_string(envResult));
                if (FAILED(envResult) || !env) {
                    if (onCompleted) onCompleted(false);
                    return envResult;
                }

                LogWV("Calling CreateCoreWebView2Controller on m_hWnd=" + std::to_string((uintptr_t)m_hWnd));
                env->CreateCoreWebView2Controller(
                    m_hWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, onCompleted](HRESULT cResult, ICoreWebView2Controller* controller) -> HRESULT {
                            LogWV("Controller callback: result = " + std::to_string(cResult));
                            if (FAILED(cResult) || !controller) {
                                if (onCompleted) onCompleted(false);
                                return cResult;
                            }

                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webView);

                            // Dark background to prevent any white flash
                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(m_controller.As(&controller2)) && controller2) {
                                COREWEBVIEW2_COLOR darkCol = { 255, 7, 8, 10 };
                                controller2->put_DefaultBackgroundColor(darkCol);
                            }

                            // Virtual host mapping: https://zapret.local -> resources/
                            std::wstring resDir = GetResourcesDir();
                            ComPtr<ICoreWebView2_3> webView3;
                            if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
                                HRESULT mapHr = webView3->SetVirtualHostNameToFolderMapping(
                                    L"zapret.local",
                                    resDir.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
                                );
                                LogWV("SetVirtualHostNameToFolderMapping: " + std::to_string(mapHr));
                            }

                            // Browser Settings
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webView->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_IsBuiltInErrorPageEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE); // F12 or inspector for testing
                            }

                            // Hook WebMessage handler
                            EventRegistrationToken token;
                            m_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        HRESULT hrMsg = args->TryGetWebMessageAsString(&msg);
                                        if (FAILED(hrMsg) || !msg) {
                                            hrMsg = args->get_WebMessageAsJson(&msg);
                                        }
                                        if (msg) {
                                            int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, msg, -1, NULL, 0, NULL, NULL);
                                            std::string jsonStr(sizeNeeded > 1 ? sizeNeeded - 1 : 0, 0);
                                            if (sizeNeeded > 1) {
                                                WideCharToMultiByte(CP_UTF8, 0, msg, -1, &jsonStr[0], sizeNeeded, NULL, NULL);
                                            }
                                            CoTaskMemFree(msg);

                                            LogWV("Received WebMessage: " + jsonStr);
                                            bool isQuick = (jsonStr.find("\"window_") != std::string::npos ||
                                                            jsonStr.find("\"get_status\"") != std::string::npos ||
                                                            jsonStr.find("\"get_cached_network\"") != std::string::npos);

                                            if (isQuick) {
                                                std::string response = NativeBridge::Instance().HandleMessage(jsonStr);
                                                PostWebMessage(response);
                                            } else {
                                                std::thread([this, jsonStr]() {
                                                    std::string response = NativeBridge::Instance().HandleMessage(jsonStr);
                                                    PostWebMessage(response);
                                                }).detach();
                                            }
                                        }
                                        return S_OK;
                                    }).Get(),
                                &token
                            );

                            Resize();
                            m_controller->put_IsVisible(TRUE);

                            // Navigate to our virtual host
                            LogWV("Navigating to https://zapret.local/index.html");
                            m_webView->Navigate(L"https://zapret.local/index.html");

                            m_isReady = true;
                            if (onCompleted) onCompleted(true);
                            return S_OK;
                        }).Get()
                );
                return S_OK;
            }).Get()
    );
    LogWV("CreateCoreWebView2EnvironmentWithOptions returned: " + std::to_string(hr));

    return SUCCEEDED(hr);
}

void WebViewWindow::Resize() {
    if (m_controller && m_hWnd) {
        RECT bounds;
        GetClientRect(m_hWnd, &bounds);
        m_controller->put_Bounds(bounds);
    }
}

void WebViewWindow::PostWebMessage(const std::string& jsonString) {
    if (m_uiThreadId != 0 && GetCurrentThreadId() == m_uiThreadId) {
        PostWebMessageDirect(jsonString);
    } else if (m_hWnd) {
        std::string* pStr = new std::string(jsonString);
        if (!PostMessageW(m_hWnd, WM_POST_WEBVIEW_JSON, 0, reinterpret_cast<LPARAM>(pStr))) {
            delete pStr;
        }
    }
}

void WebViewWindow::PostWebMessageDirect(const std::string& jsonString) {
    if (m_webView) {
        int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, jsonString.c_str(), (int)jsonString.size(), NULL, 0);
        std::wstring wMsg(sizeNeeded, 0);
        MultiByteToWideChar(CP_UTF8, 0, jsonString.c_str(), (int)jsonString.size(), &wMsg[0], sizeNeeded);
        HRESULT hr = m_webView->PostWebMessageAsJson(wMsg.c_str());
        if (FAILED(hr)) {
            LogWV("PostWebMessageAsJson FAILED: " + std::to_string(hr));
        }
    }
}
