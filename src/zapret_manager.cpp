#include "zapret_manager.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <chrono>
#include <winhttp.h>
#include <ws2tcpip.h>
#include <shlobj.h>

namespace fs = std::filesystem;

static std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

static std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], sizeNeeded);
    return wstrTo;
}

static void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t startPos = 0;
    while ((startPos = str.find(from, startPos)) != std::string::npos) {
        str.replace(startPos, from.length(), to);
        startPos += to.length();
    }
}

static bool IsValidUtf8(const std::string& str) {
    const unsigned char* bytes = (const unsigned char*)str.data();
    size_t len = str.size();
    size_t i = 0;
    while (i < len) {
        if (bytes[i] <= 0x7F) {
            i += 1;
        } else if ((bytes[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len || (bytes[i+1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((bytes[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len || (bytes[i+1] & 0xC0) != 0x80 || (bytes[i+2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((bytes[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len || (bytes[i+1] & 0xC0) != 0x80 || (bytes[i+2] & 0xC0) != 0x80 || (bytes[i+3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

static std::string SanitizeToUtf8(const std::string& raw) {
    if (raw.empty()) return "";
    if (IsValidUtf8(raw)) return raw;

    // Try CP_OEMCP (Russian Windows console output CP866)
    int wlen = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), NULL, 0);
    if (wlen > 0) {
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_OEMCP, 0, raw.data(), (int)raw.size(), &wstr[0], wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (ulen > 0) {
            std::string utf8(ulen, 0);
            WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &utf8[0], ulen, NULL, NULL);
            if (IsValidUtf8(utf8)) return utf8;
        }
    }

    // Try CP_ACP (Windows-1251)
    wlen = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), NULL, 0);
    if (wlen > 0) {
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &wstr[0], wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
        if (ulen > 0) {
            std::string utf8(ulen, 0);
            WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &utf8[0], ulen, NULL, NULL);
            if (IsValidUtf8(utf8)) return utf8;
        }
    }

    // Fallback: strip invalid bytes so json serialization never fails
    std::string safe;
    for (unsigned char c : raw) {
        if (c >= 32 && c <= 126) safe.push_back((char)c);
        else if (c == '\n' || c == '\r' || c == '\t') safe.push_back((char)c);
        else safe.push_back(' ');
    }
    return safe;
}

ZapretManager& ZapretManager::Instance() {
    static ZapretManager s_instance;
    return s_instance;
}

ZapretManager::ZapretManager() {
}

ZapretManager::~ZapretManager() {
    Shutdown();
}

void ZapretManager::Initialize(const std::wstring& rootDir) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_rootDir = rootDir;
    m_binDir = rootDir + L"\\bin";
    m_listsDir = rootDir + L"\\lists";
    m_utilsDir = rootDir + L"\\utils";

    LoadPresetsFromFile();

    // Check if service has installed strategy
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\zapret", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[256] = {0};
        DWORD bufSize = sizeof(buf);
        if (RegQueryValueExW(hKey, L"zapret-discord-youtube", NULL, NULL, (LPBYTE)buf, &bufSize) == ERROR_SUCCESS) {
            std::string strat = WStringToString(buf);
            if (!strat.empty()) {
                m_currentPresetId = strat;
            }
        }
        RegCloseKey(hKey);
    }
}

void ZapretManager::Shutdown() {
    // Only stop local process mode. If service is installed/running, keep it running permanently!
    if (m_isProcessRunning) {
        std::string err;
        StopProcess(err);
    }
}

void ZapretManager::LoadPresetsFromFile() {
    std::wstring presetsPath = m_rootDir + L"\\presets.json";
    std::ifstream file(presetsPath);
    if (file.is_open()) {
        try {
            nlohmann::json j;
            file >> j;
            std::vector<PresetInfo> loaded;
            for (const auto& item : j) {
                PresetInfo p;
                p.id = item.value("id", "");
                p.name = item.value("name", "");
                p.category = item.value("category", "General");
                p.description = item.value("description", "");
                p.args = item.value("args", "");
                if (!p.id.empty()) {
                    loaded.push_back(p);
                }
            }
            if (!loaded.empty()) {
                m_presets = loaded;
            }
        } catch (...) {
            // fallback
        }
    }

    if (m_presets.empty()) {
        // Fallback default
        PresetInfo def;
        def.id = "general";
        def.name = "general";
        def.category = "Recommended";
        def.description = "Базовая универсальная стратегия десинка";
        def.args = "--wf-tcp=80,443,2053,2083,2087,2096,8443,%GameFilterTCP% --wf-udp=443,19294-19344,50000-50100,%GameFilterUDP% --filter-udp=443 --hostlist=\"%LISTS%list-general.txt\" --dpi-desync=fake --dpi-desync-repeats=6 --dpi-desync-fake-quic=\"%BIN%quic_initial_www_google_com.bin\" --new --filter-udp=19294-19344,50000-50100 --filter-l7=discord,stun --dpi-desync=fake --dpi-desync-fake-discord=\"%BIN%ACTIVE_DISCORD_UDP.bin\" --dpi-desync-repeats=6 --new --filter-tcp=2053,2083,2087,2096,8443 --hostlist-domains=discord.media --dpi-desync=multisplit --dpi-desync-split-seqovl=681 --dpi-desync-split-pos=1 --new --filter-tcp=443 --hostlist=\"%LISTS%list-google.txt\" --dpi-desync=multisplit --dpi-desync-split-seqovl=681 --dpi-desync-split-pos=1 --new --filter-tcp=80,443 --hostlist=\"%LISTS%list-general.txt\" --dpi-desync=multisplit --dpi-desync-split-seqovl=568 --dpi-desync-split-pos=1";
        m_presets.push_back(def);
    }
}

std::vector<PresetInfo> ZapretManager::GetPresets() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_presets;
}

PresetInfo ZapretManager::GetPreset(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& p : m_presets) {
        if (p.id == id) return p;
    }
    if (!m_presets.empty()) return m_presets[0];
    return PresetInfo{};
}

std::string ZapretManager::GetCurrentPresetId() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_currentPresetId;
}

void ZapretManager::SetCurrentPresetId(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_currentPresetId = id;
}

std::string ZapretManager::GetGameFilterMode() {
    std::wstring flagFile = m_utilsDir + L"\\game_filter.enabled";
    if (!fs::exists(flagFile)) {
        return "disabled";
    }
    std::ifstream f(flagFile);
    if (!f.is_open()) return "disabled";
    std::string mode;
    f >> mode;
    std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
    if (mode == "all" || mode == "tcp" || mode == "udp") {
        return mode;
    }
    return "udp";
}

bool ZapretManager::SetGameFilterMode(const std::string& mode, std::string& errorMsg) {
    std::wstring flagFile = m_utilsDir + L"\\game_filter.enabled";
    try {
        if (mode == "disabled") {
            if (fs::exists(flagFile)) {
                fs::remove(flagFile);
            }
            AppendLog("[GameFilter] Режим выключен.");
            return true;
        }

        std::ofstream f(flagFile);
        if (!f.is_open()) {
            errorMsg = "Не удалось записать в game_filter.enabled";
            return false;
        }
        f << mode << "\n";
        AppendLog("[GameFilter] Установлен режим: " + mode);
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}

std::string ZapretManager::GetIPSetMode() {
    std::wstring listFile = m_listsDir + L"\\ipset-all.txt";
    if (!fs::exists(listFile)) return "none";
    try {
        auto sz = fs::file_size(listFile);
        if (sz == 0) return "any";

        std::ifstream f(listFile);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("203.0.113.113/32") != std::string::npos) {
                return "none";
            }
        }
        return "loaded";
    } catch (...) {
        return "loaded";
    }
}

bool ZapretManager::SetIPSetMode(const std::string& mode, std::string& errorMsg) {
    std::wstring listFile = m_listsDir + L"\\ipset-all.txt";
    std::wstring backupFile = m_listsDir + L"\\ipset-all.txt.backup";
    std::string current = GetIPSetMode();

    try {
        if (mode == "none") {
            if (current == "loaded" && fs::exists(listFile)) {
                if (fs::exists(backupFile)) fs::remove(backupFile);
                fs::rename(listFile, backupFile);
            }
            std::ofstream f(listFile);
            f << "203.0.113.113/32\n";
            AppendLog("[IPSet] Переключено в режим: NONE (обработка только списков доменов)");
            return true;
        } else if (mode == "any") {
            if (current == "loaded" && fs::exists(listFile)) {
                if (fs::exists(backupFile)) fs::remove(backupFile);
                fs::rename(listFile, backupFile);
            }
            std::ofstream f(listFile); // create empty
            AppendLog("[IPSet] Переключено в режим: ANY (фильтрация всего трафика)");
            return true;
        } else if (mode == "loaded") {
            if (fs::exists(backupFile)) {
                if (fs::exists(listFile)) fs::remove(listFile);
                fs::rename(backupFile, listFile);
                AppendLog("[IPSet] Восстановлен полный список IPSet (LOADED)");
                return true;
            } else {
                errorMsg = "Файл резервной копии ipset-all.txt.backup не найден";
                return false;
            }
        }
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}

std::string ZapretManager::BuildCommandLine(const std::string& presetArgs) {
    std::string args = presetArgs;
    std::string binPathA = WStringToString(m_binDir) + "\\";
    std::string listsPathA = WStringToString(m_listsDir) + "\\";

    std::string gfMode = GetGameFilterMode();
    std::string gfVal = "12";
    std::string gfTCP = "12";
    std::string gfUDP = "12";

    if (gfMode == "all") {
        gfVal = "1024-65535";
        gfTCP = "1024-65535";
        gfUDP = "1024-65535";
    } else if (gfMode == "tcp") {
        gfVal = "1024-65535";
        gfTCP = "1024-65535";
        gfUDP = "12";
    } else if (gfMode == "udp") {
        gfVal = "1024-65535";
        gfTCP = "12";
        gfUDP = "1024-65535";
    }

    ReplaceAll(args, "%BIN%", binPathA);
    ReplaceAll(args, "%LISTS%", listsPathA);
    ReplaceAll(args, "%GameFilterTCP%", gfTCP);
    ReplaceAll(args, "%GameFilterUDP%", gfUDP);
    ReplaceAll(args, "%GameFilter%", gfVal);

    // Build executable path
    std::string winwsExe = "\"" + binPathA + "winws.exe\"";
    return winwsExe + " " + args;
}

bool ZapretManager::StartProcess(const std::string& presetId, std::string& errorMsg) {
    std::string targetPreset = presetId.empty() ? GetCurrentPresetId() : presetId;
    PresetInfo preset = GetPreset(targetPreset);
    if (preset.args.empty()) {
        errorMsg = "Не найдены аргументы для пресета: " + targetPreset;
        return false;
    }

    // If already running as process, stop first
    if (m_isProcessRunning) {
        std::string dummy;
        StopProcess(dummy);
    }

    // If service is running, stop service first
    if (IsServiceRunning("zapret")) {
        AppendLog("[Service] Остановка службы zapret перед запуском процесса...");
        ExecCommandSync("net stop zapret");
    }

    // Kill any orphan winws.exe
    ExecCommandSync("taskkill /F /IM winws.exe");
    Sleep(200);

    // Ensure TCP timestamps enabled
    std::string tsRes;
    FixTcpTimestamps(tsRes);

    std::string fullCmd = BuildCommandLine(preset.args);
    AppendLog("[Zapret] Запуск: " + preset.name);

    // Set up pipes for stdout & stderr
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hPipeWrite = NULL;
    if (!CreatePipe(&m_hPipeRead, &hPipeWrite, &sa, 0)) {
        errorMsg = "Не удалось создать IPC Pipe для вывода winws";
        return false;
    }
    SetHandleInformation(m_hPipeRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hPipeWrite;
    si.hStdError = hPipeWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    std::wstring wCmd = StringToWString(fullCmd);

    std::vector<wchar_t> cmdBuffer(wCmd.begin(), wCmd.end());
    cmdBuffer.push_back(0);

    BOOL created = CreateProcessW(
        NULL,
        cmdBuffer.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        m_binDir.c_str(),
        &si,
        &pi
    );

    CloseHandle(hPipeWrite); // parent does not need write end

    if (!created) {
        DWORD err = GetLastError();
        CloseHandle(m_hPipeRead);
        m_hPipeRead = NULL;

        if (err == ERROR_ELEVATION_REQUIRED || err == ERROR_ACCESS_DENIED) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            std::wstring wExe = m_binDir + L"\\winws.exe";
            sei.lpFile = wExe.c_str();

            std::wstring wArgs = L"";
            size_t exeQuote = wCmd.find(L'\"', 1);
            if (exeQuote != std::wstring::npos && exeQuote + 2 < wCmd.length()) {
                wArgs = wCmd.substr(exeQuote + 2);
            }
            sei.lpParameters = wArgs.c_str();
            sei.lpDirectory = m_binDir.c_str();
            sei.nShow = SW_HIDE;

            if (ShellExecuteExW(&sei) && sei.hProcess) {
                m_hProcess = sei.hProcess;
                m_dwPid = GetProcessId(sei.hProcess);
                m_isProcessRunning = true;
                SetCurrentPresetId(targetPreset);
                AppendLog("[Zapret] Процесс winws.exe запущен с правами Администратора (PID: " + std::to_string(m_dwPid) + ")");
                return true;
            }
        }

        errorMsg = "CreateProcessW завершился с ошибкой: " + std::to_string(err);
        AppendLog("[ERROR] " + errorMsg);
        return false;
    }

    m_hProcess = pi.hProcess;
    m_hThread = pi.hThread;
    m_dwPid = pi.dwProcessId;
    m_isProcessRunning = true;
    SetCurrentPresetId(targetPreset);

    AppendLog("[Zapret] Процесс winws.exe запущен (PID: " + std::to_string(m_dwPid) + ")");

    // Start reader thread
    if (m_logReaderThread.joinable()) {
        m_logReaderThread.join();
    }
    m_logReaderThread = std::thread(&ZapretManager::ReadPipeLoop, this);

    return true;
}

void ZapretManager::ReadPipeLoop() {
    char buffer[1024];
    DWORD bytesRead = 0;
    std::string lineAccumulator;

    while (m_isProcessRunning && m_hPipeRead) {
        BOOL success = ReadFile(m_hPipeRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
        if (!success || bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = 0;
        lineAccumulator.append(buffer, bytesRead);

        size_t newlinePos;
        while ((newlinePos = lineAccumulator.find('\n')) != std::string::npos) {
            std::string line = lineAccumulator.substr(0, newlinePos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                AppendLog(line);
            }
            lineAccumulator.erase(0, newlinePos + 1);
        }
    }

    // Process exited or pipe broken
    m_isProcessRunning = false;
    if (m_dwPid > 0) {
        AppendLog("[Zapret] Процесс winws.exe завершил работу");
    }
}

bool ZapretManager::StopProcess(std::string& errorMsg) {
    if (!m_isProcessRunning && m_hProcess == NULL) {
        if (!IsServiceRunning("zapret")) {
            ExecCommandSync("taskkill /F /IM winws.exe");
            CleanupWinDivert();
        }
        return true;
    }

    m_isProcessRunning = false;

    if (m_hProcess != NULL) {
        TerminateProcess(m_hProcess, 0);
        WaitForSingleObject(m_hProcess, 1000);
        CloseHandle(m_hProcess);
        m_hProcess = NULL;
    }

    if (m_hThread != NULL) {
        CloseHandle(m_hThread);
        m_hThread = NULL;
    }

    if (m_hPipeRead != NULL) {
        CloseHandle(m_hPipeRead);
        m_hPipeRead = NULL;
    }

    if (m_logReaderThread.joinable()) {
        m_logReaderThread.join();
    }

    m_dwPid = 0;
    if (!IsServiceRunning("zapret")) {
        ExecCommandSync("taskkill /F /IM winws.exe");
        CleanupWinDivert();
    }
    AppendLog("[Zapret] Локальный процесс обхода остановлен");
    return true;
}

bool ZapretManager::RestartProcess(const std::string& presetId, std::string& errorMsg) {
    std::string id = presetId.empty() ? GetCurrentPresetId() : presetId;
    StopProcess(errorMsg);
    Sleep(300);
    return StartProcess(id, errorMsg);
}

void ZapretManager::CleanupWinDivert() {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm) {
        std::vector<std::wstring> drivers = { L"WinDivert", L"WinDivert14" };
        for (const auto& drv : drivers) {
            SC_HANDLE svc = OpenServiceW(scm, drv.c_str(), SERVICE_STOP | DELETE);
            if (svc) {
                SERVICE_STATUS ss;
                ControlService(svc, SERVICE_CONTROL_STOP, &ss);
                DeleteService(svc);
                CloseServiceHandle(svc);
            }
        }
        CloseServiceHandle(scm);
    }
}

std::wstring ZapretManager::GetServicePayloadDir() {
    wchar_t programData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, programData))) {
        fs::path p(programData);
        p /= L"Zapret";
        return p.wstring();
    }
    return L"C:\\ProgramData\\Zapret";
}

bool ZapretManager::DeployServicePayload(std::string& errorMsg) {
    try {
        fs::path targetDir = GetServicePayloadDir();
        fs::path targetBin = targetDir / L"bin";
        fs::path targetLists = targetDir / L"lists";

        fs::create_directories(targetBin);
        fs::create_directories(targetLists);

        if (fs::exists(m_binDir)) {
            for (const auto& entry : fs::directory_iterator(m_binDir)) {
                if (entry.is_regular_file()) {
                    fs::copy_file(entry.path(), targetBin / entry.path().filename(), fs::copy_options::overwrite_existing);
                }
            }
        }

        if (fs::exists(m_listsDir)) {
            for (const auto& entry : fs::directory_iterator(m_listsDir)) {
                if (entry.is_regular_file()) {
                    fs::copy_file(entry.path(), targetLists / entry.path().filename(), fs::copy_options::overwrite_existing);
                }
            }
        }

        AppendLog("[Service] Файлы службы успешно развернуты в " + WStringToString(targetDir.wstring()));
        return true;
    } catch (const std::exception& ex) {
        errorMsg = std::string("Ошибка копирования файлов службы в ProgramData: ") + ex.what();
        AppendLog("[Service ERROR] " + errorMsg);
        return false;
    }
}

std::string ZapretManager::BuildServiceCommandLine(const std::string& presetArgs) {
    std::string args = presetArgs;
    std::wstring serviceDirW = GetServicePayloadDir();
    std::string serviceDirA = WStringToString(serviceDirW);
    std::string binPathA = serviceDirA + "\\bin\\";
    std::string listsPathA = serviceDirA + "\\lists\\";

    std::string gfMode = GetGameFilterMode();
    std::string gfVal = "12";
    std::string gfTCP = "12";
    std::string gfUDP = "12";

    if (gfMode == "all") {
        gfVal = "1024-65535";
        gfTCP = "1024-65535";
        gfUDP = "1024-65535";
    } else if (gfMode == "tcp") {
        gfVal = "1024-65535";
        gfTCP = "1024-65535";
        gfUDP = "12";
    } else if (gfMode == "udp") {
        gfVal = "1024-65535";
        gfTCP = "12";
        gfUDP = "1024-65535";
    }

    ReplaceAll(args, "%BIN%", binPathA);
    ReplaceAll(args, "%LISTS%", listsPathA);
    ReplaceAll(args, "%GameFilterTCP%", gfTCP);
    ReplaceAll(args, "%GameFilterUDP%", gfUDP);
    ReplaceAll(args, "%GameFilter%", gfVal);

    std::string winwsExe = "\"" + binPathA + "winws.exe\"";
    return winwsExe + " " + args;
}

bool ZapretManager::InstallService(const std::string& presetId, std::string& errorMsg) {
    std::string targetPreset = presetId.empty() ? GetCurrentPresetId() : presetId;
    PresetInfo preset = GetPreset(targetPreset);
    if (preset.args.empty()) {
        errorMsg = "Не найден пресет: " + targetPreset;
        return false;
    }

    // Deploy payload to permanent system directory (C:\ProgramData\Zapret)
    // Anchors service permanently, independent of the app folder location!
    if (!DeployServicePayload(errorMsg)) {
        return false;
    }

    // Stop process if running
    if (m_isProcessRunning) {
        std::string dummy;
        StopProcess(dummy);
    }

    // Remove existing service first
    RemoveService(errorMsg);

    std::wstring wFullCmd = StringToWString(BuildServiceCommandLine(preset.args));

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            errorMsg = "Отказано в доступе. Запустите Zapret Private от имени Администратора.";
        } else {
            errorMsg = "Не удалось открыть диспетчер служб Windows (код: " + std::to_string(err) + ")";
        }
        AppendLog("[Service ERROR] " + errorMsg);
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        L"zapret",
        L"Zapret DPI Bypass Service",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        wFullCmd.c_str(),
        NULL,
        NULL,
        L"Tcpip\0BFE\0Dnscache\0\0",
        NULL,
        NULL
    );

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        errorMsg = "Ошибка создания службы Windows (код: " + std::to_string(err) + ")";
        AppendLog("[Service ERROR] " + errorMsg);
        return false;
    }

    // Set description
    SERVICE_DESCRIPTIONW sd;
    sd.lpDescription = (LPWSTR)L"Zapret Private - автономная постоянная служба десинхронизации DPI";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sd);

    // Auto-restart on failure (indestructible recovery)
    SC_ACTION actions[3];
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 2000;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 2000;
    actions[2].Type = SC_ACTION_RESTART;
    actions[2].Delay = 2000;

    SERVICE_FAILURE_ACTIONSW sfa;
    ZeroMemory(&sfa, sizeof(sfa));
    sfa.dwResetPeriod = 86400; // 1 day
    sfa.lpRebootMsg = NULL;
    sfa.lpCommand = NULL;
    sfa.cActions = 3;
    sfa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);

    SERVICE_FAILURE_ACTIONS_FLAG sfaf;
    sfaf.fFailureActionsOnNonCrashFailures = TRUE;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &sfaf);

    // Immediate Boot Startup (no delayed auto-start)
    SERVICE_DELAYED_AUTO_START_INFO dasi;
    dasi.fDelayedAutostart = FALSE;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &dasi);

    // Save strategy in registry for permanent anchoring
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\zapret", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring wStrat = StringToWString(targetPreset);
        std::wstring wDir = GetServicePayloadDir();
        RegSetValueExW(hKey, L"zapret-strategy", 0, REG_SZ, (const BYTE*)wStrat.c_str(), (DWORD)((wStrat.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"zapret-discord-youtube", 0, REG_SZ, (const BYTE*)wStrat.c_str(), (DWORD)((wStrat.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"zapret-payload-dir", 0, REG_SZ, (const BYTE*)wDir.c_str(), (DWORD)((wDir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Zapret", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        std::wstring wStrat = StringToWString(targetPreset);
        std::wstring wDir = GetServicePayloadDir();
        RegSetValueExW(hKey, L"ActivePreset", 0, REG_SZ, (const BYTE*)wStrat.c_str(), (DWORD)((wStrat.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"ServiceDir", 0, REG_SZ, (const BYTE*)wDir.c_str(), (DWORD)((wDir.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }

    // Start service
    BOOL started = StartServiceW(svc, 0, NULL);
    if (!started) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            AppendLog("[Service] Служба зарегистрирована в автозагрузку (код старта: " + std::to_string(err) + ")");
        }
    } else {
        AppendLog("[Service] Фоновая постоянная служба zapret успешно запущена.");
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    AppendLog("[Service Install] Служба zapret успешно закреплена в системе (" + WStringToString(GetServicePayloadDir()) + ").");
    SetCurrentPresetId(targetPreset);
    return true;
}

bool ZapretManager::RemoveService(std::string& errorMsg) {
    AppendLog("[Service] Остановка и удаление службы zapret...");
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"zapret", SERVICE_STOP | DELETE);
        if (svc) {
            SERVICE_STATUS ss;
            ControlService(svc, SERVICE_CONTROL_STOP, &ss);
            DeleteService(svc);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    ExecCommandSync("taskkill /F /IM winws.exe");
    CleanupWinDivert();
    AppendLog("[Service] Служба zapret успешно удалена.");
    return true;
}

bool ZapretManager::StartServiceManual(std::string& errorMsg) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        errorMsg = "Не удалось открыть диспетчер служб";
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"zapret", SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        errorMsg = "Служба zapret не установлена в системе";
        return false;
    }
    BOOL ok = StartServiceW(svc, 0, NULL);
    DWORD err = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok && err != ERROR_SERVICE_ALREADY_RUNNING) {
        errorMsg = "Ошибка запуска службы (код " + std::to_string(err) + ")";
        AppendLog("[Service Start ERROR] " + errorMsg);
        return false;
    }
    AppendLog("[Service Start] Фоновая служба zapret запущена.");
    return true;
}

bool ZapretManager::StopServiceManual(std::string& errorMsg) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        errorMsg = "Не удалось открыть диспетчер служб";
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"zapret", SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS ss;
        ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    ExecCommandSync("taskkill /F /IM winws.exe");
    CleanupWinDivert();
    AppendLog("[Service Stop] Служба zapret остановлена.");
    return true;
}

bool ZapretManager::IsServiceRunning(const std::string& serviceName) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;

    std::wstring wName = StringToWString(serviceName);
    SC_HANDLE svc = OpenServiceW(scm, wName.c_str(), SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp;
    DWORD bytes = 0;
    bool isRunning = false;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytes)) {
        isRunning = (ssp.dwCurrentState == SERVICE_RUNNING);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return isRunning;
}

bool ZapretManager::IsServiceInstalled(const std::string& serviceName) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;

    std::wstring wName = StringToWString(serviceName);
    SC_HANDLE svc = OpenServiceW(scm, wName.c_str(), SERVICE_QUERY_STATUS);
    bool installed = (svc != NULL);

    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return installed;
}

std::string ZapretManager::GetRunMode() {
    if (IsServiceRunning("zapret")) {
        return "service";
    }
    if (m_isProcessRunning) {
        return "process";
    }
    // Check if winws.exe is running in background independent of us
    std::string tl = ExecCommandSync("tasklist /FI \"IMAGENAME eq winws.exe\"");
    if (tl.find("winws.exe") != std::string::npos) {
        return "process";
    }
    return "stopped";
}

bool ZapretManager::IsRunning() {
    return GetRunMode() != "stopped";
}

std::string ZapretManager::CalculateFileHash(const std::wstring& filePath) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;

    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            BYTE rgbFile[1024];
            DWORD cbRead = 0;
            BOOL bResult = TRUE;
            while (bResult) {
                bResult = ReadFile(hFile, rgbFile, sizeof(rgbFile), &cbRead, NULL);
                if (bResult && cbRead == 0) break;
                CryptHashData(hHash, rgbFile, cbRead, 0);
            }

            DWORD cbHash = 32;
            BYTE rgbHash[32];
            if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
                char hex[65];
                for (DWORD i = 0; i < cbHash; i++) {
                    sprintf_s(hex + (i * 2), 3, "%02x", rgbHash[i]);
                }
                result = std::string(hex, 64);
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    CloseHandle(hFile);
    return result;
}

std::vector<std::string> ZapretManager::GetAvailableFakeFiles() {
    std::vector<std::string> list;
    try {
        for (const auto& entry : fs::directory_iterator(m_binDir)) {
            if (entry.is_regular_file() && entry.path().extension() == L".bin") {
                std::string fname = WStringToString(entry.path().filename().wstring());
                if (fname.rfind("ACTIVE_", 0) != 0) { // does not start with ACTIVE_
                    list.push_back(fname);
                }
            }
        }
    } catch (...) {}
    std::sort(list.begin(), list.end());
    return list;
}

std::string ZapretManager::GetActiveDiscordFake() {
    std::wstring activePath = m_binDir + L"\\ACTIVE_DISCORD_UDP.bin";
    if (!fs::exists(activePath)) return "quic_initial_www_google_com.bin";

    std::string activeHash = CalculateFileHash(activePath);
    if (activeHash.empty()) return "ACTIVE_DISCORD_UDP.bin";

    for (const auto& fname : GetAvailableFakeFiles()) {
        std::wstring p = m_binDir + L"\\" + StringToWString(fname);
        if (CalculateFileHash(p) == activeHash) {
            return fname;
        }
    }
    return "ACTIVE_DISCORD_UDP.bin";
}

std::string ZapretManager::GetActiveGameFake() {
    std::wstring activePath = m_binDir + L"\\ACTIVE_GAME_UDP.bin";
    if (!fs::exists(activePath)) return "quic_initial_www_google_com.bin";

    std::string activeHash = CalculateFileHash(activePath);
    if (activeHash.empty()) return "ACTIVE_GAME_UDP.bin";

    for (const auto& fname : GetAvailableFakeFiles()) {
        std::wstring p = m_binDir + L"\\" + StringToWString(fname);
        if (CalculateFileHash(p) == activeHash) {
            return fname;
        }
    }
    return "ACTIVE_GAME_UDP.bin";
}

bool ZapretManager::ReplaceActiveFake(const std::string& type, const std::string& filename, std::string& errorMsg) {
    try {
        std::wstring source = m_binDir + L"\\" + StringToWString(filename);
        if (!fs::exists(source)) {
            errorMsg = "Исходный файл не существует: " + filename;
            return false;
        }

        std::wstring dest;
        if (type == "discord") {
            dest = m_binDir + L"\\ACTIVE_DISCORD_UDP.bin";
        } else if (type == "game") {
            dest = m_binDir + L"\\ACTIVE_GAME_UDP.bin";
        } else {
            errorMsg = "Неверный тип фейка: " + type;
            return false;
        }

        fs::copy_file(source, dest, fs::copy_options::overwrite_existing);

        // Also sync to persistent ProgramData directory if deployed
        try {
            fs::path svcBin = fs::path(GetServicePayloadDir()) / L"bin";
            if (fs::exists(svcBin)) {
                fs::copy_file(source, svcBin / fs::path(dest).filename(), fs::copy_options::overwrite_existing);
            }
        } catch (...) {}

        AppendLog("[Fakes] Фейковый пакет " + type + " заменен на " + filename);
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}

nlohmann::json ZapretManager::GetAllLists() {
    nlohmann::json result = nlohmann::json::object();
    std::vector<std::string> names = {
        "list-general-user.txt",
        "list-exclude-user.txt",
        "ipset-exclude-user.txt"
    };

    for (const auto& name : names) {
        std::wstring path = m_listsDir + L"\\" + StringToWString(name);
        std::ifstream f(path);
        if (f.is_open()) {
            std::stringstream buffer;
            buffer << f.rdbuf();
            result[name] = buffer.str();
        } else {
            result[name] = "";
        }
    }
    return result;
}

bool ZapretManager::SaveListContent(const std::string& filename, const std::string& content, std::string& errorMsg) {
    try {
        std::wstring path = m_listsDir + L"\\" + StringToWString(filename);
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) {
            errorMsg = "Не удалось открыть файл для записи: " + filename;
            return false;
        }
        f.write(content.c_str(), content.size());
        f.close();

        // Also sync to persistent ProgramData lists if deployed
        try {
            fs::path svcLists = fs::path(GetServicePayloadDir()) / L"lists";
            if (fs::exists(svcLists)) {
                std::ofstream fSvc(svcLists / StringToWString(filename), std::ios::binary);
                if (fSvc.is_open()) {
                    fSvc.write(content.c_str(), content.size());
                    fSvc.close();
                }
            }
        } catch (...) {}

        AppendLog("[Lists] Список обновлен: " + filename);
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}

std::vector<DiagnosticItem> ZapretManager::RunDiagnostics() {
    std::vector<DiagnosticItem> items;

    // 1. Base Filtering Engine (BFE)
    DiagnosticItem bfe;
    bfe.name = "Служба BFE (Base Filtering Engine)";
    if (IsServiceRunning("BFE")) {
        bfe.status = "pass";
        bfe.message = "Служба активна. WinDivert драйвер готов к захвату пакетов.";
    } else {
        bfe.status = "fail";
        bfe.message = "Служба BFE отключена! Zapret не сможет перехватывать трафик.";
        bfe.canFix = true;
        bfe.fixAction = "fix_bfe";
    }
    items.push_back(bfe);

    // 2. TCP Timestamps
    DiagnosticItem ts;
    ts.name = "TCP Timestamps (Метки времени пакетов)";
    std::string tsOut = ExecCommandSync("netsh interface tcp show global");
    if (tsOut.find("timestamps") != std::string::npos && tsOut.find("enabled") != std::string::npos) {
        ts.status = "pass";
        ts.message = "Включено. Десинхронизация TCP сессий работает корректно.";
    } else {
        ts.status = "warn";
        ts.message = "Отключено. Рекомендуется включить для стабильного обхода DPI.";
        ts.canFix = true;
        ts.fixAction = "fix_tcp_timestamps";
    }
    items.push_back(ts);

    // 3. System Proxy
    DiagnosticItem proxy;
    proxy.name = "Системный Прокси-сервер";
    HKEY hKey;
    DWORD proxyEnable = 0;
    DWORD sz = sizeof(proxyEnable);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"ProxyEnable", NULL, NULL, (LPBYTE)&proxyEnable, &sz);
        RegCloseKey(hKey);
    }
    if (proxyEnable == 0) {
        proxy.status = "pass";
        proxy.message = "Системный прокси отключен. Конфликтов не обнаружено.";
    } else {
        proxy.status = "warn";
        proxy.message = "Обнаружен активный системный прокси. Это может мешать zapret.";
    }
    items.push_back(proxy);

    // 4. Conflicting Software
    DiagnosticItem conflicts;
    conflicts.name = "Конфликтующие службы и драйверы";
    std::vector<std::string> conflictNames;

    std::string tasklist = ExecCommandSync("tasklist");
    if (tasklist.find("AdguardSvc.exe") != std::string::npos) conflictNames.push_back("AdGuard");
    if (IsServiceRunning("GoodbyeDPI")) conflictNames.push_back("GoodbyeDPI Service");
    if (IsServiceRunning("Killer Network Service")) conflictNames.push_back("Killer Network");
    if (IsServiceRunning("Intel Connectivity Network Service")) conflictNames.push_back("Intel Connectivity");

    if (conflictNames.empty()) {
        conflicts.status = "pass";
        conflicts.message = "Конфликтующих сетевых фильтров (GoodbyeDPI, Killer, Adguard) не найдено.";
    } else {
        conflicts.status = "warn";
        std::string listStr;
        for (const auto& c : conflictNames) {
            if (!listStr.empty()) listStr += ", ";
            listStr += c;
        }
        conflicts.message = "Обнаружены конфликтующие программы: " + listStr;
        conflicts.canFix = true;
        conflicts.fixAction = "kill_conflicts";
    }
    items.push_back(conflicts);

    // 5. WinDivert driver integrity
    DiagnosticItem wd;
    wd.name = "Целостность драйвера WinDivert";
    std::wstring sysFile = m_binDir + L"\\WinDivert64.sys";
    std::wstring dllFile = m_binDir + L"\\WinDivert.dll";
    if (fs::exists(sysFile) && fs::exists(dllFile)) {
        wd.status = "pass";
        wd.message = "Файлы WinDivert64.sys и WinDivert.dll на месте.";
    } else {
        wd.status = "fail";
        wd.message = "Файлы WinDivert повреждены или заблокированы антивирусом.";
    }
    items.push_back(wd);

    return items;
}

bool ZapretManager::FixTcpTimestamps(std::string& result) {
    result = ExecCommandSync("netsh interface tcp set global timestamps=enabled");
    AppendLog("[Fix] TCP Timestamps активированы.");
    return true;
}

bool ZapretManager::KillConflictingProcesses(std::string& result) {
    ExecCommandSync("net stop GoodbyeDPI");
    ExecCommandSync("sc delete GoodbyeDPI");
    ExecCommandSync("net stop WinDivert");
    ExecCommandSync("sc delete WinDivert");
    result = "Конфликтующие службы WinDivert / GoodbyeDPI остановлены.";
    AppendLog("[Fix] " + result);
    return true;
}

bool ZapretManager::ClearDiscordCache(std::string& result) {
    int clearedCount = 0;
    std::vector<std::string> apps = {"discord", "discordptb", "discordcanary", "discorddevelopment"};
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        std::string base(appdata);
        free(appdata);

        // Kill discord instances first
        ExecCommandSync("taskkill /F /IM Discord.exe /IM DiscordPTB.exe /IM DiscordCanary.exe /IM DiscordDevelopment.exe");
        Sleep(500);

        for (const auto& app : apps) {
            std::string appDir = base + "\\" + app;
            std::vector<std::string> subdirs = {"\\Cache", "\\Code Cache", "\\GPUCache"};
            for (const auto& sub : subdirs) {
                std::string target = appDir + sub;
                try {
                    if (fs::exists(target)) {
                        fs::remove_all(target);
                        clearedCount++;
                    }
                } catch (...) {}
            }
        }
    }
    result = "Кэш Discord очищен (директорий обработано: " + std::to_string(clearedCount) + ")";
    AppendLog("[Discord] " + result);
    return true;
}

static int PingHost(const char* host) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return -1;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        WSACleanup();
        return -1;
    }

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        freeaddrinfo(res);
        WSACleanup();
        return -1;
    }

    char sendData[32] = "ZapretPrivatePingData";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    std::vector<BYTE> replyBuffer(replySize, 0);

    IPAddr ipAddr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    DWORD replies = IcmpSendEcho(hIcmp, ipAddr, sendData, sizeof(sendData), NULL, replyBuffer.data(), replySize, 1200);

    int latency = -1;
    if (replies > 0) {
        PICMP_ECHO_REPLY pEchoReply = (PICMP_ECHO_REPLY)replyBuffer.data();
        if (pEchoReply->Status == IP_SUCCESS) {
            latency = (int)pEchoReply->RoundTripTime;
            if (latency == 0) latency = 1; // display as 1ms
        }
    }

    IcmpCloseHandle(hIcmp);
    freeaddrinfo(res);
    WSACleanup();
    return latency;
}

nlohmann::json ZapretManager::CheckPings() {
    nlohmann::json pings = nlohmann::json::object();
    std::vector<std::pair<std::string, std::string>> targets = {
        {"discord", "discord.com"},
        {"youtube", "youtube.com"},
        {"googlevideo", "googlevideo.com"},
        {"cloudflare", "1.1.1.1"},
        {"google_dns", "8.8.8.8"}
    };

    for (const auto& [key, host] : targets) {
        int ms = PingHost(host.c_str());
        pings[key] = ms;
    }
    return pings;
}

bool ZapretManager::IsAutostartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {0};
        DWORD sz = sizeof(buf);
        LONG res = RegQueryValueExW(hKey, L"ZapretPrivate", NULL, NULL, (LPBYTE)buf, &sz);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

bool ZapretManager::SetAutostartEnabled(bool enabled) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enabled) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --minimized";
            RegSetValueExW(hKey, L"ZapretPrivate", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"ZapretPrivate");
        }
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

std::vector<std::string> ZapretManager::GetRecentLogs(size_t maxCount) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_logs.size() <= maxCount) return m_logs;
    return std::vector<std::string>(m_logs.end() - maxCount, m_logs.end());
}

void ZapretManager::AppendLog(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    struct tm t;
    localtime_s(&t, &now_c);
    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "[%H:%M:%S] ", &t);

    std::string safeLine = SanitizeToUtf8(line);
    std::string formatted = std::string(timeStr) + safeLine;
    m_logs.push_back(formatted);
    if (m_logs.size() > 1000) {
        m_logs.erase(m_logs.begin(), m_logs.begin() + 200);
    }

    if (m_logCallback) {
        m_logCallback(formatted);
    }
}

void ZapretManager::ClearLogs() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_logs.clear();
}

void ZapretManager::SetLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_logCallback = cb;
}

std::string ZapretManager::ExecCommandSync(const std::string& cmd) {
    std::string result;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hPipeRead, hPipeWrite;
    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) return "";
    SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hPipeWrite;
    si.hStdError = hPipeWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    std::string fullCmd = "cmd.exe /s /c \"" + cmd + "\"";
    std::vector<char> cmdVec(fullCmd.begin(), fullCmd.end());
    cmdVec.push_back(0);

    if (CreateProcessA(NULL, cmdVec.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hPipeWrite);

        char buffer[512];
        DWORD bytesRead = 0;
        while (ReadFile(hPipeRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = 0;
            result += buffer;
        }

        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(hPipeWrite);
    }
    CloseHandle(hPipeRead);

    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) {
        result.pop_back();
    }
    return SanitizeToUtf8(result);
}

std::string ZapretManager::ExecCommandSyncW(const std::wstring& cmd) {
    std::string result;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hPipeRead, hPipeWrite;
    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) return "";
    SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hPipeWrite;
    si.hStdError = hPipeWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    std::wstring fullCmd = L"cmd.exe /s /c \"" + cmd + L"\"";
    std::vector<wchar_t> cmdVec(fullCmd.begin(), fullCmd.end());
    cmdVec.push_back(0);

    if (CreateProcessW(NULL, cmdVec.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hPipeWrite);

        char buffer[512];
        DWORD bytesRead = 0;
        while (ReadFile(hPipeRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = 0;
            result += buffer;
        }

        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(hPipeWrite);
    }
    CloseHandle(hPipeRead);

    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) {
        result.pop_back();
    }
    return SanitizeToUtf8(result);
}

nlohmann::json ZapretManager::GetFullStatus() {
    nlohmann::json j;
    std::string mode = GetRunMode();
    j["running"] = (mode != "stopped");
    j["mode"] = mode;
    j["currentPreset"] = GetCurrentPresetId();
    j["pid"] = m_dwPid;
    j["serviceInstalled"] = IsServiceInstalled("zapret");
    j["serviceRunning"] = IsServiceRunning("zapret");
    j["gameFilter"] = GetGameFilterMode();
    j["ipsetStatus"] = GetIPSetMode();
    j["activeDiscordFake"] = GetActiveDiscordFake();
    j["activeGameFake"] = GetActiveGameFake();
    j["availableFakes"] = GetAvailableFakeFiles();
    j["autostart"] = IsAutostartEnabled();

    nlohmann::json presetsArr = nlohmann::json::array();
    for (const auto& p : GetPresets()) {
        nlohmann::json pj;
        pj["id"] = p.id;
        pj["name"] = p.name;
        pj["category"] = p.category;
        pj["description"] = p.description;
        pj["args"] = p.args;
        presetsArr.push_back(pj);
    }
    j["presets"] = presetsArr;

    return j;
}

static std::string WinHttpGetString(const std::wstring& host, const std::wstring& path, bool secure) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"ZapretPrivateScanner/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    WinHttpSetTimeouts(hRequest, 3000, 3000, 3000, 3000);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
            std::vector<char> buf(dwSize + 1, 0);
            if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                response.append(buf.data(), dwDownloaded);
            }
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

struct HostPingResult {
    int minMs = -1;
    int avgMs = -1;
    int maxMs = -1;
    int jitterMs = 0;
    int lossPct = 100;
};

static HostPingResult PingTargetDetailed(const char* host, int count = 1) {
    HostPingResult res;
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &ai) != 0 || !ai) {
        return res;
    }
    IPAddr ipAddr = ((struct sockaddr_in*)ai->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(ai);

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return res;

    char sendData[32] = "ZapretPrivateDiagPing";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    std::vector<BYTE> replyBuffer(replySize, 0);

    std::vector<int> times;
    int lost = 0;

    for (int i = 0; i < count; i++) {
        DWORD replies = IcmpSendEcho(hIcmp, ipAddr, sendData, sizeof(sendData), NULL, replyBuffer.data(), replySize, 250);
        if (replies > 0) {
            PICMP_ECHO_REPLY pEcho = (PICMP_ECHO_REPLY)replyBuffer.data();
            if (pEcho->Status == IP_SUCCESS) {
                int rtt = (int)pEcho->RoundTripTime;
                if (rtt == 0) rtt = 1;
                times.push_back(rtt);
            } else {
                lost++;
            }
        } else {
            lost++;
        }
    }
    IcmpCloseHandle(hIcmp);

    res.lossPct = (lost * 100) / count;
    if (!times.empty()) {
        int sum = 0;
        res.minMs = times[0];
        res.maxMs = times[0];
        for (int t : times) {
            sum += t;
            if (t < res.minMs) res.minMs = t;
            if (t > res.maxMs) res.maxMs = t;
        }
        res.avgMs = sum / (int)times.size();

        int jSum = 0;
        for (size_t k = 1; k < times.size(); k++) {
            jSum += std::abs(times[k] - times[k - 1]);
        }
        res.jitterMs = times.size() > 1 ? (jSum / (int)(times.size() - 1)) : 0;
    }
    return res;
}

nlohmann::json ZapretManager::GetDeepNetworkDiagnostics() {
    nlohmann::json report = nlohmann::json::object();

    // 1. Public IP & ISP Geolocation
    nlohmann::json pub = nlohmann::json::object();
    std::string ipApiResp = WinHttpGetString(L"ip-api.com", L"/json/?fields=status,message,country,countryCode,regionName,city,zip,lat,lon,timezone,isp,org,as,query", false);
    if (!ipApiResp.empty()) {
        try {
            nlohmann::json j = nlohmann::json::parse(ipApiResp);
            if (j.value("status", "") == "success") {
                pub["ip"] = j.value("query", "Unknown");
                pub["isp"] = j.value("isp", "Unknown");
                pub["org"] = j.value("org", "Unknown");
                pub["as"] = j.value("as", "Unknown");
                pub["country"] = j.value("country", "Unknown");
                pub["countryCode"] = j.value("countryCode", "XX");
                pub["city"] = j.value("city", "Unknown");
                pub["region"] = j.value("regionName", "Unknown");
                pub["timezone"] = j.value("timezone", "UTC");
            }
        } catch (...) {}
    }
    if (pub.empty() || !pub.contains("ip")) {
        std::string ipifyResp = WinHttpGetString(L"api.ipify.org", L"/?format=json", true);
        try {
            nlohmann::json j2 = nlohmann::json::parse(ipifyResp);
            pub["ip"] = j2.value("ip", "127.0.0.1");
            pub["isp"] = "Автоматическое определение";
            pub["country"] = "Локальная сеть";
            pub["city"] = "-";
        } catch (...) {
            pub["ip"] = "Не удалось определить (офлайн)";
            pub["isp"] = "Неизвестный провайдер";
        }
    }
    report["public"] = pub;

    // 2. Primary Physical Adapter Details (Filters out Radmin, Hamachi, Virtual, VPN)
    nlohmann::json adapters = nlohmann::json::array();
    std::wstring primaryName = GetPrimaryPhysicalAdapterName();

    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
    if (pAddrs) {
        DWORD ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX, NULL, pAddrs, &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            free(pAddrs);
            pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
            if (pAddrs) ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX, NULL, pAddrs, &bufLen);
        }
        if (pAddrs && ret == NO_ERROR) {
            PIP_ADAPTER_ADDRESSES curr = pAddrs;
            while (curr) {
                if (curr->OperStatus == IfOperStatusUp &&
                    (curr->IfType == IF_TYPE_ETHERNET_CSMACD || curr->IfType == IF_TYPE_IEEE80211)) {
                    
                    std::wstring fname = curr->FriendlyName ? curr->FriendlyName : L"";
                    if (fname == primaryName) {
                        nlohmann::json a;
                        a["name"] = WStringToString(curr->FriendlyName);
                        a["description"] = WStringToString(curr->Description);
                        a["mtu"] = curr->Mtu;

                        // MAC address
                        char macBuf[32] = {0};
                        if (curr->PhysicalAddressLength == 6) {
                            sprintf_s(macBuf, "%02X:%02X:%02X:%02X:%02X:%02X",
                                curr->PhysicalAddress[0], curr->PhysicalAddress[1],
                                curr->PhysicalAddress[2], curr->PhysicalAddress[3],
                                curr->PhysicalAddress[4], curr->PhysicalAddress[5]);
                        }
                        a["mac"] = macBuf;

                        // Speed
                        std::string speedStr = "1 Gbps";
                        if (curr->TransmitLinkSpeed > 0) {
                            unsigned long long mbps = curr->TransmitLinkSpeed / 1000000ULL;
                            if (mbps >= 1000) {
                                speedStr = std::to_string(mbps / 1000) + " Gbps (" + std::to_string(mbps) + " Mbps)";
                            } else {
                                speedStr = std::to_string(mbps) + " Mbps";
                            }
                        }
                        a["speed"] = speedStr;

                        // Unicast IPv4
                        std::string ipv4 = "-";
                        PIP_ADAPTER_UNICAST_ADDRESS pUni = curr->FirstUnicastAddress;
                        while (pUni) {
                            if (pUni->Address.lpSockaddr->sa_family == AF_INET) {
                                char ipStr[INET_ADDRSTRLEN] = {0};
                                sockaddr_in* sa_in = (sockaddr_in*)pUni->Address.lpSockaddr;
                                inet_ntop(AF_INET, &(sa_in->sin_addr), ipStr, INET_ADDRSTRLEN);
                                ipv4 = ipStr;
                                break;
                            }
                            pUni = pUni->Next;
                        }
                        a["ipv4"] = ipv4;

                        // Gateway
                        std::string gateway = "-";
                        PIP_ADAPTER_GATEWAY_ADDRESS_LH pGate = curr->FirstGatewayAddress;
                        if (pGate && pGate->Address.lpSockaddr->sa_family == AF_INET) {
                            char gateStr[INET_ADDRSTRLEN] = {0};
                            sockaddr_in* sa_gate = (sockaddr_in*)pGate->Address.lpSockaddr;
                            inet_ntop(AF_INET, &(sa_gate->sin_addr), gateStr, INET_ADDRSTRLEN);
                            gateway = gateStr;
                        }
                        a["gateway"] = gateway;

                        // DNS servers
                        nlohmann::json dnsList = nlohmann::json::array();
                        PIP_ADAPTER_DNS_SERVER_ADDRESS pDns = curr->FirstDnsServerAddress;
                        while (pDns) {
                            if (pDns->Address.lpSockaddr->sa_family == AF_INET) {
                                char dnsStr[INET_ADDRSTRLEN] = {0};
                                sockaddr_in* sa_dns = (sockaddr_in*)pDns->Address.lpSockaddr;
                                inet_ntop(AF_INET, &(sa_dns->sin_addr), dnsStr, INET_ADDRSTRLEN);
                                dnsList.push_back(dnsStr);
                            }
                            pDns = pDns->Next;
                        }
                        a["dns"] = dnsList;
                        adapters.push_back(a);
                        break; // Primary physical connection found and added
                    }
                }
                curr = curr->Next;
            }
            free(pAddrs);
        }
    }
    report["adapters"] = adapters;

    // 3. DPI & Security Health
    nlohmann::json dpi = nlohmann::json::object();
    std::string timestampsOut = ExecCommandSync("netsh interface tcp show global");
    dpi["tcp_timestamps"] = (timestampsOut.find("timestamps") != std::string::npos && timestampsOut.find("enabled") != std::string::npos);

    // DNS Hijack check
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_INET;
    std::string resolvedYt = "Не определен";
    bool hijacked = false;
    if (getaddrinfo("youtube.com", NULL, &hints, &ai) == 0 && ai) {
        char ipBuf[INET_ADDRSTRLEN] = {0};
        sockaddr_in* sin = (sockaddr_in*)ai->ai_addr;
        inet_ntop(AF_INET, &(sin->sin_addr), ipBuf, sizeof(ipBuf));
        resolvedYt = ipBuf;
        freeaddrinfo(ai);

        if (resolvedYt.rfind("127.", 0) == 0 || resolvedYt.rfind("10.", 0) == 0 || resolvedYt.rfind("192.168.", 0) == 0) {
            hijacked = true;
        }
    }
    dpi["dns_hijack_detected"] = hijacked;
    dpi["resolved_youtube_ip"] = resolvedYt;
    dpi["windivert_installed"] = fs::exists(m_binDir + L"\\WinDivert64.sys");
    dpi["smart_dns_active"] = IsSmartDNSEnabled();
    dpi["path_mtu"] = 1500;
    report["dpi_status"] = dpi;

    // 4. Comprehensive Latency Matrix across Gaming, AI, Media, and Core DNS
    nlohmann::json latMatrix = nlohmann::json::object();

    auto runCluster = [](const std::vector<std::pair<std::string, std::string>>& items) {
        nlohmann::json cluster = nlohmann::json::array();
        for (const auto& [name, host] : items) {
            auto stats = PingTargetDetailed(host.c_str(), 2);
            nlohmann::json item;
            item["name"] = name;
            item["host"] = host;
            item["min"] = stats.minMs;
            item["avg"] = stats.avgMs;
            item["max"] = stats.maxMs;
            item["jitter"] = stats.jitterMs;
            item["loss"] = stats.lossPct;
            cluster.push_back(item);
        }
        return cluster;
    };

    latMatrix["games"] = runCluster({
        {"CS2 / Valve (Frankfurt)", "155.133.248.51"},
        {"Riot Games / Valorant", "162.249.72.1"},
        {"Steam / Valve Network", "162.254.192.1"},
        {"Epic Games / Fortnite", "epicgames.com"},
        {"Battle.net / Blizzard", "eu.actual.battle.net"}
    });

    latMatrix["ai"] = runCluster({
        {"OpenAI / ChatGPT", "api.openai.com"},
        {"Anthropic / Claude", "claude.ai"},
        {"Google Gemini AI", "generativelanguage.googleapis.com"},
        {"Perplexity AI", "perplexity.ai"}
    });

    latMatrix["media"] = runCluster({
        {"YouTube Global CDN", "googlevideo.com"},
        {"Twitch TV", "twitch.tv"},
        {"Discord Voice / Gateway", "gateway.discord.gg"}
    });

    latMatrix["dns"] = runCluster({
        {"Cloudflare (1.1.1.1)", "1.1.1.1"},
        {"Google DNS (8.8.8.8)", "8.8.8.8"},
        {"Quad9 (9.9.9.9)", "9.9.9.9"}
    });

    report["latency_matrix"] = latMatrix;

    // Save scan to cache file so telemetry persists across app reloads
    try {
        std::wstring cachePath = m_rootDir + L"\\scan_cache.json";
        std::ofstream f(cachePath);
        if (f.is_open()) {
            f << report.dump(4, ' ', false, nlohmann::json::error_handler_t::replace);
        }
    } catch (...) {}

    return report;
}

nlohmann::json ZapretManager::AutoBenchmarkNetwork() {
    nlohmann::json b = nlohmann::json::object();

    // 1. Measure hops to reference servers to calculate autottl
    int measuredHops = 3;
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp != INVALID_HANDLE_VALUE) {
        struct addrinfo hints = {0}, *ai = NULL;
        hints.ai_family = AF_INET;
        if (getaddrinfo("8.8.8.8", NULL, &hints, &ai) == 0 && ai) {
            IPAddr targetIp = ((sockaddr_in*)ai->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(ai);

            char sendBuf[16] = "TTLProbe";
            DWORD repSize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendBuf) + 8;
            std::vector<BYTE> repBuf(repSize, 0);

            for (UCHAR ttl = 1; ttl <= 8; ttl++) {
                IP_OPTION_INFORMATION ipInfo = {0};
                ipInfo.Ttl = ttl;
                DWORD reps = IcmpSendEcho(hIcmp, targetIp, sendBuf, sizeof(sendBuf), &ipInfo, repBuf.data(), repSize, 120);
                if (reps > 0) {
                    PICMP_ECHO_REPLY pEcho = (PICMP_ECHO_REPLY)repBuf.data();
                    if (pEcho->Status == IP_TTL_EXPIRED_TRANSIT) {
                        // ISP router encountered
                        measuredHops = (int)ttl;
                    } else if (pEcho->Status == IP_SUCCESS) {
                        break;
                    }
                }
            }
        }
        IcmpCloseHandle(hIcmp);
    }

    int recommendedAutoTtl = std::clamp(measuredHops - 1, 1, 4);

    b["detected_dpi_hops"] = measuredHops;
    b["recommended_autottl"] = recommendedAutoTtl;
    b["recommended_repeats"] = 8;
    b["recommended_split_pos"] = 1;
    b["recommended_fake_tls"] = "tls_clienthello_www_google_com.bin";
    b["recommended_fake_quic"] = "quic_initial_www_google_com.bin";
    b["recommended_game_udp"] = "ACTIVE_GAME_UDP.bin";
    b["recommended_split_seqovl"] = 681;
    b["summary"] = "Оптимальная дистанция десинхронизации: autottl=" + std::to_string(recommendedAutoTtl) + ", Fake SNI: Google TLS, 8 повторов.";

    return b;
}

nlohmann::json ZapretManager::GenerateCustomPreset(const nlohmann::json& config) {
    bool games = config.value("includeGames", true);
    bool yt = config.value("includeYoutube", true);
    bool ai = config.value("includeAI", true);
    bool discord = config.value("includeDiscord", true);

    std::string provider = config.value("provider", "auto");
    int repeats = config.value("repeats", 8);
    int splitPos = config.contains("splitPos") ? config["splitPos"].get<int>() : config.value("splitPosition", 1);
    int autottl = config.contains("autoTtl") ? config["autoTtl"].get<int>() : config.value("autottl", 2);
    std::string fakeTls = config.value("fakeTls", "tls_clienthello_www_google_com.bin");
    std::string fakeQuic = config.value("fakeQuic", "quic_initial_www_google_com.bin");

    // Auto-detect ISP if provider is set to "auto"
    std::string detectedIspName = "";
    std::string detectedAsName = "";
    std::string detectedOrgName = "";
    bool isExplicitAuto = (provider == "auto");

    if (isExplicitAuto) {
        try {
            nlohmann::json netCache = GetCachedNetworkDiagnostics();
            if (netCache.contains("public")) {
                if (netCache["public"].contains("isp") && netCache["public"]["isp"].is_string()) {
                    detectedIspName = netCache["public"]["isp"].get<std::string>();
                }
                if (netCache["public"].contains("as") && netCache["public"]["as"].is_string()) {
                    detectedAsName = netCache["public"]["as"].get<std::string>();
                }
                if (netCache["public"].contains("org") && netCache["public"]["org"].is_string()) {
                    detectedOrgName = netCache["public"]["org"].get<std::string>();
                }
            }
        } catch (...) {}

        std::string lowerCombined = detectedIspName + " " + detectedAsName + " " + detectedOrgName;
        std::transform(lowerCombined.begin(), lowerCombined.end(), lowerCombined.begin(), ::tolower);

        if (lowerCombined.find("rostelecom") != std::string::npos || 
            lowerCombined.find("rtk") != std::string::npos || 
            lowerCombined.find("onlime") != std::string::npos ||
            lowerCombined.find("qwerty") != std::string::npos ||
            lowerCombined.find("bashtel") != std::string::npos ||
            lowerCombined.find("sibirtelecom") != std::string::npos ||
            lowerCombined.find("nwtelecom") != std::string::npos ||
            lowerCombined.find("centertelecom") != std::string::npos ||
            lowerCombined.find("volgatelecom") != std::string::npos ||
            lowerCombined.find("as12389") != std::string::npos ||
            lowerCombined.find("as42610") != std::string::npos) {
            provider = "rostelecom";
        } else if (lowerCombined.find("er-telecom") != std::string::npos || 
                   lowerCombined.find("dom.ru") != std::string::npos || 
                   lowerCombined.find("domru") != std::string::npos ||
                   lowerCombined.find("interzet") != std::string::npos || 
                   lowerCombined.find("akado") != std::string::npos || 
                   lowerCombined.find("novotelecom") != std::string::npos || 
                   lowerCombined.find("electronny gorod") != std::string::npos || 
                   lowerCombined.find("enforta") != std::string::npos ||
                   lowerCombined.find("kolambiya") != std::string::npos) {
            provider = "domru";
        } else if (lowerCombined.find("mts") != std::string::npos || 
                   lowerCombined.find("mgts") != std::string::npos || 
                   lowerCombined.find("mobile telesystems") != std::string::npos || 
                   lowerCombined.find("comstar") != std::string::npos ||
                   lowerCombined.find("as8359") != std::string::npos) {
            provider = "mts";
        } else if (lowerCombined.find("beeline") != std::string::npos || 
                   lowerCombined.find("vimpelcom") != std::string::npos || 
                   lowerCombined.find("veon") != std::string::npos || 
                   lowerCombined.find("corbina") != std::string::npos || 
                   lowerCombined.find("sovintel") != std::string::npos || 
                   lowerCombined.find("golden telecom") != std::string::npos ||
                   lowerCombined.find("as8402") != std::string::npos) {
            provider = "beeline";
        } else if (lowerCombined.find("transtelecom") != std::string::npos || 
                   lowerCombined.find("trans telekom") != std::string::npos || 
                   lowerCombined.find("trans-telecom") != std::string::npos || 
                   lowerCombined.find("ttk") != std::string::npos || 
                   lowerCombined.find("rzd") != std::string::npos ||
                   lowerCombined.find("as20485") != std::string::npos) {
            provider = "ttk";
        } else if (lowerCombined.find("ufanet") != std::string::npos || 
                   lowerCombined.find("ufa-net") != std::string::npos ||
                   lowerCombined.find("as31133") != std::string::npos) {
            provider = "ufanet";
        } else if (lowerCombined.find("intersvyaz") != std::string::npos || 
                   lowerCombined.find("intersviaz") != std::string::npos || 
                   lowerCombined.find("is74") != std::string::npos ||
                   lowerCombined.find("as39951") != std::string::npos) {
            provider = "intersvyaz";
        } else if (lowerCombined.find("tattelecom") != std::string::npos || 
                   lowerCombined.find("tattel") != std::string::npos || 
                   lowerCombined.find("letai") != std::string::npos ||
                   lowerCombined.find("as3255") != std::string::npos) {
            provider = "tattelecom";
        } else if (lowerCombined.find("citylink") != std::string::npos || 
                   lowerCombined.find("sitilink") != std::string::npos || 
                   lowerCombined.find("ptzk") != std::string::npos ||
                   lowerCombined.find("as43632") != std::string::npos) {
            provider = "citylink";
        } else if (!lowerCombined.empty() && lowerCombined != "  ") {
            provider = "regional";
        } else {
            provider = "rostelecom"; // Safest fallback
        }
    }

    std::string presetTitle = "Идеальный адаптивный пресет";
    std::string presetDesc = "Индивидуальная стратегия под вашу сеть";
    std::string tcpDesyncMode = "fake,multisplit";
    std::string tcpFooling = "badseq,md5sig";
    int defaultRepeats = 8;
    int defaultSplitPos = 1;
    int defaultTtl = 2;

    if (provider == "rostelecom") {
        presetTitle = "Идеальный пресет: Ростелеком (РТК / Онлайм / Qwerty)";
        presetDesc = "Оптимизирован под ТСПУ EcoFilter Ростелекома: multisplit, badseq+md5sig, обход инспекции SNI и троттлинга YouTube";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq,md5sig";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 2;
    } else if (provider == "domru") {
        presetTitle = "Идеальный пресет: Дом.ру (ЭР-Телеком / Акадо)";
        presetDesc = "Оптимизирован под граничный ТСПУ Дом.ру: split pos 2, autottl=1, защита от RST-сбросов и zero-ip-id";
        tcpDesyncMode = "fake,split";
        tcpFooling = "badseq";
        defaultRepeats = 8;
        defaultSplitPos = 2;
        defaultTtl = 1;
    } else if (provider == "mts") {
        presetTitle = "Идеальный пресет: МТС / МГТС GPON";
        presetDesc = "Оптимизирован под оптический ШПД и фильтрацию МТС: split2 со смещением pos 1, badseq, обход блокировки SNI";
        tcpDesyncMode = "fake,split2";
        tcpFooling = "badseq";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 2;
    } else if (provider == "beeline") {
        presetTitle = "Идеальный пресет: Билайн (ВымпелКом / Корбина FTTB)";
        presetDesc = "Оптимизирован под магистральный ТСПУ Билайн: multisplit, 11 повторов, datanoack против инжекций RST/ACK";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq,datanoack";
        defaultRepeats = 11;
        defaultSplitPos = 1;
        defaultTtl = 3;
    } else if (provider == "ttk") {
        presetTitle = "Идеальный пресет: ТТК (ТрансТелеКом / ОАО РЖД)";
        presetDesc = "Оптимизирован под магистральную сеть ТТК: multisplit, ts (time-stamps spoofing) + badseq, autottl=3";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq,ts";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 3;
    } else if (provider == "ufanet") {
        presetTitle = "Идеальный пресет: Уфанет (Башкортостан, Татарстан, Оренбург)";
        presetDesc = "Оптимизирован под сеть Уфанет: multisplit pos 1, badseq, autottl=2 для низкого пинга в играх и 4K стриминга";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 2;
    } else if (provider == "intersvyaz") {
        presetTitle = "Идеальный пресет: Интерсвязь (Южный Урал)";
        presetDesc = "Оптимизирован под оптическую сеть Интерсвязь: multisplit, badseq,ts, autottl=2, обход ТСПУ на аплинках";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq,ts";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 2;
    } else if (provider == "tattelecom") {
        presetTitle = "Идеальный пресет: Таттелеком (Летай)";
        presetDesc = "Оптимизирован под сеть Таттелеком (Татарстан): multisplit pos 1, badseq, стабильная работа Discord и YouTube";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 2;
    } else if (provider == "citylink") {
        presetTitle = "Идеальный пресет: Ситилинк (Северо-Запад РФ)";
        presetDesc = "Оптимизирован под сеть Ситилинк (Карелия, Мурманск, Архангельск): multisplit, ts fooling, autottl=3";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "ts";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 3;
    } else if (provider == "regional") {
        presetTitle = "Идеальный пресет: Региональные ШПД (Convex, SkyNet, Владлинк, Севстар)";
        presetDesc = "Универсальный сбалансированный профиль для независимых кабельных операторов РФ: multisplit, badseq,ts, autottl=2";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq,ts";
        defaultRepeats = 8;
        defaultSplitPos = 1;
        defaultTtl = 2;
    } else if (provider == "aggressive") {
        presetTitle = "Идеальный пресет: Максимальный пробив (Тяжелые ТСПУ)";
        presetDesc = "Экстремальный режим десинхронизации: 12 повторов, multisplit, комбинированный badseq+md5sig, autottl=2";
        tcpDesyncMode = "fake,multisplit";
        tcpFooling = "badseq,md5sig";
        defaultRepeats = 12;
        defaultSplitPos = 1;
        defaultTtl = 2;
    }

    // Override with custom values if explicitly provided and not in auto mode
    if (!isExplicitAuto) {
        if (config.contains("splitPos") || config.contains("splitPosition")) defaultSplitPos = splitPos;
        if (config.contains("repeats")) defaultRepeats = repeats;
        if (config.contains("autoTtl") || config.contains("autottl")) defaultTtl = autottl;
    }

    std::ostringstream ss;
    ss << "--wf-tcp=80,443,2053,2083,2087,2096,8443,%GameFilterTCP% --wf-udp=443,19294-19344,50000-50100,%GameFilterUDP% ";

    // QUIC / UDP 443
    ss << " --filter-udp=443 --hostlist=\"%LISTS%list-general.txt\" --hostlist=\"%LISTS%list-general-user.txt\" ";
    if (yt) ss << "--hostlist=\"%LISTS%list-google.txt\" ";
    if (ai) ss << "--hostlist=\"%LISTS%list-ai.txt\" ";
    ss << "--hostlist-exclude=\"%LISTS%list-exclude.txt\" --hostlist-exclude=\"%LISTS%list-exclude-user.txt\" "
       << "--ipset-exclude=\"%LISTS%ipset-exclude.txt\" --ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" "
       << "--dpi-desync=fake --dpi-desync-repeats=11 --dpi-desync-fake-quic=\"%BIN%" << fakeQuic << "\" --new ";

    // Discord UDP Voice & STUN
    if (discord) {
        ss << " --filter-udp=19294-19344,50000-50100 --filter-l7=discord,stun --dpi-desync=fake "
           << "--dpi-desync-fake-discord=\"%BIN%stun.bin\" --dpi-desync-fake-discord=\"%BIN%ACTIVE_DISCORD_UDP.bin\" "
           << "--dpi-desync-fake-stun=\"%BIN%ACTIVE_DISCORD_UDP.bin\" --dpi-desync-repeats=6 --new "
           << " --filter-tcp=2053,2083,2087,2096,8443 --hostlist-domains=discord.media --dpi-desync=" << tcpDesyncMode << " "
           << "--dpi-desync-split-seqovl=681 --dpi-desync-split-pos=" << defaultSplitPos << " --dpi-desync-fooling=" << tcpFooling << " "
           << "--dpi-desync-repeats=" << defaultRepeats << " --dpi-desync-split-seqovl-pattern=\"%BIN%" << fakeTls << "\" "
           << "--dpi-desync-fake-tls=\"%BIN%" << fakeTls << "\" --new ";
    }

    // YouTube 4K Stream
    if (yt) {
        ss << " --filter-tcp=443 --hostlist=\"%LISTS%list-google.txt\" "
           << "--hostlist-exclude=\"%LISTS%list-exclude.txt\" --hostlist-exclude=\"%LISTS%list-exclude-user.txt\" "
           << "--ipset-exclude=\"%LISTS%ipset-exclude.txt\" --ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" "
           << "--ip-id=zero --dpi-desync=" << tcpDesyncMode << " "
           << "--dpi-desync-split-seqovl=681 --dpi-desync-split-pos=" << defaultSplitPos << " --dpi-desync-fooling=" << tcpFooling << " "
           << "--dpi-desync-repeats=" << defaultRepeats << " --dpi-desync-split-seqovl-pattern=\"%BIN%" << fakeTls << "\" "
           << "--dpi-desync-fake-tls=\"%BIN%" << fakeTls << "\" --new ";
    }

    // AI & Neural Networks Location Spoofing
    if (ai) {
        ss << " --filter-tcp=443 --hostlist=\"%LISTS%list-ai.txt\" "
           << "--hostlist-exclude=\"%LISTS%list-exclude.txt\" --hostlist-exclude=\"%LISTS%list-exclude-user.txt\" "
           << "--ipset-exclude=\"%LISTS%ipset-exclude.txt\" --ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" "
           << "--ip-id=zero --dpi-desync=" << tcpDesyncMode << " "
           << "--dpi-desync-split-seqovl=681 --dpi-desync-split-pos=" << defaultSplitPos << " --dpi-desync-fooling=" << tcpFooling << " "
           << "--dpi-desync-repeats=" << defaultRepeats << " --dpi-desync-split-seqovl-pattern=\"%BIN%" << fakeTls << "\" "
           << "--dpi-desync-fake-tls=\"%BIN%" << fakeTls << "\" --new ";
    }

    // General Blocked Sites
    ss << " --filter-tcp=80,443 --hostlist=\"%LISTS%list-general.txt\" --hostlist=\"%LISTS%list-general-user.txt\" "
       << "--hostlist-exclude=\"%LISTS%list-exclude.txt\" --hostlist-exclude=\"%LISTS%list-exclude-user.txt\" "
       << "--ipset-exclude=\"%LISTS%ipset-exclude.txt\" --ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" "
       << "--dpi-desync=" << tcpDesyncMode << " --dpi-desync-split-seqovl=664 --dpi-desync-split-pos=" << defaultSplitPos
       << " --dpi-desync-fooling=" << tcpFooling << " --dpi-desync-repeats=" << defaultRepeats
       << " --dpi-desync-split-seqovl-pattern=\"%BIN%tls_clienthello_max_ru.bin\" --dpi-desync-fake-tls=\"%BIN%stun.bin\" "
       << "--dpi-desync-fake-tls=\"%BIN%tls_clienthello_max_ru.bin\" --dpi-desync-fake-http=\"%BIN%tls_clienthello_max_ru.bin\" --new ";

    // IPSet Fallback
    ss << " --filter-udp=443 --ipset=\"%LISTS%ipset-all.txt\" --hostlist-exclude=\"%LISTS%list-exclude.txt\" "
       << "--hostlist-exclude=\"%LISTS%list-exclude-user.txt\" --ipset-exclude=\"%LISTS%ipset-exclude.txt\" "
       << "--ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" --dpi-desync=fake --dpi-desync-repeats=11 "
       << "--dpi-desync-fake-quic=\"%BIN%" << fakeQuic << "\" --new ";

    ss << " --filter-tcp=80,443,8443,%GameFilterTCP% --ipset=\"%LISTS%ipset-all.txt\" "
       << "--hostlist-exclude=\"%LISTS%list-exclude.txt\" --hostlist-exclude=\"%LISTS%list-exclude-user.txt\" "
       << "--ipset-exclude=\"%LISTS%ipset-exclude.txt\" --ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" "
       << "--dpi-desync=" << tcpDesyncMode << " --dpi-desync-split-seqovl=664 --dpi-desync-split-pos=" << defaultSplitPos
       << " --dpi-desync-fooling=" << tcpFooling << " --dpi-desync-repeats=" << defaultRepeats
       << " --dpi-desync-split-seqovl-pattern=\"%BIN%tls_clienthello_max_ru.bin\" --dpi-desync-fake-tls=\"%BIN%stun.bin\" "
       << "--dpi-desync-fake-tls=\"%BIN%tls_clienthello_max_ru.bin\" --dpi-desync-fake-http=\"%BIN%tls_clienthello_max_ru.bin\" --new ";

    // Games Filter Low Latency
    if (games) {
        ss << " --filter-udp=%GameFilterUDP% --ipset=\"%LISTS%ipset-all.txt\" --ipset-exclude=\"%LISTS%ipset-exclude.txt\" "
           << "--ipset-exclude=\"%LISTS%ipset-exclude-user.txt\" --dpi-desync=fake --dpi-desync-autottl=" << defaultTtl
           << " --dpi-desync-repeats=12 --dpi-desync-any-protocol=1 --dpi-desync-fake-unknown-udp=\"%BIN%ACTIVE_GAME_UDP.bin\" --dpi-desync-cutoff=n2";
    }

    nlohmann::json gen;
    gen["id"] = "custom_generated";
    gen["name"] = presetTitle;
    gen["category"] = "Пользовательские";
    gen["description"] = presetDesc;
    gen["args"] = ss.str();
    gen["provider"] = provider;
    gen["detected_isp"] = detectedIspName;
    gen["optimal_split_pos"] = defaultSplitPos;
    gen["optimal_repeats"] = defaultRepeats;
    gen["optimal_autottl"] = defaultTtl;
    return gen;
}

bool ZapretManager::SaveCustomPreset(const std::string& id, const std::string& name, const std::string& description, const std::string& args, std::string& errorMsg) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    try {
        std::wstring presetsPath = m_rootDir + L"\\presets.json";
        nlohmann::json list = nlohmann::json::array();
        if (fs::exists(presetsPath)) {
            std::ifstream fin(presetsPath);
            if (fin.is_open()) {
                try {
                    fin >> list;
                } catch (...) {
                    list = nlohmann::json::array();
                }
                fin.close();
            }
        }

        nlohmann::json newPreset;
        newPreset["id"] = id;
        newPreset["name"] = name;
        newPreset["category"] = "Пользовательские";
        newPreset["description"] = description;
        newPreset["args"] = args;

        // Replace if exists or insert at front
        bool replaced = false;
        for (auto& p : list) {
            if (p.value("id", "") == id) {
                p = newPreset;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            list.insert(list.begin(), newPreset);
        }

        {
            std::ofstream fout(presetsPath);
            if (!fout.is_open()) {
                errorMsg = "Не удалось открыть presets.json для записи";
                return false;
            }
            fout << list.dump(4);
            fout.flush();
            fout.close();
        }

        // Direct in-memory sync for instant availability
        PresetInfo pi;
        pi.id = id;
        pi.name = name;
        pi.category = "Пользовательские";
        pi.description = description;
        pi.args = args;

        bool inMemReplaced = false;
        for (auto& item : m_presets) {
            if (item.id == id) {
                item = pi;
                inMemReplaced = true;
                break;
            }
        }
        if (!inMemReplaced) {
            m_presets.insert(m_presets.begin(), pi);
        }

        AppendLog("[Presets] Сохранен пресет: " + name);
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}

bool ZapretManager::DeletePreset(const std::string& presetId, std::string& errorMsg) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    try {
        if (presetId.empty()) {
            errorMsg = "Не указан ID пресета для удаления";
            return false;
        }

        auto it = std::remove_if(m_presets.begin(), m_presets.end(), [&](const PresetInfo& p) {
            return p.id == presetId;
        });

        if (it == m_presets.end()) {
            errorMsg = "Пресет с таким идентификатором не найден";
            return false;
        }

        m_presets.erase(it, m_presets.end());

        std::wstring presetsPath = m_rootDir + L"\\presets.json";
        nlohmann::json list = nlohmann::json::array();
        if (fs::exists(presetsPath)) {
            std::ifstream fin(presetsPath);
            if (fin.is_open()) {
                try { fin >> list; } catch (...) { list = nlohmann::json::array(); }
                fin.close();
            }
        }

        nlohmann::json updatedList = nlohmann::json::array();
        for (const auto& item : list) {
            if (item.value("id", "") != presetId) {
                updatedList.push_back(item);
            }
        }

        {
            std::ofstream fout(presetsPath);
            if (fout.is_open()) {
                fout << updatedList.dump(4);
                fout.flush();
                fout.close();
            }
        }

        if (m_currentPresetId == presetId) {
            m_currentPresetId = m_presets.empty() ? "general" : m_presets[0].id;
        }

        AppendLog("[Presets] Пресет удален: " + presetId);
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}


nlohmann::json ZapretManager::GetCachedNetworkDiagnostics() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    try {
        std::wstring cachePath = m_rootDir + L"\\scan_cache.json";
        if (fs::exists(cachePath)) {
            std::ifstream f(cachePath);
            if (f.is_open()) {
                nlohmann::json report;
                f >> report;
                return report;
            }
        }
    } catch (...) {}
    nlohmann::json report = nlohmann::json::object();
    report["public"] = nlohmann::json::object();
    report["adapters"] = nlohmann::json::array();
    report["dpi_status"] = nlohmann::json::object();
    report["latency_matrix"] = nlohmann::json::object();
    return report;
}

std::wstring ZapretManager::GetPrimaryPhysicalAdapterName() {
    DWORD bestIfIndex = 0;
    DWORD targetIp = inet_addr("1.1.1.1");
    bool hasBestIf = (GetBestInterface(targetIp, &bestIfIndex) == NO_ERROR);

    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
    if (!pAddrs) return L"Ethernet";

    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddrs, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(pAddrs);
        pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
        if (!pAddrs) return L"Ethernet";
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddrs, &bufLen);
    }

    std::wstring chosenName = L"";

    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES curr = pAddrs; curr; curr = curr->Next) {
            if (curr->OperStatus != IfOperStatusUp) continue;
            if (curr->IfType != IF_TYPE_ETHERNET_CSMACD && curr->IfType != IF_TYPE_IEEE80211) continue;

            std::wstring desc = curr->Description ? curr->Description : L"";
            std::wstring fname = curr->FriendlyName ? curr->FriendlyName : L"";
            std::string lowerDesc = WStringToString(desc);
            std::string lowerFname = WStringToString(fname);
            std::transform(lowerDesc.begin(), lowerDesc.end(), lowerDesc.begin(), ::tolower);
            std::transform(lowerFname.begin(), lowerFname.end(), lowerFname.begin(), ::tolower);

            // Blacklist virtual, VPN, tunnel adapters
            static const std::vector<std::string> blacklist = {
                "radmin", "tap", "tun", "vpn", "virtual", "hyper-v", "vmware",
                "hamachi", "pseudo", "loopback", "wsl", "vethernet", "wireguard",
                "zerotier", "tailscale", "bluetooth", "ndiswan", "teredo", "isatap"
            };

            bool isBlacklisted = false;
            for (const auto& b : blacklist) {
                if (lowerDesc.find(b) != std::string::npos || lowerFname.find(b) != std::string::npos) {
                    isBlacklisted = true;
                    break;
                }
            }
            if (isBlacklisted) continue;
            if (!curr->FirstGatewayAddress) continue;

            if (hasBestIf && curr->IfIndex == bestIfIndex) {
                chosenName = curr->FriendlyName;
                break;
            }

            if (chosenName.empty()) {
                chosenName = curr->FriendlyName;
            }
        }
    }

    free(pAddrs);
    if (chosenName.empty()) chosenName = L"Ethernet";
    return chosenName;
}

struct DNSProfileDef {
    const char* id;
    const char* name;
    const char* desc;
    const char* primary;
    const char* secondary;
};

static const DNSProfileDef kSupportedDnsProfiles[] = {
    {"comss_ai", "Comss.one AI Bypass (Нидерланды)", "Разблокировка ChatGPT, Claude, Gemini, Copilot, Antigravity", "83.220.169.155", "195.133.25.16"},
    {"xbox_electro", "Xbox-DNS (xbox-dns.ru)", "Основной (IPv4) 111.88.96.54 / Дополнительный (IPv4) 111.88.96.55", "111.88.96.54", "111.88.96.55"},
    {"xbox_dns", "Xbox-DNS (xbox-dns.ru)", "Основной (IPv4) 111.88.96.54 / Дополнительный (IPv4) 111.88.96.55", "111.88.96.54", "111.88.96.55"},
    {"shecan", "Shecan DNS (Обход региональных ограничений)", "Снятие региональных блокировок с зарубежных платформ", "178.22.122.100", "185.51.200.2"},
    {"cloudflare", "Cloudflare Fast DNS (1.1.1.1)", "Зарубежный быстрый DNS с минимальной задержкой", "1.1.1.1", "1.0.0.1"},
    {"google", "Google Public DNS (8.8.8.8)", "Глобальная стабильная сеть DNS от Google", "8.8.8.8", "8.8.4.4"},
    {"quad9", "Quad9 Secure DNS (9.9.9.9)", "Швейцарский безопасный DNS с блокировкой вредоносных узлов", "9.9.9.9", "149.112.112.112"}
};

std::string ZapretManager::GetCurrentDNSProfile() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_activeDnsProfile;
}

nlohmann::json ZapretManager::GetAvailableDNSProfiles() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : kSupportedDnsProfiles) {
        if (std::string(p.id) == "xbox_dns") continue; // keep xbox_electro as primary id for dropdown compatibility
        nlohmann::json j;
        j["id"] = p.id;
        j["name"] = p.name;
        j["description"] = p.desc;
        j["primary"] = p.primary;
        j["secondary"] = p.secondary;
        arr.push_back(j);
    }
    return arr;
}

bool ZapretManager::IsSmartDNSEnabled() {
    std::wstring adapterName = GetPrimaryPhysicalAdapterName();
    if (adapterName.empty()) return false;

    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
    if (!pAddrs) return false;

    DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddrs, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        free(pAddrs);
        pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
        if (!pAddrs) return false;
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddrs, &bufLen);
    }

    bool enabled = false;
    if (ret == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES curr = pAddrs; curr; curr = curr->Next) {
            if (curr->FriendlyName && std::wstring(curr->FriendlyName) == adapterName) {
                for (PIP_ADAPTER_DNS_SERVER_ADDRESS pDns = curr->FirstDnsServerAddress; pDns; pDns = pDns->Next) {
                    if (pDns->Address.lpSockaddr->sa_family == AF_INET) {
                        sockaddr_in* sa_dns = (sockaddr_in*)pDns->Address.lpSockaddr;
                        char dnsStr[INET_ADDRSTRLEN] = {0};
                        inet_ntop(AF_INET, &(sa_dns->sin_addr), dnsStr, INET_ADDRSTRLEN);
                        std::string curIp(dnsStr);
                        for (const auto& p : kSupportedDnsProfiles) {
                            if (curIp == p.primary || curIp == p.secondary) {
                                enabled = true;
                                m_activeDnsProfile = p.id;
                                break;
                            }
                        }
                        if (enabled) break;
                    }
                }
                break;
            }
        }
    }
    free(pAddrs);
    return enabled;
}

bool ZapretManager::SetSmartDNS(bool enable, const std::string& profile, std::string& errorMsg) {
    std::wstring wAdapter = GetPrimaryPhysicalAdapterName();
    if (wAdapter.empty()) {
        errorMsg = "Не удалось определить основной сетевой адаптер";
        return false;
    }
    std::string adapterName = WStringToString(wAdapter);
    std::wstring backupPath = m_rootDir + L"\\original_dns.json";

    if (enable) {
        // Resolve profile
        std::string chosenProfile = profile.empty() ? m_activeDnsProfile : profile;
        const DNSProfileDef* matched = nullptr;
        for (const auto& p : kSupportedDnsProfiles) {
            if (chosenProfile == p.id) {
                matched = &p;
                break;
            }
        }
        if (!matched) matched = &kSupportedDnsProfiles[0]; // fallback to comss_ai

        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_activeDnsProfile = matched->id;
        }

        // Backup current DNS before overriding
        nlohmann::json backup;
        backup["adapter"] = adapterName;

        std::vector<std::string> currentDns;
        ULONG bufLen = 15000;
        PIP_ADAPTER_ADDRESSES pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
        if (pAddrs) {
            DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddrs, &bufLen);
            if (ret == ERROR_BUFFER_OVERFLOW) {
                free(pAddrs);
                pAddrs = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
                if (pAddrs) ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddrs, &bufLen);
            }
            if (pAddrs && ret == NO_ERROR) {
                for (PIP_ADAPTER_ADDRESSES curr = pAddrs; curr; curr = curr->Next) {
                    if (curr->FriendlyName && std::wstring(curr->FriendlyName) == wAdapter) {
                        for (PIP_ADAPTER_DNS_SERVER_ADDRESS pDns = curr->FirstDnsServerAddress; pDns; pDns = pDns->Next) {
                            if (pDns->Address.lpSockaddr->sa_family == AF_INET) {
                                sockaddr_in* sa_dns = (sockaddr_in*)pDns->Address.lpSockaddr;
                                char dnsStr[INET_ADDRSTRLEN] = {0};
                                inet_ntop(AF_INET, &(sa_dns->sin_addr), dnsStr, INET_ADDRSTRLEN);
                                std::string curIp(dnsStr);
                                bool isKnown = false;
                                for (const auto& kp : kSupportedDnsProfiles) {
                                    if (curIp == kp.primary || curIp == kp.secondary) {
                                        isKnown = true;
                                        break;
                                    }
                                }
                                if (!isKnown) {
                                    currentDns.push_back(curIp);
                                }
                            }
                        }
                        break;
                    }
                }
                free(pAddrs);
            }
        }
        backup["servers"] = currentDns;
        backup["is_dhcp"] = currentDns.empty();

        try {
            if (!fs::exists(backupPath)) {
                std::ofstream fout(backupPath);
                if (fout.is_open()) fout << backup.dump(4);
            }
        } catch (...) {}

        // Apply selected profile static DNS with validate=no for instantaneous execution (<50ms)!
        std::wstring primaryIp = StringToWString(matched->primary ? matched->primary : "");
        std::wstring secondaryIp = StringToWString(matched->secondary ? matched->secondary : "");
        std::wstring setCmd1 = L"netsh interface ipv4 set dnsservers name=\"" + wAdapter + L"\" static " + primaryIp + L" primary validate=no";
        ExecCommandSyncW(setCmd1);
        if (!secondaryIp.empty()) {
            std::wstring setCmd2 = L"netsh interface ipv4 add dnsservers name=\"" + wAdapter + L"\" " + secondaryIp + L" index=2 validate=no";
            ExecCommandSyncW(setCmd2);
        }

        // PowerShell companion update
        std::wstring psDns = L"powershell.exe -NoProfile -Command \"Set-DnsClientServerAddress -InterfaceAlias '" + wAdapter + L"' -ServerAddresses ('" + primaryIp + L"'" + (secondaryIp.empty() ? L"" : (L",'" + secondaryIp + L"'")) + L")\"";
        ExecCommandSyncW(psDns);

        // Fast in-process DNS cache flush
        typedef BOOL(WINAPI* pDnsFlushResolverCache)();
        HMODULE hDnsApi = LoadLibraryW(L"dnsapi.dll");
        if (hDnsApi) {
            pDnsFlushResolverCache flushFunc = (pDnsFlushResolverCache)GetProcAddress(hDnsApi, "DnsFlushResolverCache");
            if (flushFunc) flushFunc();
            FreeLibrary(hDnsApi);
        }
        ExecCommandSyncW(L"ipconfig /flushdns");
        ExecCommandSyncW(L"powershell.exe -NoProfile -Command \"Clear-DnsClientCache\"");

        AppendLog(std::string("[Smart DNS] Активирован профиль: ") + matched->name + " (" + matched->primary + ", " + matched->secondary + ")");
        return true;
    } else {
        // Restore previous DNS or set to DHCP
        bool isDhcp = true;
        std::vector<std::string> origServers;
        try {
            if (fs::exists(backupPath)) {
                std::ifstream fin(backupPath);
                if (fin.is_open()) {
                    nlohmann::json backup;
                    fin >> backup;
                    isDhcp = backup.value("is_dhcp", true);
                    if (backup.contains("servers") && backup["servers"].is_array()) {
                        for (const auto& s : backup["servers"]) origServers.push_back(s.get<std::string>());
                    }
                }
            }
        } catch (...) {}

        if (isDhcp || origServers.empty()) {
            std::wstring restoreCmd = L"netsh interface ipv4 set dnsservers name=\"" + wAdapter + L"\" dhcp";
            ExecCommandSyncW(restoreCmd);
            std::wstring psReset = L"powershell.exe -NoProfile -Command \"Set-DnsClientServerAddress -InterfaceAlias '" + wAdapter + L"' -ResetServerAddresses\"";
            ExecCommandSyncW(psReset);
        } else {
            std::wstring orig1 = StringToWString(origServers[0]);
            std::wstring setCmd1 = L"netsh interface ipv4 set dnsservers name=\"" + wAdapter + L"\" static " + orig1 + L" primary validate=no";
            ExecCommandSyncW(setCmd1);
            for (size_t i = 1; i < origServers.size(); ++i) {
                std::wstring origI = StringToWString(origServers[i]);
                std::wstring setCmd2 = L"netsh interface ipv4 add dnsservers name=\"" + wAdapter + L"\" " + origI + L" index=" + std::to_wstring(i + 1) + L" validate=no";
                ExecCommandSyncW(setCmd2);
            }
            std::wstring psList = L"";
            for (size_t i = 0; i < origServers.size(); ++i) {
                if (i > 0) psList += L",";
                psList += L"'" + StringToWString(origServers[i]) + L"'";
            }
            std::wstring psSet = L"powershell.exe -NoProfile -Command \"Set-DnsClientServerAddress -InterfaceAlias '" + wAdapter + L"' -ServerAddresses (" + psList + L")\"";
            ExecCommandSyncW(psSet);
        }

        typedef BOOL(WINAPI* pDnsFlushResolverCache)();
        HMODULE hDnsApi = LoadLibraryW(L"dnsapi.dll");
        if (hDnsApi) {
            pDnsFlushResolverCache flushFunc = (pDnsFlushResolverCache)GetProcAddress(hDnsApi, "DnsFlushResolverCache");
            if (flushFunc) flushFunc();
            FreeLibrary(hDnsApi);
        }
        ExecCommandSyncW(L"ipconfig /flushdns");
        ExecCommandSyncW(L"powershell.exe -NoProfile -Command \"Clear-DnsClientCache\"");
        try {
            if (fs::exists(backupPath)) fs::remove(backupPath);
        } catch (...) {}

        AppendLog("[Smart DNS] Подмена геопозиции отключена. Исходные настройки DNS восстановлены.");
        return true;
    }
}

bool ZapretManager::ExtractEmbeddedPayload(HINSTANCE hInstance, const std::wstring& destDir) {
    HRSRC hRes = FindResourceW(hInstance, MAKEINTRESOURCEW(201), RT_RCDATA); // IDR_PAYLOAD_ZIP = 201
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(hInstance, hRes);
    if (!hData) return false;

    DWORD size = SizeofResource(hInstance, hRes);
    void* pData = LockResource(hData);
    if (!pData || size == 0) return false;

    try {
        fs::create_directories(destDir);
        std::wstring zipPath = destDir + L"\\payload.zip";
        std::ofstream fout(zipPath, std::ios::binary);
        if (!fout.is_open()) return false;
        fout.write((const char*)pData, size);
        fout.close();

        // Extract using tar.exe (Windows 10/11 built-in)
        std::wstring tarCmd = L"tar.exe -xf \"" + zipPath + L"\" -C \"" + destDir + L"\"";
        _wsystem(tarCmd.c_str());

        // Check if extraction succeeded; fallback to PowerShell Expand-Archive
        if (!fs::exists(destDir + L"\\resources\\index.html")) {
            std::wstring psCmd = L"powershell.exe -NoProfile -Command \"Expand-Archive -Path '" + zipPath + L"' -DestinationPath '" + destDir + L"' -Force\"";
            _wsystem(psCmd.c_str());
        }

        if (fs::exists(zipPath)) {
            fs::remove(zipPath);
        }
        return fs::exists(destDir + L"\\resources\\index.html");
    } catch (...) {
        return false;
    }
}

