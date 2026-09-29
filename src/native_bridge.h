#pragma once

#include <windows.h>
#include <string>
#include <functional>
#include "../third_party/json.hpp"

class NativeBridge {
public:
    static NativeBridge& Instance();

    void Initialize(HWND hWnd, std::function<void(const std::string&)> sendWebMessageFunc);
    std::string HandleMessage(const std::string& jsonString);
    void PushLogLine(const std::string& logLine);

private:
    NativeBridge();
    ~NativeBridge();

    HWND m_hWnd = NULL;
    std::function<void(const std::string&)> m_sendWebMessage;
};
