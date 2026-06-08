// KernelSU exec wrapper
let _cbId = 0;
function exec(cmd) {
    return new Promise((resolve) => {
        const key = `_ksu_cb_${Date.now()}_${_cbId++}`;
        window[key] = (errno, stdout, stderr) => {
            delete window[key];
            resolve({ errno, stdout: stdout || '', stderr: stderr || '' });
        };
        if (typeof ksu !== 'undefined' && ksu.exec) {
            ksu.exec(cmd, '{}', key);
        } else {
            resolve({ errno: 1, stdout: '', stderr: 'ksu not defined' });
        }
    });
}

function showToast(msg) {
    if (typeof ksu !== 'undefined' && ksu.toast) {
        ksu.toast(msg);
    }
}

// Variables
const ADB_DIR = "/data/adb";
const MOD_DIR = `${ADB_DIR}/modules`;
const NM_DATA = `${ADB_DIR}/nomount`;
const NM_BIN = `${MOD_DIR}/nomount/bin/nm`;
const FILES = {
    verbose: `${NM_DATA}/.verbose`,
    disable: `${NM_DATA}/disable`,
    exclusions: `${NM_DATA}/.exclusion_list`,
};

const viewLoadState = {
    'view-home': false,
    'view-modules': false,
    'view-exclusions': false,
    'view-options': false,
};

// Helpers
function isValidUid(uid) { return /^\d+$/.test(String(uid)); }

function isValidModId(modId) {
    const s = String(modId);
    if (s.includes('..')) return false;
    return /^[a-zA-Z0-9._-]+$/.test(s);
}

function escapeHtml(value) {
    return String(value ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

function syncSystemBarTheme() {
    const meta = document.querySelector('meta[name="theme-color"]');
    if (!meta) return;

    const styles = getComputedStyle(document.documentElement);
    const surface = styles.getPropertyValue('--md-sys-color-background').trim()
        || styles.getPropertyValue('--md-sys-color-surface').trim();

    if (surface) meta.setAttribute('content', surface);
}

function getHomeElements() {
    return {
        stats: document.getElementById('injection-stats'),
        kernel: document.getElementById('kernel-version'),
        device: document.getElementById('device-model'),
        android: document.getElementById('android-ver'),
        statusTitle: document.getElementById('status-title'),
        statusLabel: document.getElementById('status-indicator'),
        statusCard: document.querySelector('.home-status-card'),
        statusIcon: document.getElementById('status-icon'),
    };
}

function applyHomeData(data, statsText) {
    const elements = getHomeElements();
    if (elements.kernel) elements.kernel.textContent = data.kernelVer || "Unknown";
    if (elements.device) elements.device.textContent = data.deviceModel || "Unknown";
    if (elements.android) elements.android.textContent = data.androidInfo || "Unknown";
    if (elements.statusLabel) elements.statusLabel.textContent = data.versionFull || "Unknown";
    if (statsText && elements.stats) elements.stats.textContent = statsText;

    if (data.active) {
        setActiveUI(elements.statusTitle, elements.statusLabel, elements.statusCard, elements.statusIcon);
    } else {
        setInactiveUI(elements.statusTitle, elements.statusLabel, elements.statusCard, elements.statusIcon);
    }
}

const systemThemeQuery = window.matchMedia?.('(prefers-color-scheme: dark)');
if (systemThemeQuery?.addEventListener) {
    systemThemeQuery.addEventListener('change', () => requestAnimationFrame(syncSystemBarTheme));
} else {
    systemThemeQuery?.addListener?.(() => requestAnimationFrame(syncSystemBarTheme));
}

// Navigation
function initNavigation() {
    const navItems = document.querySelectorAll('.nav-item');
    const views = document.querySelectorAll('.view-content');
    const fabContainer = document.getElementById('fab-container');

    navItems.forEach(item => {
        item.addEventListener('click', () => {
            navItems.forEach(nav => nav.classList.remove('active'));
            item.classList.add('active');

            const targetId = item.dataset.target;
            views.forEach(view => {
                view.classList.remove('active');
            });
            document.getElementById(targetId).classList.add('active');

            if (targetId === 'view-exclusions') {
                fabContainer.classList.add('visible');
            } else {
                fabContainer.classList.remove('visible');
            }

            if (viewLoadState[targetId] === false) {
                viewLoadState[targetId] = true;
                switch (targetId) {
                    case 'view-home': loadHome(); break;
                    case 'view-modules': loadModules(); break;
                    case 'view-exclusions': loadExclusions(); break;
                    case 'view-options': loadOptions(); break;
                }
            }
        });
    });
}

// Home
async function loadHome() {
    const cache = localStorage.getItem('nm_home_cache');
    if (cache) {
        try {
            applyHomeData(JSON.parse(cache));
        } catch (e) {}
    }

    (async () => {
        const script = `
            uname -r; echo "|||"
            getprop ro.product.vendor.model; [ -z "$(getprop ro.product.vendor.model)" ] && getprop ro.product.model; echo "|||"
            getprop ro.build.version.release; echo "|||"
            getprop ro.build.version.sdk; echo "|||"
            grep "version=" ${MOD_DIR}/nomount/module.prop | cut -d= -f2; echo "|||"
            ${NM_BIN} v; echo "|||"
            ${NM_BIN} list json
        `;

        try {
            const result = await exec(script);
            const parts = result.stdout.split('|||').map(s => s.trim());
            
            let jsonRaw = parts[6];
            if (!jsonRaw) jsonRaw = "[]";
            let activeModulesCount = 0;
            let dVer = "Unknown";

            try {
                const rules = JSON.parse(jsonRaw);
                const modCounts = {};
                rules.forEach(r => {
                    if (r && r.real && r.real.startsWith(MOD_DIR)) {
                        const rParts = r.real.split('/');
                        const modName = rParts[4];
                        
                        if (modName && modName !== 'nomount') {
                            modCounts[modName] = (modCounts[modName] || 0) + 1;
                        }
                    }
                });
                
                activeModulesCount = Object.keys(modCounts).length;
                dVer = parts[5]; 
            } catch (e) {
                console.error("Error parsing rules in Home:", e);
            }

            const [kVer, model, aRel, aSdk, mVer] = parts;
            const androidInfo = `Android ${aRel} (API ${aSdk})`;
            const versionFull = `${mVer} (${dVer})`;
            const homeData = {
                kernelVer: kVer,
                deviceModel: model,
                androidInfo: androidInfo,
                versionFull: versionFull,
                active: dVer && dVer !== "Unknown"
            };

            requestAnimationFrame(() => {
                applyHomeData(homeData, `${activeModulesCount} modules injecting`);
                localStorage.setItem('nm_home_cache', JSON.stringify(homeData));
            });
        } catch (e) {
            console.error("Delayed Home update error:", e);
        }
    })();
}

function setActiveUI(title, label, box, icon) {
    if (title) title.textContent = "Active";
    label?.classList.add('active');
    label?.classList.remove('inactive');
    box?.classList.add('active');
    box?.classList.remove('inactive');
    icon?.classList.remove('inactive');
    icon?.replaceChildren(document.createTextNode('check_circle'));
}

function setInactiveUI(title, label, box, icon) {
    if (title) title.textContent = "Inactive";
    label?.classList.add('inactive');
    label?.classList.remove('active');
    box?.classList.remove('active');
    box?.classList.add('inactive');
    if (icon) {
        icon.classList.add('inactive');
        icon.replaceChildren(document.createTextNode('error'));
    }
}

// Modules
let currentRenderId = 0;
async function loadModules() {
    const listContainer = document.getElementById('modules-list');
    const emptyBanner = document.getElementById('modules-empty');
    if (!listContainer) return;

    const renderId = ++currentRenderId;

    try {
        const rulesRes = await exec(`${NM_BIN} list json`);
        const activeRules = JSON.parse(rulesRes.stdout || "[]");

        const script = `
            cd ${MOD_DIR}
            for mod in *; do
                [ ! -d "$mod" ] || [ "$mod" = "nomount" ] && continue
                [ ! -f "$mod/module.prop" ] && continue
                has_injectable=0
                for p in system vendor product system_ext oem odm my_* tran_*; do
                    if [ -d "$mod/$p" ]; then has_injectable=1; break; fi
                done
                [ $has_injectable -eq 0 ] && continue
                name=$(grep "^name=" "$mod/module.prop" | head -n1 | cut -d= -f2-)
                [ -f "$mod/disable" ] && disable="true" || disable="false"
                [ -f "$mod/skip_mount" ] && skip_mount="true" || skip_mount="false"
                echo "$mod|$name|$disable|$skip_mount"
            done
        `;

        const result = await exec(script);
        const lines = result.stdout.split('\n').filter(l => l.trim() !== '');

        listContainer.innerHTML = ''; 

        if (lines.length === 0) {
            if (emptyBanner) emptyBanner.classList.add('active');
            return;
        }

        const ruleCountByMod = {};
        activeRules.forEach(r => {
            if (!r || !r.real) return;
            const parts = r.real.split('/');
            if (parts.length > 4 && parts[3] === 'modules') {
                const modName = parts[4];
                if (modName && modName !== 'nomount') {
                    ruleCountByMod[modName] = (ruleCountByMod[modName] || 0) + 1;
                }
            }
        });

        const entries = lines.map(line => {
            const [modId, realName, disableStr, skipStr] = line.split('|');
            const hasDisable = disableStr === 'true';
            const hasSkipMount = skipStr === 'true';
            const fileCount = ruleCountByMod[modId] || 0;
            const isLoaded = fileCount > 0;

            let status = "Inactive";
            if (isLoaded) {
                status = hasDisable ? "Loaded" : "Active";
            } else {
                if (hasDisable) status = "Disabled";
                else if (hasSkipMount) status = "Skipped";
            }

            return [modId, {
                realName: (realName || modId).trim(),
                hasDisable,
                hasSkipMount,
                isLoaded,
                status,
                fileCount,
            }];
        });
        const processEntries = () => {
            if (renderId !== currentRenderId) return;
            const chunk = entries.splice(0, 3);

            requestAnimationFrame(() => {
                chunk.forEach(([modId, data]) => {
                    const card = document.createElement('div');
                    card.className = 'card module-card';
                    card.dataset.moduleId = modId;
                    const actionLabel = data.isLoaded ? 'hot unload' : 'hot load';
                    card.innerHTML = `
                        <div class="module-header">
                            <div class="module-info">
                                <h3>${escapeHtml(data.realName)}</h3>
                                <p>Status: ${escapeHtml(data.status)}</p>
                                <div class="file-count">
                                    <span>Injected: ${data.fileCount} files</span>
                                </div>
                            </div>
                            <md-switch id="switch-${modId}" aria-label="Toggle module" ${!data.hasDisable ? 'selected' : ''}></md-switch>
                        </div>
                        <div class="module-divider"></div>
                        <div class="module-extension">
                            <button class="btn-hot-action ${data.isLoaded ? 'unload' : ''}" id="btn-hot-${modId}">
                                <span>${actionLabel}</span>
                            </button>
                        </div>
                    `;

                    const toggle = card.querySelector('md-switch');
                    toggle.addEventListener('change', async () => {
                        if (!isValidModId(modId)) return;
                        if (toggle.dataset.busy === 'true') return;
                        const targetState = toggle.selected;
                        const motionDone = delay(320);
                        toggle.dataset.busy = 'true';
                        toggle.classList.add('switch-busy');
                        try {
                            if (targetState) {
                                await exec(`rm ${MOD_DIR}/${modId}/disable`);
                                await loadModule(modId);
                            } else {
                                await unloadModule(modId);
                                await exec(`touch ${MOD_DIR}/${modId}/disable`);
                            }
                        } finally {
                            await motionDone;
                            toggle.classList.remove('switch-busy');
                            delete toggle.dataset.busy;
                            loadModules();
                        }
                    });

                    const hotBtn = card.querySelector('.btn-hot-action');
                    hotBtn.addEventListener('click', async () => {
                        if (!isValidModId(modId)) return;
                        hotBtn.disabled = true;
                        try {
                            if (data.isLoaded) await unloadModule(modId);
                            else await loadModule(modId);
                        } finally {
                            loadModules();
                        }
                    });

                    listContainer.appendChild(card);
                });

                if (entries.length > 0) {
                    setTimeout(processEntries, 8);
                } else {
                    emptyBanner?.classList.toggle('active', listContainer.children.length === 0);
                }
            });
        };

        processEntries();

    } catch (e) {
        console.error("Error loading modules:", e);
        listContainer.innerHTML = `<div class="error-message">Error loading modules: ${e.message}</div>`;
    }
}

async function loadModule(modId) {
    if (!isValidModId(modId)) return;
    const TARGET_PARTITIONS = ["system", "vendor", "product", "system_ext", "odm", "oem"];
    const modPath = `${MOD_DIR}/${modId}`;
    const partitionsStr = TARGET_PARTITIONS.join(' ');
    const loadScript = `
        cd ${modPath} || exit 0
        find -L ${partitionsStr} \\( -type f -o -type l \\) -exec sh -c '
            mod="$1"; shift
            for f do
                printf "/%s\\0%s/%s\\0" "$f" "$mod" "$f"
            done
        ' _ ${modPath} {} + 2>/dev/null | xargs -0 -r -n 500 ${NM_BIN} add
    `;

    try {
        await exec(loadScript);
    } catch (e) {
        console.error(`Error loading module ${modId}:`, e);
        throw e;
    }
}

async function unloadModule(modId) {
    if (!isValidModId(modId)) return;
    try {
        const res = await exec(`${NM_BIN} list json`);
        const rules = JSON.parse(res.stdout || "[]");
        const modulePath = `${MOD_DIR}/${modId}/`;
        const targets = rules
            .filter(r => r && r.real && r.real.startsWith(modulePath))
            .map(r => r.virtual);

        if (targets.length === 0) return;
        const chunkSize = 500;
        for (let i = 0; i < targets.length; i += chunkSize) {
            const chunk = targets.slice(i, i + chunkSize);
            const escapedTargets = chunk.map(t => t).join(' ');
            const batchScript = `printf "%s\\0" ${escapedTargets} | xargs -0 -r -n 500 ${NM_BIN} del`;
            await exec(batchScript);
        }
    } catch (e) {
        console.error(`Error unloading module ${modId}:`, e);
        throw e;
    }
}

// Apps and exclusions
let allAppsCache = [];
let showSystemApps = false;
let appLoadingPromise = null;

async function ensureAppsCache() {
    if (allAppsCache.length > 0) return;
    if (appLoadingPromise) return appLoadingPromise;

    appLoadingPromise = (async () => {
        try {
            const pkgs = JSON.parse(ksu.listPackages("all"));
            const chunkSize = 200;
            let allInfo = [];
            
            for (let i = 0; i < pkgs.length; i += chunkSize) {
                const chunk = pkgs.slice(i, i + chunkSize);
                const infoRaw = ksu.getPackagesInfo(JSON.stringify(chunk));
                allInfo = allInfo.concat(JSON.parse(infoRaw));
                await new Promise(r => setTimeout(r, 5));
            }
            
            allAppsCache = allInfo.map(app => {
                const label = app.appLabel || app.packageName;
                return {
                    packageName: app.packageName,
                    appLabel: label,
                    uid: String(app.uid),
                    isSystem: app.isSystem,
                    _searchLabel: label.toLowerCase(),
                    _searchPackage: app.packageName.toLowerCase()
                };
            }).sort((a, b) => a.appLabel.localeCompare(b.appLabel));
            
        } catch (e) {
            console.error("Error loading applist:", e);
            throw e;
        } finally {
            appLoadingPromise = null;
        }
    })();

    return appLoadingPromise;
}

// Virtualized App List State
let currentlyDisplayedApps = [];
let appListRenderIndex = 0;
const APP_RENDER_BATCH_SIZE = 50;
let listObserver = null;
let filterTimeout;

async function loadExclusions() {
    const listContainer = document.getElementById('exclusions-list');

    (async () => {
        try {
            listContainer.innerHTML = '';
            const cat = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
            const blockedUids = new Set(cat.stdout.split('\n').filter(u => u.trim() !== ''));

            if (blockedUids.size > 0) {
                await ensureAppsCache();
            }

            const appsMap = new Map(allAppsCache.map(app => [String(app.uid), app]));
            const currentItems = Array.from(listContainer.querySelectorAll('.setting-item'));
            const existingUids = new Set(currentItems.map(i => i.dataset.uid));

            currentItems.forEach(item => {
                if (!blockedUids.has(item.dataset.uid)) item.remove();
            });

            const fragment = document.createDocumentFragment();
            blockedUids.forEach(uid => {
                if (!existingUids.has(uid)) {
                    const app = appsMap.get(uid);
                    const label = app ? (app.appLabel || app.packageName) : `UID: ${uid}`;
                    const pkg = app ? app.packageName : 'System/Unknown';
                    
                    const item = document.createElement('div');
                    item.className = 'card setting-item';
                    item.dataset.uid = uid;
                    
                    item.innerHTML = `
                        <div class="exclusion-app">
                            <img src="ksu://icon/${pkg}" class="app-icon-img"
                                onerror="this.src='data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg=='" />
                            <div class="setting-text">
                                <h3>${label}</h3>
                                <p>${pkg}</p>
                            </div>
                        </div>
                        <md-icon-button class="btn-delete"><md-icon>delete</md-icon></md-icon-button>
                    `;
                    
                    item.querySelector('.btn-delete').onclick = () => removeExclusion(uid, label);
                    fragment.appendChild(item);
                }
            });

            requestAnimationFrame(() => {
                const placeholder = listContainer.querySelector('.empty-list-placeholder');
                if (placeholder) placeholder.remove();
                listContainer.appendChild(fragment);
                
                if (blockedUids.size === 0) {
                    listContainer.innerHTML = `
                        <div class="empty-list-placeholder empty-state">
                            <div class="empty-face">(｡•̀ᴗ-)✧</div>
                            <div class="empty-text">No exclusions yet</div>
                        </div>
                    `;
                }
            });

        } catch (e) { console.error(e); }
    })();
}

async function openAppSelector() {
    const modal = document.getElementById('app-selector-modal');
    const container = document.getElementById('app-list-container');
    const searchInput = document.getElementById('app-search-input');
    const filterMenu = document.getElementById('filter-menu');
    const filterBtn = document.getElementById('btn-filter-toggle');
    const sysSwitch = document.getElementById('switch-system-apps');
    const closeModalBtn = document.getElementById('btn-close-modal');

    modal.classList.add('active');

    if (listObserver) listObserver.disconnect();
    filterMenu.classList.remove('active'); 
    searchInput.value = '';
    
    if (sysSwitch) sysSwitch.selected = showSystemApps;

    const closeModal = () => {
        modal.classList.remove('active');
        if (listObserver) listObserver.disconnect();
        closeModalBtn.removeEventListener('click', closeModal);
    };
    closeModalBtn.addEventListener('click', closeModal);

    container.innerHTML = '<div class="loading-spinner">Loading apps...</div>';
    
    listObserver = new IntersectionObserver((entries) => {
        if (entries[0].isIntersecting) {
            renderNextAppBatch();
        }
    }, { root: container, rootMargin: '200px' });

    try {
        await ensureAppsCache();
        filterAndRender();

        searchInput.oninput = (e) => {
            filterAndRender(e.target.value);
        };

        filterBtn.onclick = () => {
            filterMenu.classList.toggle('active');
        };

        sysSwitch.onchange = () => {
            showSystemApps = sysSwitch.selected;
            filterAndRender(searchInput.value);
        };

    } catch (e) {
        container.innerHTML = `<div class="error-message">Error: ${e.message}</div>`;
        console.error(e);
    }
}

function filterAndRender(searchTerm = '') {
    clearTimeout(filterTimeout);
    filterTimeout = setTimeout(() => {
        const term = searchTerm.toLowerCase();
        
        currentlyDisplayedApps = allAppsCache.filter(app => {
            return (app._searchLabel.includes(term) || 
                    app._searchPackage.includes(term)) &&
                   (showSystemApps ? true : !app.isSystem);
        });

        const container = document.getElementById('app-list-container');
        container.innerHTML = '';
        appListRenderIndex = 0;
        
        renderNextAppBatch();
    }, 10);
}

function renderNextAppBatch() {
    const container = document.getElementById('app-list-container');
    
    const batch = currentlyDisplayedApps.slice(
        appListRenderIndex, 
        appListRenderIndex + APP_RENDER_BATCH_SIZE
    );

    if (batch.length === 0) {
        if (listObserver) listObserver.disconnect();
        return;
    }

    const fragment = document.createDocumentFragment();

    batch.forEach(app => {
        const item = document.createElement('div');
        item.className = 'app-item segment-card';
        
        const iconSrc = `ksu://icon/${app.packageName}`;
        const fallback = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==";

        item.innerHTML = `
            <img src="${iconSrc}" class="app-icon-img" loading="lazy" onerror="this.src='${fallback}'" />
            <div class="app-details">
                <div class="app-name">${app.appLabel}</div>
                <div class="app-pkg">${app.packageName}</div>
            </div>
            <div class="app-meta">
                <div class="uid-label">UID: ${app.uid}</div>
                ${app.isSystem ? '<span class="system-chip">SYS</span>' : ''}
            </div>
        `;

        item.addEventListener('click', async () => {
            await addExclusion(app.uid, app.appLabel || app.packageName);
            document.getElementById('app-selector-modal').classList.remove('active');
        });

        fragment.appendChild(item);
    });
    
    container.appendChild(fragment);
    appListRenderIndex += batch.length;

    const lastElement = container.querySelector('.app-item:last-child');
    if (lastElement && listObserver) {
        listObserver.observe(lastElement);
    }
}

async function removeExclusion(uid, name) {
    if (!isValidUid(uid)) return showToast("Invalid UID");
    showToast(`Unblocking ${name}...`);
    try {
        const cat = await exec(`cat ${FILES.exclusions}`);
        const lines = cat.stdout.split('\n')
                                .map(l => l.trim())
                                .filter(l => l !== '' && l !== String(uid) && isValidUid(l));
        const newContent = lines.join('\n');
        await exec(`echo ${newContent} > ${FILES.exclusions}`);

        await exec(`${NM_BIN} unblock ${uid}`);
        await loadExclusions();
    } catch (e) { showToast("Error unblocking"); }
}

async function addExclusion(uid, name) {
    if (!isValidUid(uid)) return showToast("Invalid UID");
    try {
        const cat = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        if (cat.stdout.includes(String(uid))) return showToast("Already blocked");

        await exec(`echo "${uid}" >> ${FILES.exclusions}`);
        await exec(`${NM_BIN} block ${uid}`);
        showToast(`Blocked: ${name}`);
        await loadExclusions();
    } catch (e) { showToast("Error blocking"); }
}

// Options
async function loadOptions() {
    const swVerbose = document.getElementById('setting-verbose');
    const swSafe = document.getElementById('setting-safemode');
    const btnClear = document.getElementById('btn-clear-rules');
    const v = await exec(`[ -f ${FILES.verbose} ] && echo yes`);
    const s = await exec(`[ -f ${FILES.disable} ] && echo yes`);

    if (swVerbose) swVerbose.selected = v.stdout.includes('yes');
    if (swSafe) swSafe.selected = s.stdout.includes('yes');

    if (swVerbose) {
        swVerbose.addEventListener('change', (e) => {
            exec(e.target.selected ? `touch ${FILES.verbose}` : `rm ${FILES.verbose}`);
        });
    }

    if (swSafe) {
        swSafe.addEventListener('change', (e) => {
            exec(e.target.selected ? `touch ${FILES.disable}` : `rm ${FILES.disable}`);
        });
    }

    if (btnClear) {
        btnClear.onclick = () => {
            showToast("Clearing all rules...");
            (async () => {
                try {
                    await exec(`${NM_BIN} clear`);
                    showToast("All rules cleared!");
                    loadModules();
                    loadExclusions();
                } catch (e) {
                    showToast("Clear failed");
                }
            })();
        };
    }
}

// Pull to refresh
let isGlobalLoading = false;
function initPullToRefresh() {
    const container = document.querySelector('.page-container');
    const indicator = document.querySelector('.pull-to-refresh-indicator');
    if (!container || !indicator) return;
    
    const indicatorIcon = indicator.querySelector('.icon');

    let startY = 0;
    let pullDistance = 0;
    const pullThreshold = 90; 

    const isRefreshableView = () => {
        const activeView = document.querySelector('.view-content.active');
        return activeView && (activeView.id === 'view-modules' || activeView.id === 'view-exclusions');
    };


    container.addEventListener('touchstart', (e) => {
        if (isGlobalLoading || container.scrollTop !== 0 || !isRefreshableView()) {
            startY = 0; return;
        }
        startY = e.touches[0].pageY;
        indicator.style.transition = 'none';
    }, { passive: true });

    container.addEventListener('touchmove', (e) => {
        if (startY === 0 || isGlobalLoading) return;
        const currentY = e.touches[0].pageY;
        pullDistance = (currentY - startY) * 0.4;

        if (pullDistance > 0 && container.scrollTop === 0) {
            if (e.cancelable) e.preventDefault(); 
            const rotation = Math.min(180, (pullDistance / pullThreshold) * 180);
            const opacity = Math.min(1, pullDistance / pullThreshold);
            
            indicator.style.top = `${Math.min(pullDistance, pullThreshold) - 60}px`;
            indicator.style.opacity = opacity;
            if (indicatorIcon) indicatorIcon.style.transform = `rotate(${rotation}deg)`;
        }
    }, { passive: false });

    container.addEventListener('touchend', async () => {
        if (startY === 0 || isGlobalLoading) return;
        indicator.style.transition = 'all 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275)';

        if (pullDistance >= pullThreshold) {
            isGlobalLoading = true;
            indicator.classList.add('refreshing');
            indicator.style.top = '24px';
            indicator.style.opacity = '1';

            try {
                await refreshCurrentView();
                await new Promise(r => setTimeout(r, 400));
            } catch (e) {
                showToast("Refresh failed");
            } finally {
                resetIndicator();
            }
        } else {
            resetIndicator();
        }
        startY = 0; pullDistance = 0;
    });

    function resetIndicator() {
        isGlobalLoading = false;
        indicator.classList.remove('refreshing');
        indicator.style.top = '-60px';
        indicator.style.opacity = '0';
        setTimeout(() => { if (indicatorIcon) indicatorIcon.style.transform = 'rotate(0deg)'; }, 300);
    }
}

async function refreshCurrentView() {
    const activeView = document.querySelector('.view-content.active');
    if (!activeView) return;

    switch (activeView.id) {
        case 'view-home': await loadHome(); break;
        case 'view-modules': await loadModules(); break;
        case 'view-exclusions': await loadExclusions(); break;
        case 'view-options': await loadOptions(); break;
    }
}

// Init
document.addEventListener('DOMContentLoaded', () => {
    syncSystemBarTheme();
    initNavigation();
    initPullToRefresh();
    
    const fab = document.getElementById('fab-add-exclusion');
    if (fab) fab.addEventListener('click', openAppSelector);

    const cache = localStorage.getItem('nm_home_cache');
    if (cache) {
        try {
            applyHomeData(JSON.parse(cache));
        } catch (e) {
            console.error("Error parsing Home cache", e);
        }
    }

    viewLoadState['view-home'] = true;
    loadHome(); 

    (async () => {
        try {
            await ensureAppsCache();
            if (!viewLoadState['view-modules']) loadModules();
            if (!viewLoadState['view-exclusions']) loadExclusions();
        } catch (e) {
            console.error("Background pre-cache failed", e);
        }
    })();
});
