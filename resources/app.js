// Zapret Private - Frontend State Engine

let currentRequestId = 1;
const pendingRequests = new Map();
let appState = {
    running: false,
    mode: "stopped",
    currentPreset: "ultimate_hybrid",
    gameFilter: "udp",
    ipsetStatus: "loaded",
    activeDiscordFake: "",
    activeGameFake: "",
    presets: [],
    availableFakes: []
};

let activeListFile = "list-general-user.txt";
let allListsData = {};
let lastScanData = null;
let currentClusterTab = "games";

// Send IPC message to C++ backend
function sendIpc(action, payload = {}) {
    return new Promise((resolve, reject) => {
        const reqId = currentRequestId++;
        const msg = {
            action: action,
            requestId: reqId,
            ...payload
        };

        pendingRequests.set(reqId, { resolve, reject });

        if (window.chrome && window.chrome.webview) {
            window.chrome.webview.postMessage(msg);
        } else {
            console.warn("WebView2 IPC not available. Mocking response for action:", action);
            setTimeout(() => {
                resolve({ success: true, mock: true });
            }, 100);
        }
    });
}

// Receive messages from C++ backend
if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', (event) => {
        const msg = event.data;
        if (!msg) return;

        // Log push event
        if (msg.type === "log_event") {
            appendTerminalLine(msg.data);
            return;
        }

        // Request-Response matching
        if (msg.requestId && pendingRequests.has(msg.requestId)) {
            const { resolve } = pendingRequests.get(msg.requestId);
            pendingRequests.delete(msg.requestId);
            resolve(msg);
        }
    });
}

// Toast notification helper
function showToast(text, type = "info") {
    const container = document.getElementById("toastContainer");
    const toast = document.createElement("div");
    toast.className = "toast";
    toast.innerText = text;
    container.appendChild(toast);
    setTimeout(() => {
        toast.style.opacity = "0";
        toast.style.transform = "translateY(10px)";
        setTimeout(() => toast.remove(), 300);
    }, 3200);
}

// Initialize Application
async function initApp() {
    setupTitlebar();
    setupNavigation();
    setupControls();

    try {
        const res = await sendIpc("get_status");
        if (res.success && res.data) {
            updateUIWithState(res.data);
        }
    } catch (e) {
        console.error("Init failed:", e);
    }

    refreshPings();
    loadLists();
    initGenerator();
    initNetScan();

    try {
        const dnsRes = await sendIpc("get_smart_dns_status");
        if (dnsRes && dnsRes.success) {
            updateSmartDnsUI(dnsRes.enabled);
        }
    } catch (e) {}

    setInterval(refreshPings, 15000);
    setInterval(refreshStatus, 4000);
}

async function refreshStatus() {
    try {
        const res = await sendIpc("get_status");
        if (res.success && res.data) {
            updateUIWithState(res.data);
        }
    } catch (e) {}
}

function updateUIWithState(state) {
    appState = state;

    const isRunning = state.running;
    const mode = state.mode; // "service", "process", "stopped"

    // Titlebar Pill
    const pill = document.getElementById("titlebarStatusPill");
    const pillText = document.getElementById("titlebarStatusText");
    if (isRunning) {
        pill.classList.add("active");
        pillText.innerText = mode === "service" ? "Служба активна" : "Обход активен";
    } else {
        pill.classList.remove("active");
        pillText.innerText = "Отключен";
    }

    // Hero Control Card
    const heroCard = document.querySelector(".hero-control-card");
    const engineText = document.getElementById("engineStateText");
    const powerLabel = document.getElementById("btnMasterPowerLabel");

    if (isRunning) {
        heroCard.classList.add("active");
        engineText.innerText = mode === "service" ? "Служба Windows работает" : "DPI-десинк активен (WinDivert)";
        powerLabel.innerText = "ВЫКЛЮЧИТЬ";
    } else {
        heroCard.classList.remove("active");
        engineText.innerText = "Готов к запуску";
        powerLabel.innerText = "ВКЛЮЧИТЬ";
    }

    // Telemetry Cards
    document.getElementById("cardStrategyName").innerText = state.currentPreset || "general";
    document.getElementById("cardStrategyTag").innerText = (state.currentPreset || "GENERAL").toUpperCase();

    const gfVal = state.gameFilter || "disabled";
    const gfTag = document.getElementById("cardGameFilterTag");
    if (gfTag) gfTag.innerText = gfVal.toUpperCase();
    const gfValEl = document.getElementById("cardGameFilterVal");
    if (gfValEl) gfValEl.innerText = gfVal === "disabled" ? "Отключен" : (gfVal === "all" ? "TCP & UDP (1024-65535)" : (gfVal === "udp" ? "UDP (1024-65535)" : "TCP (1024-65535)"));

    // Radio sync in Gaming tab
    const radios = document.getElementsByName("gfMode");
    for (let r of radios) {
        if (r.value === gfVal) r.checked = true;
    }

    // Service Card
    const srvTag = document.getElementById("cardServiceTag");
    const srvVal = document.getElementById("cardServiceVal");
    const srvBtn = document.getElementById("btnToggleServiceMode");

    if (state.serviceInstalled) {
        srvTag.innerText = state.serviceRunning ? "SERVICE ACTIVE" : "INSTALLED";
        srvTag.className = "stat-tag " + (state.serviceRunning ? "emerald-tag" : "gold-tag");
        srvVal.innerText = state.serviceRunning ? "Служба Windows (Авто)" : "Служба остановлена";
        srvBtn.innerText = "Удалить службу";
    } else {
        srvTag.innerText = "NOT INSTALLED";
        srvTag.className = "stat-tag";
        srvVal.innerText = "Фоновый процесс";
        srvBtn.innerText = "Установить службу";
    }

    // Sidebar status
    document.getElementById("sbModeVal").innerText = isRunning ? (mode === "service" ? "Служба" : "Процесс") : "Standby";

    // Presets Grid
    if (state.presets && state.presets.length > 0) {
        renderPresets(state.presets, state.currentPreset);
    }

    // Fakes dropdowns
    if (state.availableFakes && state.availableFakes.length > 0) {
        renderFakesDropdowns(state.availableFakes, state.activeDiscordFake, state.activeGameFake);
    }
}

// Luxury Confirmation Modal with Smooth Backdrop Blur
function showLuxuryConfirm({
    title = "Удаление пресета",
    question = "Вы точно хотите удалить данный пресет?",
    presetName = "Пресет",
    presetDesc = "Конфигурация обхода",
    confirmText = "Да, удалить",
    cancelText = "Отмена"
} = {}) {
    return new Promise((resolve) => {
        const modal = document.getElementById("confirmModal");
        if (!modal) {
            resolve(window.confirm(question + ` (${presetName})`));
            return;
        }

        const titleEl = document.getElementById("confirmModalTitle");
        const qEl = document.getElementById("confirmModalQuestion");
        const nameEl = document.getElementById("confirmModalPresetName");
        const descEl = document.getElementById("confirmModalPresetDesc");
        const btnConfirm = document.getElementById("btnModalConfirm");
        const btnCancel = document.getElementById("btnModalCancel");

        if (titleEl) titleEl.innerText = title;
        if (qEl) qEl.innerText = question;
        if (nameEl) nameEl.innerText = presetName;
        if (descEl) descEl.innerText = presetDesc;

        let isClosed = false;

        const cleanup = (result) => {
            if (isClosed) return;
            isClosed = true;

            modal.classList.remove("is-active");
            window.removeEventListener("keydown", onKeyDown);
            btnConfirm.removeEventListener("click", onConfirm);
            btnCancel.removeEventListener("click", onCancel);
            modal.removeEventListener("click", onBackdropClick);

            setTimeout(() => {
                resolve(result);
            }, 320);
        };

        const onConfirm = () => cleanup(true);
        const onCancel = () => cleanup(false);
        const onBackdropClick = (e) => {
            if (e.target === modal) cleanup(false);
        };
        const onKeyDown = (e) => {
            if (e.key === "Escape") cleanup(false);
            else if (e.key === "Enter") cleanup(true);
        };

        btnConfirm.addEventListener("click", onConfirm);
        btnCancel.addEventListener("click", onCancel);
        modal.addEventListener("click", onBackdropClick);
        window.addEventListener("keydown", onKeyDown);

        requestAnimationFrame(() => {
            modal.classList.add("is-active");
        });
    });
}

// Presets rendering and filtering
function renderPresets(presets, activePresetId) {
    const container = document.getElementById("presetsGridContainer");
    if (!container) return;
    const countBadge = document.getElementById("presetsCountBadge");
    if (countBadge) countBadge.innerText = presets ? presets.length : 0;

    const searchInput = document.getElementById("presetSearchInput");
    const searchTerm = searchInput ? searchInput.value.toLowerCase() : "";
    const activeFilter = document.querySelector(".filter-pill.active")?.getAttribute("data-filter") || "all";

    container.innerHTML = "";

    const filtered = presets.filter(p => {
        const matchesSearch = p.name.toLowerCase().includes(searchTerm) || p.description.toLowerCase().includes(searchTerm);
        const matchesCategory = (activeFilter === "all") || (p.category === activeFilter);
        return matchesSearch && matchesCategory;
    });

    filtered.forEach(p => {
        const card = document.createElement("div");
        const isActive = (p.id === activePresetId);
        card.className = "preset-card" + (isActive ? " is-active-preset" : "");

        card.innerHTML = `
            <div>
                <div class="preset-card-top">
                    <span class="preset-card-title">${p.name}</span>
                    <span class="preset-card-category">${p.category}</span>
                </div>
                <div class="preset-card-desc">${p.description}</div>
            </div>
            <div class="preset-card-actions">
                <button class="btn-luxury-primary btn-apply-preset" data-id="${p.id}">
                    ${isActive && appState.running ? "Активен" : "Применить"}
                </button>
                <button class="btn-luxury-secondary btn-service-preset" data-id="${p.id}">
                    В службу
                </button>
                <button class="btn-delete-preset" data-id="${p.id}" title="Удалить пресет">
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
                </button>
            </div>
        `;

        card.querySelector(".btn-apply-preset").addEventListener("click", () => applyPreset(p.id));
        card.querySelector(".btn-service-preset").addEventListener("click", () => installAsService(p.id));
        card.querySelector(".btn-delete-preset").addEventListener("click", async (e) => {
            e.stopPropagation();
            const confirmed = await showLuxuryConfirm({
                title: "Удаление пресета",
                question: "Вы точно хотите удалить данный пресет?",
                presetName: p.name,
                presetDesc: p.description || p.category || "Конфигурация обхода DPI",
                confirmText: "Да, удалить",
                cancelText: "Отмена"
            });

            if (confirmed) {
                showToast(`Удаление пресета: ${p.name}...`);
                const res = await sendIpc("delete_preset", { presetId: p.id });
                if (res.success) {
                    showToast(`Пресет «${p.name}» успешно удален!`);
                    if (res.status) {
                        updateUIWithState(res.status);
                    }
                } else {
                    showToast(`Ошибка удаления: ${res.error || "Сбой"}`, "error");
                }
            }
        });

        container.appendChild(card);
    });
}

function renderFakesDropdowns(fakes, activeDiscord, activeGame) {
    const selDiscord = document.getElementById("selectDiscordFake");
    const selGame = document.getElementById("selectGameFake");

    if (selDiscord.children.length === 0) {
        fakes.forEach(f => {
            const optD = document.createElement("option");
            optD.value = f;
            optD.innerText = f;
            if (f === activeDiscord) optD.selected = true;
            selDiscord.appendChild(optD);

            const optG = document.createElement("option");
            optG.value = f;
            optG.innerText = f;
            if (f === activeGame) optG.selected = true;
            selGame.appendChild(optG);
        });
    }
}

async function applyPreset(presetId) {
    showToast(`Запуск пресета: ${presetId}...`);
    try {
        const res = await sendIpc("start", { presetId });
        if (res.success) {
            showToast(`Пресет ${presetId} успешно запущен!`);
            updateUIWithState(res.status);
        } else {
            showToast(`Ошибка: ${res.error || "Не удалось запустить"}`, "error");
        }
    } catch (e) {
        showToast("Ошибка связи с ядром", "error");
    }
}

async function installAsService(presetId) {
    showToast(`Установка постоянной службы Windows (${presetId})...`);
    try {
        const res = await sendIpc("install_service", { presetId });
        if (res.success) {
            showToast(`Служба zapret установлена и надежно закреплена!`);
            updateUIWithState(res.status);
        } else {
            showToast(`Ошибка установки службы: ${res.error}`, "error");
        }
    } catch (e) {
        showToast("Ошибка связи", "error");
    }
}

// Master Power Button Action
async function toggleMasterPower() {
    if (appState.running) {
        showToast("Остановка обхода...");
        const res = (appState.mode === "service")
            ? await sendIpc("stop_service")
            : await sendIpc("stop");
        if (res.success) {
            showToast("Обход успешно отключен");
            updateUIWithState(res.status);
        } else {
            showToast(`Ошибка: ${res.error || "Не удалось отключить"}`, "error");
        }
    } else {
        const preset = appState.currentPreset || "ultimate_hybrid";
        if (appState.serviceInstalled) {
            showToast(`Запуск службы (${preset})...`);
            const res = await sendIpc("start_service");
            if (res.success) {
                showToast("Служба zapret успешно запущена!");
                updateUIWithState(res.status);
            } else {
                showToast(`Ошибка: ${res.error}`, "error");
            }
        } else {
            showToast(`Запуск обхода (${preset})...`);
            const res = await sendIpc("start", { presetId: preset });
            if (res.success) {
                showToast("Обход успешно запущен!");
                updateUIWithState(res.status);
            } else {
                showToast(`Ошибка: ${res.error}`, "error");
            }
        }
    }
}

// Setup Titlebar Controls
function setupTitlebar() {
    document.getElementById("btnMinimize")?.addEventListener("click", () => sendIpc("window_minimize"));
    document.getElementById("btnMaximize")?.addEventListener("click", () => sendIpc("window_maximize"));
    document.getElementById("btnClose")?.addEventListener("click", () => sendIpc("window_close"));

    // Native window drag
    document.querySelectorAll("[data-drag]").forEach(el => {
        el.addEventListener("mousedown", (e) => {
            if (e.target.closest("button") || e.target.closest("input")) return;
            sendIpc("window_drag");
        });
    });
}

function switchTab(tabName) {
    const navItems = document.querySelectorAll(".nav-item");
    navItems.forEach(item => {
        if (item.getAttribute("data-tab") === tabName) {
            item.click();
        }
    });
}

// Setup Navigation Tabs
function setupNavigation() {
    const navItems = document.querySelectorAll(".nav-item");

    navItems.forEach(item => {
        item.addEventListener("click", () => {
            navItems.forEach(i => i.classList.remove("active"));
            item.classList.add("active");

            const tabName = item.getAttribute("data-tab");
            document.querySelectorAll(".tab-pane").forEach(pane => {
                pane.classList.remove("active");
            });

            const activePane = document.getElementById(`pane-${tabName}`);
            if (activePane) {
                activePane.classList.add("active");
            }
        });
    });
}

// Setup Controls
function setupControls() {
    document.getElementById("btnMasterPower").addEventListener("click", toggleMasterPower);

    document.getElementById("btnQuickSwitchPreset").addEventListener("click", () => {
        switchTab("presets");
    });

    const btnAi = document.getElementById("btnDashToggleAi");
    if (btnAi) btnAi.addEventListener("click", toggleSmartDns);

    const selectDns = document.getElementById("selectDnsProfile");
    if (selectDns) {
        selectDns.addEventListener("change", async () => {
            const btn = document.getElementById("btnDashToggleAi");
            if (btn?.getAttribute("data-active") === "true") {
                const profile = selectDns.value;
                showToast(`Переключение DNS на: ${profile}...`);
                const res = await sendIpc("set_smart_dns", { enabled: true, profile: profile });
                if (res.success) {
                    updateSmartDnsUI(true, res.active_profile || profile);
                    showToast("Профиль DNS успешно применен!");
                }
            }
        });
    }

    const btnFilterQuick = document.getElementById("btnToggleGameFilterQuick");
    if (btnFilterQuick) {
        btnFilterQuick.addEventListener("click", () => {
            switchTab("presets");
            setTimeout(() => {
                document.getElementById("presetsExtraSettings")?.scrollIntoView({ behavior: "smooth" });
            }, 100);
        });
    }

    document.getElementById("btnToggleServiceMode").addEventListener("click", async () => {
        if (appState.serviceInstalled) {
            showToast("Удаление службы Windows...");
            const res = await sendIpc("remove_service");
            if (res.success) {
                showToast("Служба удалена");
                updateUIWithState(res.status);
            }
        } else {
            installAsService(appState.currentPreset || "ultimate_hybrid");
        }
    });

    // Preset Search and Filter Pills
    document.getElementById("presetSearchInput").addEventListener("input", () => {
        renderPresets(appState.presets, appState.currentPreset);
    });

    document.querySelectorAll(".filter-pill").forEach(pill => {
        pill.addEventListener("click", () => {
            document.querySelectorAll(".filter-pill").forEach(p => p.classList.remove("active"));
            pill.classList.add("active");
            renderPresets(appState.presets, appState.currentPreset);
        });
    });

    // Gaming Tab Actions
    const btnGaming = document.getElementById("btnApplyGamingUdpPreset");
    if (btnGaming) btnGaming.addEventListener("click", () => applyPreset("gaming_udp"));
    const btnUlt = document.getElementById("btnApplyUltimatePreset");
    if (btnUlt) btnUlt.addEventListener("click", () => applyPreset("ultimate_hybrid"));

    // Game filter radio change
    document.getElementsByName("gfMode").forEach(radio => {
        radio.addEventListener("change", async (e) => {
            const mode = e.target.value;
            showToast(`Установка фильтра портов: ${mode}...`);
            const res = await sendIpc("set_game_filter", { mode });
            if (res.success) {
                showToast("Режим фильтра обновлен. Перезапустите обход.");
                updateUIWithState(res.status);
            }
        });
    });

    // Fakes
    document.getElementById("btnApplyDiscordFake").addEventListener("click", async () => {
        const val = document.getElementById("selectDiscordFake").value;
        const res = await sendIpc("replace_fake", { type: "discord", filename: val });
        if (res.success) {
            showToast(`Discord UDP фейк заменен на: ${val}`);
        }
    });

    document.getElementById("btnApplyGameFake").addEventListener("click", async () => {
        const val = document.getElementById("selectGameFake").value;
        const res = await sendIpc("replace_fake", { type: "game", filename: val });
        if (res.success) {
            showToast(`Game UDP фейк заменен на: ${val}`);
        }
    });

    // Discord Cache
    document.getElementById("btnClearDiscordCache").addEventListener("click", async () => {
        showToast("Очистка кэша Discord...");
        const res = await sendIpc("clear_discord_cache");
        showToast(res.message || "Кэш очищен");
    });

    // Lists Tab
    document.querySelectorAll(".list-tab-btn").forEach(btn => {
        btn.addEventListener("click", () => {
            document.querySelectorAll(".list-tab-btn").forEach(b => b.classList.remove("active"));
            btn.classList.add("active");
            activeListFile = btn.getAttribute("data-file");
            displayActiveList();
        });
    });

    document.getElementById("btnSaveActiveList").addEventListener("click", async () => {
        const content = document.getElementById("listTextEditor").value;
        showToast(`Сохранение ${activeListFile}...`);
        const res = await sendIpc("save_list", { filename: activeListFile, content });
        if (res.success) {
            allListsData[activeListFile] = content;
            showToast("Список успешно сохранен!");
        } else {
            showToast(`Ошибка сохранения: ${res.error}`, "error");
        }
    });

    // Diagnostics Tab
    const btnDiagLegacy = document.getElementById("btnRunDiagnostics");
    if (btnDiagLegacy) btnDiagLegacy.addEventListener("click", runDiagnostics);
    const btnDiagSys = document.getElementById("btnRunSysDiagnostics");
    if (btnDiagSys) btnDiagSys.addEventListener("click", runDiagnostics);

    // Terminal
    document.getElementById("btnClearTerminal").addEventListener("click", () => {
        document.getElementById("terminalOutput").innerHTML = "";
    });

    document.getElementById("btnCopyTerminal").addEventListener("click", () => {
        const text = document.getElementById("terminalOutput").innerText;
        navigator.clipboard.writeText(text);
        showToast("Логи скопированы в буфер обмена");
    });

    document.getElementById("btnRefreshPings").addEventListener("click", refreshPings);
}

// Load and display lists
async function loadLists() {
    try {
        const res = await sendIpc("get_lists");
        if (res.success && res.data) {
            allListsData = res.data;
            displayActiveList();
        }
    } catch (e) {}
}

function displayActiveList() {
    document.getElementById("editorFilename").innerText = activeListFile;
    const content = allListsData[activeListFile] || "";
    const editor = document.getElementById("listTextEditor");
    editor.value = content;

    const lines = content.split("\n").filter(l => l.trim().length > 0 && !l.trim().startsWith("#")).length;
    document.getElementById("editorStats").innerText = `${lines} записей | UTF-8`;
}

// Latency Radar Check
async function refreshPings() {
    try {
        const res = await sendIpc("check_pings");
        if (res.success && res.data) {
            updatePingBadge("pingDiscord", res.data.discord);
            updatePingBadge("pingYoutube", res.data.youtube);
            updatePingBadge("pingCloudflare", res.data.cloudflare);
            updatePingBadge("pingGoogle", res.data.google_dns);

            const cfPing = res.data.cloudflare;
            if (cfPing > 0) {
                document.getElementById("sbPingVal").innerText = `${cfPing} ms`;
            }
        }
    } catch (e) {}
}

function updatePingBadge(elId, ms) {
    const el = document.getElementById(elId);
    if (!el) return;

    if (ms <= 0 || ms == null) {
        el.innerText = "Timeout";
        el.className = "ping-badge-val slow";
    } else {
        el.innerText = `${ms} ms`;
        if (ms < 50) {
            el.className = "ping-badge-val fast";
        } else if (ms < 120) {
            el.className = "ping-badge-val medium";
        } else {
            el.className = "ping-badge-val slow";
        }
    }
}

// Diagnostics
async function runDiagnostics() {
    showToast("Выполняется диагностика системы...");
    const container = document.getElementById("diagnosticsList");
    if (!container) return;
    container.innerHTML = `<div style="color: var(--text-tertiary); padding: 16px;">Сканирование компонентов Windows, BFE, сетевых фильтров и драйверов...</div>`;

    try {
        const res = await sendIpc("run_diagnostics");
        if (res.success && res.data) {
            container.innerHTML = "";
            res.data.forEach(item => {
                const card = document.createElement("div");
                card.className = "diag-card";

                let iconColorClass = "diag-pass";
                let iconSvg = `<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="20 6 9 17 4 12"/></svg>`;

                if (item.status === "warn") {
                    iconColorClass = "diag-warn";
                    iconSvg = `<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>`;
                } else if (item.status === "fail") {
                    iconColorClass = "diag-fail";
                    iconSvg = `<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>`;
                }

                card.innerHTML = `
                    <div class="diag-left">
                        <div class="diag-status-icon ${iconColorClass}">
                            ${iconSvg}
                        </div>
                        <div>
                            <div class="diag-name">${item.name}</div>
                            <div class="diag-msg">${item.message}</div>
                        </div>
                    </div>
                    <div>
                        ${item.canFix ? `<button class="btn-luxury-primary btn-diag-fix" data-action="${item.fixAction}">Исправить</button>` : ''}
                    </div>
                `;

                if (item.canFix) {
                    card.querySelector(".btn-diag-fix").addEventListener("click", () => handleDiagFix(item.fixAction));
                }

                container.appendChild(card);
            });
            showToast("Диагностика завершена");
        }
    } catch (e) {
        showToast("Ошибка проведения диагностики", "error");
    }
}

async function handleDiagFix(action) {
    showToast(`Применение исправления (${action})...`);
    if (action === "fix_tcp_timestamps") {
        const res = await sendIpc("fix_tcp_timestamps");
        showToast(res.message || "TCP Timestamps включены");
    } else if (action === "kill_conflicts") {
        const res = await sendIpc("kill_conflicts");
        showToast(res.message || "Конфликты устранены");
    }
    setTimeout(runDiagnostics, 800);
}

// Terminal Append Line
function appendTerminalLine(line) {
    const term = document.getElementById("terminalOutput");
    const div = document.createElement("div");
    div.className = "terminal-line";

    if (line.includes("[ERROR]") || line.includes("failed")) {
        div.innerHTML = `<span class="term-err">${escapeHtml(line)}</span>`;
    } else if (line.includes("[Zapret]") || line.includes("[Service]")) {
        div.innerHTML = `<span class="term-info">${escapeHtml(line)}</span>`;
    } else {
        div.innerText = line;
    }

    term.appendChild(div);

    if (document.getElementById("chkAutoScroll").checked) {
        term.scrollTop = term.scrollHeight;
    }
}

function escapeHtml(text) {
    return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// ----------------------------------------------------
// PRESET GENERATOR ENGINE (AI / SMART TUNING)
// ----------------------------------------------------
let lastGeneratedPreset = null;

function initGenerator() {
    const inputs = [
        "genChkGames", "genChkYoutube", "genChkAI", "genChkDiscord",
        "genSplitPos", "genRepeats", "genAutoTtl", "genFakeTls"
    ];

    inputs.forEach(id => {
        const el = document.getElementById(id);
        if (el) {
            el.addEventListener("change", () => updateGeneratedPreview(false));
        }
    });

    const providerDefaults = {
        "auto": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "rostelecom": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "domru": { splitPos: 2, repeats: 8, autoTtl: 1 },
        "mts": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "beeline": { splitPos: 1, repeats: 11, autoTtl: 3 },
        "ttk": { splitPos: 1, repeats: 8, autoTtl: 3 },
        "ufanet": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "intersvyaz": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "tattelecom": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "citylink": { splitPos: 1, repeats: 8, autoTtl: 3 },
        "regional": { splitPos: 1, repeats: 8, autoTtl: 2 },
        "aggressive": { splitPos: 1, repeats: 12, autoTtl: 2 }
    };

    // Provider Tiles Selection
    document.querySelectorAll(".provider-tile").forEach(tile => {
        tile.addEventListener("click", () => {
            document.querySelectorAll(".provider-tile").forEach(t => t.classList.remove("active"));
            tile.classList.add("active");
            const prov = tile.getAttribute("data-provider");
            const input = document.getElementById("genProviderPreset");
            if (input) input.value = prov;

            // Auto-update controls to provider recommended values
            if (prov && providerDefaults[prov] && prov !== "auto") {
                const defs = providerDefaults[prov];
                const splitEl = document.getElementById("genSplitPos");
                const repEl = document.getElementById("genRepeats");
                const ttlEl = document.getElementById("genAutoTtl");
                if (splitEl) splitEl.value = defs.splitPos.toString();
                if (repEl) repEl.value = defs.repeats.toString();
                if (ttlEl) ttlEl.value = defs.autoTtl.toString();
            }

            updateGeneratedPreview(false);
        });
    });

    const btnManual = document.getElementById("btnGeneratePresetManual");
    if (btnManual) {
        btnManual.addEventListener("click", () => updateGeneratedPreview(true));
    }

    const btnBenchmark = document.getElementById("btnAutoBenchmarkRun");
    if (btnBenchmark) {
        btnBenchmark.addEventListener("click", runAutoBenchmark);
    }

    const btnCopy = document.getElementById("btnCopyGeneratedCmd");
    if (btnCopy) {
        btnCopy.addEventListener("click", () => {
            const txt = document.getElementById("genArgsPreview").value;
            if (txt) {
                navigator.clipboard.writeText(txt);
                showToast("Командная строка скопирована в буфер обмена");
            }
        });
    }

    const btnApply = document.getElementById("btnApplyGeneratedNow");
    if (btnApply) {
        btnApply.addEventListener("click", applyGeneratedPreset);
    }

    const btnSave = document.getElementById("btnSaveGeneratedToPresets");
    if (btnSave) {
        btnSave.addEventListener("click", saveGeneratedPresetToLibrary);
    }

    // Initial preview generation
    updateGeneratedPreview(false);
}

async function updateGeneratedPreview(showNotification = false) {
    const config = {
        provider: document.getElementById("genProviderPreset")?.value || "auto",
        includeGames: document.getElementById("genChkGames")?.checked ?? true,
        includeYoutube: document.getElementById("genChkYoutube")?.checked ?? true,
        includeAI: document.getElementById("genChkAI")?.checked ?? true,
        includeDiscord: document.getElementById("genChkDiscord")?.checked ?? true,
        splitPos: parseInt(document.getElementById("genSplitPos")?.value) || 1,
        repeats: parseInt(document.getElementById("genRepeats")?.value) || 8,
        autoTtl: parseInt(document.getElementById("genAutoTtl")?.value) || 2,
        fakeTls: document.getElementById("genFakeTls")?.value || "tls_clienthello_www_google_com.bin"
    };

    try {
        const res = await sendIpc("generate_preset", config);
        if (res.success && res.data) {
            lastGeneratedPreset = res.data;
            const preview = document.getElementById("genArgsPreview");
            if (preview) {
                preview.value = res.data.args;
                preview.style.boxShadow = "0 0 16px rgba(223, 180, 102, 0.4)";
                setTimeout(() => { preview.style.boxShadow = ""; }, 600);
            }
            if (res.data.detected_isp && config.provider === "auto") {
                const titleEl = document.getElementById("benchmarkTitle");
                if (titleEl) {
                    titleEl.innerText = `Провайдер определен: ${res.data.detected_isp} (${res.data.name})`;
                }
                if (res.data.optimal_repeats) {
                    const splitEl = document.getElementById("genSplitPos");
                    const repEl = document.getElementById("genRepeats");
                    const ttlEl = document.getElementById("genAutoTtl");
                    if (splitEl && res.data.optimal_split_pos) splitEl.value = res.data.optimal_split_pos.toString();
                    if (repEl && res.data.optimal_repeats) repEl.value = res.data.optimal_repeats.toString();
                    if (ttlEl && res.data.optimal_autottl) ttlEl.value = res.data.optimal_autottl.toString();
                }
            } else if (config.provider !== "auto") {
                const titleEl = document.getElementById("benchmarkTitle");
                if (titleEl) {
                    titleEl.innerText = `Выбран профиль: ${res.data.name}`;
                }
            }
            const descEl = document.getElementById("benchmarkDesc");
            if (descEl && res.data.description) {
                descEl.innerText = res.data.description;
            }
            if (showNotification) {
                showToast(`${res.data.name || 'Идеальный пресет'} успешно сформирован!`);
            }
        }
    } catch (e) {
        console.error("Failed to generate preview:", e);
        if (showNotification) {
            showToast("Ошибка при генерации конфигурации", "error");
        }
    }
}

async function runAutoBenchmark() {
    const btn = document.getElementById("btnAutoBenchmarkRun");
    const origHtml = btn ? btn.innerHTML : "";
    if (btn) {
        btn.disabled = true;
        btn.innerHTML = `<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 2a10 10 0 0 1 10 10"/></svg> Калибровка...`;
    }

    showToast("Калибровка сети: зондирование TTL до ТСПУ и DPI...");

    try {
        const res = await sendIpc("benchmark_network");
        if (res.success && res.data) {
            const b = res.data;
            const hopsEl = document.getElementById("statDpiHops");
            const ttlEl = document.getElementById("statOptTtl");
            const repEl = document.getElementById("statRepeats");
            const fakeEl = document.getElementById("statFakeSni");
            const titleEl = document.getElementById("benchmarkTitle");
            const descEl = document.getElementById("benchmarkDesc");

            if (hopsEl) hopsEl.innerText = b.detected_dpi_hops != null ? b.detected_dpi_hops : "3";
            if (ttlEl) ttlEl.innerText = b.recommended_autottl != null ? b.recommended_autottl : "2";
            if (repEl) repEl.innerText = (b.recommended_repeats || 8) + "x";
            if (fakeEl) fakeEl.innerText = "Google TLS";

            if (titleEl) titleEl.innerText = `Калибровка завершена: ТСПУ на шаге ${b.detected_dpi_hops || 3}`;
            if (descEl) descEl.innerText = b.summary || "Оптимальные параметры десинхронизации успешно подобраны.";

            // Apply to fine tuning select controls
            const ttlSel = document.getElementById("genAutoTtl");
            if (ttlSel && b.recommended_autottl) ttlSel.value = b.recommended_autottl.toString();

            const repSel = document.getElementById("genRepeats");
            if (repSel && b.recommended_repeats) repSel.value = b.recommended_repeats.toString();

            const splitSel = document.getElementById("genSplitPos");
            if (splitSel && b.recommended_split_pos) splitSel.value = b.recommended_split_pos.toString();

            showToast("Калибровка завершена! Оптимальные параметры выставлены.");
            await updateGeneratedPreview();
        } else {
            showToast("Не удалось завершить калибровку", "error");
        }
    } catch (e) {
        showToast("Ошибка связи при калибровке", "error");
    } finally {
        if (btn) {
            btn.disabled = false;
            btn.innerHTML = origHtml;
        }
    }
}

async function applyGeneratedPreset() {
    if (!lastGeneratedPreset) {
        await updateGeneratedPreview();
    }
    if (!lastGeneratedPreset) {
        showToast("Ошибка: конфигурация не сформирована", "error");
        return;
    }

    showToast("Применение идеального пресета...");
    try {
        const customId = "custom_generated";
        const customName = lastGeneratedPreset.name || "Индивидуальный пресет";
        const saveRes = await sendIpc("save_preset", {
            id: customId,
            name: customName,
            description: lastGeneratedPreset.description || "Адаптивный синтезированный пресет",
            args: lastGeneratedPreset.args
        });

        if (saveRes.success) {
            const startRes = await sendIpc("start", { presetId: customId });
            if (startRes.success) {
                showToast("Идеальный пресет успешно применен и запущен!");
                if (startRes.status) {
                    updateUIWithState(startRes.status);
                }
                switchTab("dashboard");
            } else {
                showToast(`Ошибка запуска: ${startRes.error || "Сбой"}`, "error");
            }
        } else {
            showToast(`Ошибка сохранения: ${saveRes.error}`, "error");
        }
    } catch (e) {
        showToast("Ошибка связи с ядром", "error");
    }
}

async function saveGeneratedPresetToLibrary() {
    if (!lastGeneratedPreset) {
        await updateGeneratedPreview();
    }
    if (!lastGeneratedPreset) return;

    showToast("Сохранение пресета в библиотеку...");
    try {
        const customId = "custom_" + Date.now().toString(36);
        const res = await sendIpc("save_preset", {
            id: customId,
            name: lastGeneratedPreset.name || "Пользовательский пресет",
            description: lastGeneratedPreset.description || "Индивидуальная адаптивная стратегия",
            args: lastGeneratedPreset.args
        });

        if (res.success) {
            showToast("Пресет успешно добавлен в библиотеку пресетов!");
            if (res.status) {
                updateUIWithState(res.status);
            }
            switchTab("presets");
            const allBtn = document.querySelector('.filter-pill[data-filter="all"]');
            if (allBtn) {
                document.querySelectorAll(".filter-pill").forEach(p => p.classList.remove("active"));
                allBtn.classList.add("active");
                if (res.status && res.status.presets) {
                    renderPresets(res.status.presets, res.status.currentPreset);
                }
            }
        } else {
            showToast(`Ошибка сохранения: ${res.error}`, "error");
        }
    } catch (e) {
        showToast("Ошибка сохранения пресета", "error");
    }
}

// ----------------------------------------------------
// SMART DNS (AI / NEURAL NETWORKS & GEOHIDE BYPASS)
// ----------------------------------------------------
const DNS_PROFILE_LABELS = {
    "comss_ai": "Comss AI (Нидерланды)",
    "geohide": "Geohide EU (Скрытие геолокации)",
    "xbox_electro": "Xbox-DNS (111.88.96.54 / 111.88.96.55)",
    "xbox_dns": "Xbox-DNS (111.88.96.54 / 111.88.96.55)",
    "shecan": "Shecan (Обход санкций)",
    "malun": "Malun Smart DNS (Анонимизация)",
    "cloudflare": "Cloudflare Fast (1.1.1.1)",
    "google": "Google Public (8.8.8.8)",
    "quad9": "Quad9 Secure (9.9.9.9)"
};

function updateSmartDnsUI(enabled, activeProfile = "comss_ai") {
    const dashTag = document.getElementById("dashAiStatusTag");
    const dashVal = document.getElementById("dashAiStatusVal");
    const dashBtn = document.getElementById("btnDashToggleAi");
    const selectDns = document.getElementById("selectDnsProfile");

    if (activeProfile && selectDns) {
        selectDns.value = activeProfile;
    }

    const profLabel = DNS_PROFILE_LABELS[activeProfile] || "Smart DNS";

    if (dashTag) {
        dashTag.innerText = enabled ? "АКТИВНО" : "ОТКЛЮЧЕНО";
        dashTag.className = "stat-tag " + (enabled ? "emerald-tag" : "");
    }
    if (dashVal) {
        dashVal.innerText = enabled ? profLabel : "Прямое подключение";
    }
    if (dashBtn) {
        dashBtn.innerText = enabled ? "Отключить подмену DNS" : "Включить подмену DNS";
        dashBtn.setAttribute("data-active", enabled ? "true" : "false");
    }

    const badge = document.getElementById("badgeSmartDnsStatus");
    const btn = document.getElementById("btnToggleSmartDns");
    if (badge) {
        if (enabled) {
            badge.className = "status-pill-badge";
            badge.innerText = `Активно (${profLabel})`;
        } else {
            badge.className = "status-pill-badge warn";
            badge.innerText = "Отключено";
        }
    }
    if (btn) {
        btn.setAttribute("data-active", enabled ? "true" : "false");
        btn.innerText = enabled ? "Отключить" : "Включить";
    }
}

async function toggleSmartDns() {
    const btn = document.getElementById("btnDashToggleAi") || document.getElementById("btnToggleSmartDns");
    const isCurrentlyActive = (btn?.getAttribute("data-active") === "true");
    const nextState = !isCurrentlyActive;
    const profile = document.getElementById("selectDnsProfile")?.value || "comss_ai";

    const label = DNS_PROFILE_LABELS[profile] || profile;
    showToast(nextState ? `Активация DNS: ${label}...` : "Отключение подмены DNS...");
    try {
        const res = await sendIpc("set_smart_dns", { enabled: nextState, profile: profile });
        if (res.success) {
            updateSmartDnsUI(res.smart_dns_enabled, res.active_profile || profile);
            showToast(nextState ? `DNS активирован: ${label}` : "Исходные настройки DNS восстановлены");
        } else {
            showToast(`Ошибка переключения DNS: ${res.error || "Сбой"}`, "error");
        }
    } catch (e) {
        showToast("Ошибка связи с ядром", "error");
    }
}

function populateScanData(data) {
    if (!data) return;
    lastScanData = data;

    // 1. ISP & Geolocation
    const pub = data.public || {};
    const ispNameEl = document.getElementById("netscanIspName");
    const pubIpEl = document.getElementById("netscanPublicIp");
    const asnEl = document.getElementById("netscanAsn");
    const orgEl = document.getElementById("netscanOrg");
    const cityEl = document.getElementById("netscanGeoCity");
    const countryEl = document.getElementById("netscanGeoCountry");
    const tzEl = document.getElementById("netscanGeoTz");

    if (ispNameEl) ispNameEl.innerText = pub.isp || pub.org || "Не определен";
    if (pubIpEl) pubIpEl.innerText = pub.ip || "--.--.--.--";
    if (asnEl) asnEl.innerText = pub.as ? `ASN: ${pub.as}` : "ASN: --";
    if (orgEl) orgEl.innerText = pub.org || "--";
    if (cityEl) cityEl.innerText = (pub.city || "--") + (pub.region ? `, ${pub.region}` : "");
    if (countryEl) countryEl.innerText = (pub.country || "--") + (pub.countryCode ? ` [${pub.countryCode}]` : "");
    if (tzEl) tzEl.innerText = `Часовой пояс: ${pub.timezone || "UTC"}`;

    // 2. Primary Adapter Fields
    const ad = (data.adapters && data.adapters.length > 0) ? data.adapters[0] : null;
    if (ad) {
        const setTxt = (id, val) => {
            const el = document.getElementById(id);
            if (el) el.innerText = val || "-";
        };
        setTxt("adName", ad.name);
        setTxt("adSpeed", ad.speed);
        setTxt("adIp", ad.ipv4);
        setTxt("adGateway", ad.gateway);
        setTxt("adDns", (ad.dns && ad.dns.length > 0) ? ad.dns.join(", ") : "-");
        setTxt("adMtu", ad.mtu ? `${ad.mtu} байт` : "-");
        setTxt("adMac", ad.mac);
        setTxt("adDesc", ad.description);
    }

    // 3. DPI & Security Status
    const dpi = data.dpi_status || {};
    const badgeHijack = document.getElementById("badgeDnsHijack");
    const descHijack = document.getElementById("dpiHijackDesc");
    if (badgeHijack) {
        if (dpi.dns_hijack_detected) {
            badgeHijack.className = "status-pill-badge danger";
            badgeHijack.innerText = "Подмена (РКН)";
            if (descHijack) descHijack.innerText = "Обнаружена подмена IP-адресов провайдером (DNS Hijack)";
        } else {
            badgeHijack.className = "status-pill-badge";
            badgeHijack.innerText = "Чистый (Без подмен)";
            if (descHijack) descHijack.innerText = "DNS-ответы не фильтруются локальным провайдером";
        }
    }

    const badgeTimestamps = document.getElementById("badgeTcpTimestamps");
    if (badgeTimestamps) {
        if (dpi.tcp_timestamps) {
            badgeTimestamps.className = "status-pill-badge";
            badgeTimestamps.innerText = "Включены (OK)";
        } else {
            badgeTimestamps.className = "status-pill-badge warn";
            badgeTimestamps.innerText = "Отключены";
        }
    }

    const badgeWinDivert = document.getElementById("badgeWinDivertReady");
    if (badgeWinDivert) {
        if (dpi.windivert_installed) {
            badgeWinDivert.className = "status-pill-badge";
            badgeWinDivert.innerText = "Установлен (Sys64)";
        } else {
            badgeWinDivert.className = "status-pill-badge danger";
            badgeWinDivert.innerText = "Отсутствует";
        }
    }

    const badgeYt = document.getElementById("badgeYoutubeIp");
    if (badgeYt) {
        badgeYt.innerText = dpi.resolved_youtube_ip || "--";
    }

    if (dpi.smart_dns_active != null) {
        updateSmartDnsUI(dpi.smart_dns_active);
    }

    // 4. Latency Table
    renderClusterTable(currentClusterTab);
}

function initNetScan() {
    const btnScan = document.getElementById("btnStartFullNetScan");
    if (btnScan) {
        btnScan.addEventListener("click", runFullNetScan);
    }

    const btnDiag = document.getElementById("btnRunSysDiagnostics");
    if (btnDiag) {
        btnDiag.addEventListener("click", runDiagnostics);
    }

    const clusterTabs = document.querySelectorAll("#matrixClusterTabs .matrix-tab-btn");
    clusterTabs.forEach(btn => {
        btn.addEventListener("click", () => {
            clusterTabs.forEach(b => b.classList.remove("active"));
            btn.classList.add("active");
            currentClusterTab = btn.getAttribute("data-cluster");
            renderClusterTable(currentClusterTab);
        });
    });

    const btnSmartDns = document.getElementById("btnToggleSmartDns");
    if (btnSmartDns) {
        btnSmartDns.addEventListener("click", toggleSmartDns);
    }

    // Load persisted scan results immediately so data is never lost
    sendIpc("get_cached_network").then(res => {
        if (res.success && res.data) {
            populateScanData(res.data);
        }
    }).catch(() => {});

    // Check Smart DNS state
    sendIpc("get_smart_dns_status").then(res => {
        if (res.success) {
            updateSmartDnsUI(res.enabled, res.active_profile);
        }
    }).catch(() => {});
}

async function runFullNetScan() {
    const btn = document.getElementById("btnStartFullNetScan");
    const origHtml = btn ? btn.innerHTML : "";
    if (btn) {
        btn.disabled = true;
        btn.innerHTML = `<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 2a10 10 0 0 1 10 10"/></svg> Сканирование...`;
    }

    const tableBody = document.getElementById("netscanTableBody");
    if (tableBody) {
        tableBody.innerHTML = `<tr><td colspan="8" class="table-loading">Выполняется зондирование основного физического адаптера, замер пинга и задержки...</td></tr>`;
    }

    showToast("Запущено глубокое сканирование сети...");

    try {
        const res = await sendIpc("scan_network");
        if (res.success && res.data) {
            populateScanData(res.data);
            showToast("Глубокое сканирование сети завершено и сохранено!");
        } else {
            showToast("Ошибка получения данных сканирования", "error");
        }
    } catch (e) {
        showToast("Ошибка связи при сканировании", "error");
    } finally {
        if (btn) {
            btn.disabled = false;
            btn.innerHTML = origHtml;
        }
    }
}

function renderClusterTable(clusterKey) {
    const tableBody = document.getElementById("netscanTableBody");
    if (!tableBody) return;

    if (!lastScanData || !lastScanData.latency_matrix) {
        tableBody.innerHTML = `<tr><td colspan="8" class="table-loading">Нажмите «Просканировать сеть» для замера метрик</td></tr>`;
        return;
    }

    const items = lastScanData.latency_matrix[clusterKey] || [];
    if (items.length === 0) {
        tableBody.innerHTML = `<tr><td colspan="8" class="table-loading">Нет данных для выбранного кластера</td></tr>`;
        return;
    }

    tableBody.innerHTML = "";

    items.forEach(item => {
        let avgText = "-";
        let badgeClass = "latency-bad";
        let badgeText = "Недоступен";

        if (item.avg > 0) {
            avgText = `${item.avg} ms`;
            if (item.avg < 45) {
                badgeClass = "latency-good";
                badgeText = "Идеально";
            } else if (item.avg < 100) {
                badgeClass = "latency-good";
                badgeText = "Отлично";
            } else if (item.avg < 180) {
                badgeClass = "latency-fair";
                badgeText = "Норма";
            } else {
                badgeClass = "latency-bad";
                badgeText = "Высокий пинг";
            }
        }

        const tr = document.createElement("tr");
        tr.innerHTML = `
            <td><strong>${escapeHtml(item.name || "")}</strong></td>
            <td><code class="val-pill">${escapeHtml(item.host || "")}</code></td>
            <td>${item.min > 0 ? `${item.min} ms` : "-"}</td>
            <td><strong style="color: var(--gold-light); font-size: 13px;">${avgText}</strong></td>
            <td>${item.max > 0 ? `${item.max} ms` : "-"}</td>
            <td>${item.jitter != null ? `${item.jitter} ms` : "-"}</td>
            <td><span style="color: ${item.loss > 0 ? '#f87171' : 'var(--text-secondary)'}">${item.loss != null ? `${item.loss}%` : "0%"}</span></td>
            <td><span class="latency-badge ${badgeClass}">${badgeText}</span></td>
        `;
        tableBody.appendChild(tr);
    });
}

// Launch app
window.addEventListener("DOMContentLoaded", initApp);
