#pragma once

#include <windows.h>
#include <wrl.h>
#include <string>
#include <functional>
#include "../third_party/webview2/build/native/include/WebView2.h"

class WebViewWindow {
public:
    WebViewWindow(HWND hWnd, const std::wstring& rootDir);
    ~WebViewWindow();

    bool Init(std::function<void(bool success)> onCompleted);
    void Resize();
    void PostWebMessage(const std::string& jsonString);
    void PostWebMessageDirect(const std::string& jsonString);

    bool IsReady() const { return m_isReady; }

private:
    HWND m_hWnd = NULL;
    DWORD m_uiThreadId = 0;
    std::wstring m_rootDir;
    bool m_isReady = false;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;

    std::wstring GetUserDataDir() const;
    std::wstring GetResourcesDir() const;
};
