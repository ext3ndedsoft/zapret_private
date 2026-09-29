#include "native_bridge.h"
#include "zapret_manager.h"

NativeBridge& NativeBridge::Instance() {
    static NativeBridge s_instance;
    return s_instance;
}

NativeBridge::NativeBridge() {}
NativeBridge::~NativeBridge() {}

void NativeBridge::Initialize(HWND hWnd, std::function<void(const std::string&)> sendWebMessageFunc) {
    m_hWnd = hWnd;
    m_sendWebMessage = sendWebMessageFunc;

    ZapretManager::Instance().SetLogCallback([this](const std::string& line) {
        PushLogLine(line);
    });
}

void NativeBridge::PushLogLine(const std::string& logLine) {
    if (m_sendWebMessage) {
        nlohmann::json evt;
        evt["type"] = "log_event";
        evt["data"] = logLine;
        m_sendWebMessage(evt.dump());
    }
}

std::string NativeBridge::HandleMessage(const std::string& jsonString) {
    nlohmann::json response;
    response["success"] = false;

    try {
        nlohmann::json req = nlohmann::json::parse(jsonString);
        std::string action = req.value("action", "");
        int requestId = req.value("requestId", 0);
        response["requestId"] = requestId;
        response["action"] = action;

        auto& mgr = ZapretManager::Instance();

        if (action == "get_status") {
            response["data"] = mgr.GetFullStatus();
            response["success"] = true;
        } else if (action == "start") {
            std::string presetId = req.value("presetId", "");
            std::string err;
            bool ok = mgr.StartProcess(presetId, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "stop") {
            std::string err;
            bool ok = mgr.StopProcess(err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "restart") {
            std::string presetId = req.value("presetId", "");
            std::string err;
            bool ok = mgr.RestartProcess(presetId, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "install_service") {
            std::string presetId = req.value("presetId", "");
            std::string err;
            bool ok = mgr.InstallService(presetId, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "remove_service") {
            std::string err;
            bool ok = mgr.RemoveService(err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "start_service") {
            std::string err;
            bool ok = mgr.StartServiceManual(err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "stop_service") {
            std::string err;
            bool ok = mgr.StopServiceManual(err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "set_game_filter") {
            std::string mode = req.value("mode", "disabled");
            std::string err;
            bool ok = mgr.SetGameFilterMode(mode, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "set_ipset_mode") {
            std::string mode = req.value("mode", "loaded");
            std::string err;
            bool ok = mgr.SetIPSetMode(mode, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "replace_fake") {
            std::string type = req.value("type", "");
            std::string filename = req.value("filename", "");
            std::string err;
            bool ok = mgr.ReplaceActiveFake(type, filename, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "get_lists") {
            response["data"] = mgr.GetAllLists();
            response["success"] = true;
        } else if (action == "save_list") {
            std::string filename = req.value("filename", "");
            std::string content = req.value("content", "");
            std::string err;
            bool ok = mgr.SaveListContent(filename, content, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
        } else if (action == "run_diagnostics") {
            auto diag = mgr.RunDiagnostics();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& d : diag) {
                nlohmann::json dj;
                dj["name"] = d.name;
                dj["status"] = d.status;
                dj["message"] = d.message;
                dj["canFix"] = d.canFix;
                dj["fixAction"] = d.fixAction;
                arr.push_back(dj);
            }
            response["data"] = arr;
            response["success"] = true;
        } else if (action == "fix_tcp_timestamps") {
            std::string res;
            mgr.FixTcpTimestamps(res);
            response["message"] = res;
            response["success"] = true;
        } else if (action == "clear_discord_cache") {
            std::string res;
            mgr.ClearDiscordCache(res);
            response["message"] = res;
            response["success"] = true;
        } else if (action == "kill_conflicts") {
            std::string res;
            mgr.KillConflictingProcesses(res);
            response["message"] = res;
            response["success"] = true;
        } else if (action == "check_pings") {
            response["data"] = mgr.CheckPings();
            response["success"] = true;
        } else if (action == "scan_network") {
            response["data"] = mgr.GetDeepNetworkDiagnostics();
            response["success"] = true;
        } else if (action == "get_cached_network") {
            response["data"] = mgr.GetCachedNetworkDiagnostics();
            response["success"] = true;
        } else if (action == "set_smart_dns") {
            bool enabled = req.value("enabled", false);
            std::string profile = req.value("profile", "");
            std::string err;
            bool ok = mgr.SetSmartDNS(enabled, profile, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["smart_dns_enabled"] = mgr.IsSmartDNSEnabled();
            response["active_profile"] = mgr.GetCurrentDNSProfile();
        } else if (action == "get_smart_dns_status") {
            response["enabled"] = mgr.IsSmartDNSEnabled();
            response["active_profile"] = mgr.GetCurrentDNSProfile();
            response["profiles"] = mgr.GetAvailableDNSProfiles();
            response["success"] = true;
        } else if (action == "get_dns_profiles") {
            response["profiles"] = mgr.GetAvailableDNSProfiles();
            response["active_profile"] = mgr.GetCurrentDNSProfile();
            response["enabled"] = mgr.IsSmartDNSEnabled();
            response["success"] = true;
        } else if (action == "benchmark_network") {
            response["data"] = mgr.AutoBenchmarkNetwork();
            response["success"] = true;
        } else if (action == "generate_preset") {
            response["data"] = mgr.GenerateCustomPreset(req);
            response["success"] = true;
        } else if (action == "save_preset") {
            std::string id = req.value("id", "custom_generated");
            std::string name = req.value("name", "Идеальный адаптивный пресет");
            std::string desc = req.value("description", "Сгенерированная индивидуальная стратегия");
            std::string args = req.value("args", "");
            std::string err;
            bool ok = mgr.SaveCustomPreset(id, name, desc, args, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "delete_preset") {
            std::string presetId = req.value("presetId", "");
            std::string err;
            bool ok = mgr.DeletePreset(presetId, err);
            response["success"] = ok;
            if (!ok) response["error"] = err;
            response["status"] = mgr.GetFullStatus();
        } else if (action == "get_logs") {
            response["data"] = mgr.GetRecentLogs();
            response["success"] = true;
        } else if (action == "clear_logs") {
            mgr.ClearLogs();
            response["success"] = true;
        } else if (action == "set_autostart") {
            bool enabled = req.value("enabled", false);
            bool ok = mgr.SetAutostartEnabled(enabled);
            response["success"] = ok;
            response["autostart"] = mgr.IsAutostartEnabled();
        } else if (action == "window_minimize") {
            if (m_hWnd) ShowWindow(m_hWnd, SW_MINIMIZE);
            response["success"] = true;
        } else if (action == "window_maximize") {
            if (m_hWnd) {
                if (IsZoomed(m_hWnd)) {
                    ShowWindow(m_hWnd, SW_RESTORE);
                } else {
                    ShowWindow(m_hWnd, SW_MAXIMIZE);
                }
            }
            response["success"] = true;
        } else if (action == "window_close") {
            if (m_hWnd) {
                // Minimize to tray instead of quitting
                ShowWindow(m_hWnd, SW_HIDE);
            }
            response["success"] = true;
        } else if (action == "app_quit") {
            if (m_hWnd) PostMessage(m_hWnd, WM_CLOSE, 0, 0);
            response["success"] = true;
        } else if (action == "window_drag") {
            if (m_hWnd) {
                ReleaseCapture();
                SendMessage(m_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            response["success"] = true;
        } else {
            response["error"] = "Неизвестное действие: " + action;
        }

    } catch (const std::exception& ex) {
        response["error"] = std::string("Ошибка обработки запроса: ") + ex.what();
    }

    try {
        return response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return "{\"error\":\"Serialization error\",\"success\":false}";
    }
}
