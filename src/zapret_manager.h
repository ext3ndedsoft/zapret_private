#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include "../third_party/json.hpp"

struct PresetInfo {
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    std::string args;
};

struct DiagnosticItem {
    std::string name;
    std::string status; // "pass", "warn", "fail"
    std::string message;
    bool canFix = false;
    std::string fixAction;
};

class ZapretManager {
public:
    static ZapretManager& Instance();

    void Initialize(const std::wstring& rootDir);
    void Shutdown();

    // Presets
    std::vector<PresetInfo> GetPresets();
    PresetInfo GetPreset(const std::string& id);
    std::string GetCurrentPresetId();
    void SetCurrentPresetId(const std::string& id);

    // Process & Service Control
    bool StartProcess(const std::string& presetId, std::string& errorMsg);
    bool StopProcess(std::string& errorMsg);
    bool RestartProcess(const std::string& presetId, std::string& errorMsg);

    bool InstallService(const std::string& presetId, std::string& errorMsg);
    bool RemoveService(std::string& errorMsg);
    bool StartServiceManual(std::string& errorMsg);
    bool StopServiceManual(std::string& errorMsg);

    // Status
    nlohmann::json GetFullStatus();
    bool IsRunning();
    std::string GetRunMode(); // "service", "process", "stopped"

    // Game Filter & IPSet
    std::string GetGameFilterMode(); // "disabled", "all", "tcp", "udp"
    bool SetGameFilterMode(const std::string& mode, std::string& errorMsg);

    std::string GetIPSetMode(); // "loaded", "any", "none"
    bool SetIPSetMode(const std::string& mode, std::string& errorMsg);

    // Active Fakes
    std::vector<std::string> GetAvailableFakeFiles();
    std::string GetActiveDiscordFake();
    std::string GetActiveGameFake();
    bool ReplaceActiveFake(const std::string& type, const std::string& filename, std::string& errorMsg);

    // Lists
    nlohmann::json GetAllLists();
    bool SaveListContent(const std::string& filename, const std::string& content, std::string& errorMsg);

    // Diagnostics & Optimization
    std::vector<DiagnosticItem> RunDiagnostics();
    bool FixTcpTimestamps(std::string& result);
    bool ClearDiscordCache(std::string& result);
    bool KillConflictingProcesses(std::string& result);
    nlohmann::json CheckPings();

    // Deep Network Diagnostics Scanner & Geo/ISP
    nlohmann::json GetDeepNetworkDiagnostics();
    nlohmann::json GetCachedNetworkDiagnostics();
    std::wstring GetPrimaryPhysicalAdapterName();

    // AI Geolocation Spoofing & Smart DNS
    bool SetSmartDNS(bool enable, const std::string& profile, std::string& errorMsg);
    bool IsSmartDNSEnabled();
    std::string GetCurrentDNSProfile();
    nlohmann::json GetAvailableDNSProfiles();

    // Standalone Payload Extraction
    static bool ExtractEmbeddedPayload(HINSTANCE hInstance, const std::wstring& destDir);

    // Preset Generator Engine & Benchmarking
    nlohmann::json AutoBenchmarkNetwork();
    nlohmann::json GenerateCustomPreset(const nlohmann::json& config);
    bool SaveCustomPreset(const std::string& id, const std::string& name, const std::string& description, const std::string& args, std::string& errorMsg);
    bool DeletePreset(const std::string& presetId, std::string& errorMsg);

    // Standalone Persistent Service Helpers
    std::wstring GetServicePayloadDir();
    bool DeployServicePayload(std::string& errorMsg);
    std::string BuildServiceCommandLine(const std::string& presetArgs);

    // Autostart
    bool IsAutostartEnabled();
    bool SetAutostartEnabled(bool enabled);

    // Logs
    std::vector<std::string> GetRecentLogs(size_t maxCount = 200);
    void AppendLog(const std::string& line);
    void ClearLogs();

    // Callback for live logs
    void SetLogCallback(std::function<void(const std::string&)> cb);

private:
    ZapretManager();
    ~ZapretManager();

    std::wstring m_rootDir;
    std::wstring m_binDir;
    std::wstring m_listsDir;
    std::wstring m_utilsDir;

    std::vector<PresetInfo> m_presets;
    std::string m_currentPresetId = "general";
    std::string m_activeDnsProfile = "comss_ai";
    std::recursive_mutex m_mutex;

    // Process handling
    HANDLE m_hProcess = NULL;
    HANDLE m_hThread = NULL;
    HANDLE m_hPipeRead = NULL;
    DWORD m_dwPid = 0;
    std::atomic<bool> m_isProcessRunning{false};
    std::thread m_logReaderThread;

    // Logs
    std::vector<std::string> m_logs;
    std::function<void(const std::string&)> m_logCallback;

    // Helpers
    std::string BuildCommandLine(const std::string& presetArgs);
    std::string CalculateFileHash(const std::wstring& filePath);
    void ReadPipeLoop();
    void CleanupWinDivert();
    void LoadPresetsFromFile();
    std::string ExecCommandSync(const std::string& cmd);
    std::string ExecCommandSyncW(const std::wstring& cmd);
    bool IsServiceRunning(const std::string& serviceName);
    bool IsServiceInstalled(const std::string& serviceName);
};
