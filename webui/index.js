import { exec, toast, listPackages, getPackagesInfo } from 'kernelsu-alt';
import '@material/web/button/filled-button.js';
import '@material/web/button/outlined-button.js';
import '@material/web/iconbutton/outlined-icon-button.js';
import '@material/web/icon/icon.js';
import '@material/web/iconbutton/icon-button.js';
import '@material/web/switch/switch.js';
import '@material/web/fab/fab.js';
 
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

function showToast(msg) {
    try { toast(msg); } catch (e) { console.log(`[TOAST]: ${msg}`); }
}

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
                viewLoadState[targetId] = true; // Set true immediately to prevent re-triggering
                switch (targetId) {
                    case 'view-home':
                        loadHome();
                        break;
                    case 'view-modules':
                        loadModules();
                        break;
                    case 'view-exclusions':
                        loadExclusions();
                        break;
                    case 'view-options':
                        loadOptions();
                        break;
                }
            }
        });
    });
}

async function loadHome() {
    const statsDisplay = document.getElementById('injection-stats');
    const kernelDisplay = document.getElementById('kernel-version');
    const deviceDisplay = document.getElementById('device-model');
    const androidDisplay = document.getElementById('android-ver');
    const versionDisplay = document.getElementById('nomount-version');
    const indicator = document.getElementById('status-indicator');

    const cache = localStorage.getItem('nm_home_cache');
    if (cache) {
        try {
            const data = JSON.parse(cache);
            kernelDisplay.textContent = data.kernelVer;
            deviceDisplay.textContent = data.deviceModel;
            androidDisplay.textContent = data.androidInfo;
            versionDisplay.textContent = data.versionFull;
            if (data.active) {
                indicator.textContent = "Active";
                indicator.style.color = "var(--md-sys-color-primary)";
            }
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
            
            const jsonRaw = parts[6];
            let activeModulesCount = 0;
            let dVer = "Unknown";

            try {
                const rules = JSON.parse(jsonRaw);
                const uniqueMods = new Set();
                rules.forEach(r => {
                    if (r.real.includes(MOD_DIR)) {
                        const parts = r.real.split('/');
                        const modName = parts[4];
                        if (modName && modName !== 'nomount') uniqueMods.add(modName);
                    }
                });
                activeModulesCount = uniqueMods.size;
                dVer = parts[5]; 
            } catch (e) {
                console.error("Error parsing rules in Home:", e);
            }

            const [kVer, model, aRel, aSdk, mVer] = parts;
            const androidInfo = `Android ${aRel} (API ${aSdk})`;
            const versionFull = `${mVer} (${dVer})`;

            requestAnimationFrame(() => {
                kernelDisplay.textContent = kVer || "Unknown";
                deviceDisplay.textContent = model || "Unknown";
                androidDisplay.textContent = androidInfo;
                versionDisplay.textContent = versionFull;
                statsDisplay.textContent = `${activeModulesCount} modules injecting`;

                if (dVer !== "Unknown") {
                    indicator.textContent = "Active";
                    indicator.style.color = "var(--md-sys-color-primary)";
                }

                localStorage.setItem('nm_home_cache', JSON.stringify({
                    kernelVer: kVer,
                    deviceModel: model,
                    androidInfo: androidInfo,
                    versionFull: versionFull,
                    active: dVer !== "Unknown"
                }));
            });
        } catch (e) {
            console.error("Delayed Home update error:", e);
        }
    })();
}

let currentRenderId = 0;
async function loadModules() {
    const listContainer = document.getElementById('modules-list');
    const emptyBanner = document.getElementById('modules-empty');
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
                [ -f "$mod/disable" ] && enabled="false" || enabled="true"
                echo "$mod|$name|$enabled"
            done
        `;

        const result = await exec(script);
        const lines = result.stdout.split('\n').filter(l => l.trim() !== '');
        
        const newModuleData = new Map(lines.map(line => {
            const [modId, realName, en] = line.split('|');;
            const moduleRules = activeRules.filter(r => r && r.real && r.real.includes(`${MOD_DIR}/${modId}/`));
            return [modId, {
                realName: (realName || modId).trim(),
                isEnabled: en === 'true',
                isLoaded: moduleRules.length > 0,
                fileCount: moduleRules.length,
            }];
        }));

        const currentCards = Array.from(listContainer.querySelectorAll('.module-card'));
        const existingCardsMap = new Map(currentCards.map(c => [c.dataset.moduleId, c]));

        if (newModuleData.size === 0) {
            listContainer.innerHTML = `<div style="padding:20px; opacity:0.5;">No inyectable modules found.</div>`;
            return;
        }

        for (const [modId, card] of existingCardsMap) {
            if (!newModuleData.has(modId)) {
                card.remove();
                existingCardsMap.delete(modId);
            }
        }

        const entries = Array.from(newModuleData.entries());

        listContainer.innerHTML = '';
        const processEntries = () => {
            if (renderId !== currentRenderId) return;
            const chunk = entries.splice(0, 3);
            
            chunk.forEach(([modId, data]) => {
                let card = existingCardsMap.get(modId);

                if (card) {
                    const toggle = card.querySelector('md-switch');
                    if (toggle && !toggle.disabled) toggle.selected = data.isEnabled;

                    const fileCountEl = card.querySelector('.file-count span');
                    if (fileCountEl) fileCountEl.textContent = `${data.fileCount} file${data.fileCount !== 1 ? 's' : ''} injected`;
                    
                    const hotBtn = card.querySelector('.btn-hot-action');
                    if (hotBtn && !hotBtn.disabled) {
                        hotBtn.textContent = data.isLoaded ? 'Hot Unload' : 'Hot Load';
                        data.isLoaded ? hotBtn.classList.add('unload') : hotBtn.classList.remove('unload');
                    }
                } else {
                    card = document.createElement('div');
                    card.className = 'card module-card';
                    card.dataset.moduleId = modId;
                    card.innerHTML = `
                        <div class="module-header">
                            <div class="module-info">
                                <h3>${data.realName}</h3>
                                <p>${modId}</p>
                            </div>
                            <md-switch id="switch-${modId}" ${data.isEnabled ? 'selected' : ''}></md-switch>
                        </div>
                        <div class="module-divider"></div>
                        <div class="module-extension">
                            <div class="file-count">
                                <md-icon style="font-size:16px;">description</md-icon>
                                <span>${data.fileCount} files injected</span>
                            </div>
                            <button class="btn-hot-action ${data.isLoaded ? 'unload' : ''}" id="btn-hot-${modId}">
                                ${data.isLoaded ? 'Hot Unload' : 'Hot Load'}
                            </button>
                        </div>
                    `;

                    const toggle = card.querySelector('md-switch');
                    toggle.addEventListener('change', () => {
                        const targetState = toggle.selected;
                        toggle.disabled = true;

                        (async () => {
                            try {
                                if (targetState) {
                                    await exec(`rm ${MOD_DIR}/${modId}/disable`);
                                    await loadModule(modId);
                                    showToast(`${data.realName} Enabled`);
                                } else {
                                    await unloadModule(modId);
                                    await exec(`touch ${MOD_DIR}/${modId}/disable`);
                                    showToast(`${data.realName} Disabled`);
                                }
                            } catch (e) {
                                showToast(`Error: ${e.message}`);
                            } finally {
                                setTimeout(() => loadModules(), 10);
                            }
                        })();
                    });

                    const hotBtn = card.querySelector('.btn-hot-action');
                    hotBtn.addEventListener('click', async () => {
                        const originalText = hotBtn.textContent;
                        hotBtn.textContent = "...";
                        hotBtn.disabled = true;

                        try {
                            const rulesRes = await exec(`${NM_BIN} list json`);
                            const rules = JSON.parse(rulesRes.stdout);

                            const isLoaded = rules.some(r => r && r.real && r.real.includes(`${MOD_DIR}/${modId}/`));

                            if (isLoaded) {
                                await unloadModule(modId);
                                showToast(`${data.realName} Unloaded`);
                            } else {
                                await loadModule(modId);
                                showToast(`${data.realName} Loaded`);
                            }
                        } catch (e) {
                            showToast(`Action failed: ${e.message}`);
                            hotBtn.textContent = originalText;
                            hotBtn.disabled = false;
                        } finally {
                            await loadModules(); 
                        }
                    });

                    listContainer.appendChild(card);
                }
            });

            if (entries.length > 0) {
                setTimeout(processEntries, 16);
            } else {
                emptyBanner.classList.toggle('active', listContainer.children.length === 0);
            }
        };

        processEntries();

    } catch (e) {
        console.error("Error loading modules:", e);
        listContainer.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error loading modules: ${e.message}</div>`;
    }
}

async function loadModule(modId) {
    const findScript = `find ${MOD_DIR}/${modId} -type f`;
    const res = await exec(findScript);
    const files = res.stdout.split('\n').filter(f => f.trim() !== '');

    if (files.length === 0) return;

    const batchScript = files.map(file => {
        const relativePath = file.replace(`${MOD_DIR}/${modId}/`, '');
        return `${NM_BIN} add "/${relativePath}" "${file}"`;
    }).join('\n');

    await exec(batchScript);
}

async function unloadModule(modId) {
    try {
        const res = await exec(`${NM_BIN} list json`);
        const rules = JSON.parse(res.stdout);

        const modulePath = `${MOD_DIR}/${modId}/`;
        const targets = rules
            .filter(r => r && r.real && r.real.includes(modulePath))
            .map(r => r.virtual);

        if (targets.length === 0) return;

        const chunkSize = 100;
        for (let i = 0; i < targets.length; i += chunkSize) {
            const chunk = targets.slice(i, i + chunkSize);
            const batch = chunk.map(t => `${NM_BIN} del "${t}"`).join('\n');
            await exec(batch);
        }
    } catch (e) {
        console.error("Error in unloadModule:", e);
        throw e;
    }
}

let allAppsCache = [];
let showSystemApps = false;

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
            const cat = await exec(`cat ${FILES.exclusions}`);
            const blockedUids = new Set(cat.stdout.split('\n').filter(u => u.trim() !== ''));

            if (allAppsCache.length === 0 && blockedUids.size > 0) {
                const packages = await listPackages();
                allAppsCache = await getPackagesInfo(packages);
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
                        <div style="display:flex; align-items:center; gap:16px;">
                            <img src="ksu://icon/${pkg}" style="width: 40px; height: 40px; border-radius: 10px;" 
                                onerror="this.src='data:image/svg+xml;base64,...'" />
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
                if(placeholder) placeholder.remove();
                listContainer.appendChild(fragment);
                
                if (blockedUids.size === 0) {
                    listContainer.innerHTML = '<div style="padding:20px; opacity:0.5;" class="empty-list-placeholder">No exclusions yet</div>';
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

    // Cleanup previous state
    if (listObserver) listObserver.disconnect();
    filterMenu.classList.remove('active'); 
    searchInput.value = '';
    sysSwitch.selected = showSystemApps;

    const closeModal = () => {
        modal.classList.remove('active');
        if (listObserver) listObserver.disconnect();
        closeModalBtn.removeEventListener('click', closeModal);
    };
    closeModalBtn.addEventListener('click', closeModal);

    container.innerHTML = '<div class="loading-spinner" style="padding:20px; text-align:center;">Loading apps...</div>';
    
    listObserver = new IntersectionObserver((entries) => {
        if (entries[0].isIntersecting) {
            renderNextAppBatch();
        }
    }, { root: container, rootMargin: '200px' });

    try {
        if (!allAppsCache || allAppsCache.length === 0) {
            const packages = await listPackages();
            allAppsCache = await getPackagesInfo(packages);
            allAppsCache.sort((a, b) => (a.appLabel || a.packageName).localeCompare(b.appLabel || b.packageName));
        }

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
        container.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error: ${e.message}</div>`;
        console.error(e);
    }
}

function filterAndRender(searchTerm = '') {
    clearTimeout(filterTimeout);
    filterTimeout = setTimeout(() => {
        const term = searchTerm.toLowerCase();
        
        currentlyDisplayedApps = allAppsCache.filter(app => {
            return ((app.appLabel || "").toLowerCase().includes(term) || 
                   (app.packageName || "").toLowerCase().includes(term)) &&
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
        item.className = 'app-item';
        
        const iconSrc = `ksu://icon/${app.packageName}`;
        const fallback = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==";

        item.innerHTML = `
            <img src="${iconSrc}" class="app-icon-img" loading="lazy" onerror="this.src='${fallback}'" /> 
            <div class="app-details">
                <span class="app-name">${app.appLabel || app.packageName}</span>
                <span class="app-pkg">${app.packageName}</span>
            </div>
            <div style="text-align:right;">
                <div style="font-size: 12px; color: var(--md-sys-color-primary);">UID: ${app.uid}</div>
                ${app.isSystem ? '<span style="font-size:10px; background:#333; padding:2px 4px; border-radius:4px; opacity:0.7;">SYS</span>' : ''}
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
    showToast(`Unblocking ${name}...`);
    (async () => {
        try {
            const cat = await exec(`cat ${FILES.exclusions}`);
            const lines = cat.stdout.split('\n').map(l => l.trim()).filter(l => l !== '' && l !== String(uid));
            const newContent = lines.join('\n');
            await exec(`echo "${newContent}" > ${FILES.exclusions}`);

            await exec(`${NM_BIN} unblock ${uid}`);
            await loadExclusions();
        } catch (e) { showToast("Error unblocking"); }
    })();
}

async function addExclusion(uid, name) {
    (async () => {
        try {
            const cat = await exec(`cat ${FILES.exclusions}`);
            if (cat.stdout.includes(String(uid))) return showToast("Already blocked");

            await exec(`echo "${uid}" >> ${FILES.exclusions}`);
            await exec(`${NM_BIN} block ${uid}`);
            showToast(`Blocked: ${name}`);
            await loadExclusions();
        } catch (e) { showToast("Error blocking"); }
    })();
}

async function loadOptions() {
    const swVerbose = document.getElementById('setting-verbose');
    const swSafe = document.getElementById('setting-safemode');
    const btnClear = document.getElementById('btn-clear-rules');

    (async () => {
        const v = await exec(`[ -f ${FILES.verbose} ] && echo yes`);
        const s = await exec(`[ -f ${FILES.disable} ] && echo yes`);
        swVerbose.selected = v.stdout.includes('yes');
        swSafe.selected = s.stdout.includes('yes');
    })();

    swVerbose.onchange = () => {
        exec(swVerbose.selected ? `touch ${FILES.verbose}` : `rm ${FILES.verbose}`);
    };

    swSafe.onchange = () => {
        exec(swSafe.selected ? `touch ${FILES.disable}` : `rm ${FILES.disable}`);
    };

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

let isGlobalLoading = false;
function initPullToRefresh() {
    const container = document.querySelector('.page-container');
    const indicator = document.querySelector('.pull-to-refresh-indicator');
    const indicatorIcon = indicator.querySelector('md-icon');

    let startY = 0;
    let pullDistance = 0;
    const pullThreshold = 90; 

    const isRefreshableView = () => {
        const activeView = document.querySelector('.view-content.active');
        return activeView !== null; 
    };

    container.addEventListener('touchstart', (e) => {
        if (isGlobalLoading || container.scrollTop !== 0 || !isRefreshableView()) {
            startY = 0;
            return;
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
            indicatorIcon.style.transform = `rotate(${rotation}deg)`;
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

        startY = 0;
        pullDistance = 0;
    });

    function resetIndicator() {
        isGlobalLoading = false;
        indicator.classList.remove('refreshing');
        indicator.style.top = '-60px';
        indicator.style.opacity = '0';
        setTimeout(() => {
            indicatorIcon.style.transform = 'rotate(0deg)';
        }, 300);
    }
}

async function refreshCurrentView() {
    const activeView = document.querySelector('.view-content.active');
    if (!activeView) return;

    const viewId = activeView.id;
    console.log(`Refreshing view: ${viewId}`);

    // Bypass the 'viewLoadState' check for a forced refresh
    switch (viewId) {
        case 'view-home':
            await loadHome();
            break;
        case 'view-modules':
            await loadModules();
            break;
        case 'view-exclusions':
            await loadExclusions();
            break;
        case 'view-options':
            await loadOptions();
            break;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    initNavigation();
    initPullToRefresh();
    document.getElementById('fab-add-exclusion').addEventListener('click', openAppSelector);

    const cache = localStorage.getItem('nm_home_cache');
    if (cache) {
        try {
            const data = JSON.parse(cache);
            document.getElementById('device-model').textContent = data.deviceModel;
            document.getElementById('kernel-version').textContent = data.kernelVer;
            document.getElementById('android-ver').textContent = data.androidInfo;
            document.getElementById('nomount-version').textContent = data.versionFull;
            
            if (data.active) {
                const indicator = document.getElementById('status-indicator');
                indicator.textContent = "Active";
                indicator.style.color = "var(--md-sys-color-primary)";
            }
        } catch (e) {
            console.error("Error parsing Home cache", e);
        }
    }

    viewLoadState['view-home'] = true;
    setTimeout(() => {
        loadHome(); 
    }, 100);

    setTimeout(async () => {
        try {
            if (allAppsCache.length === 0) {
                const packages = await listPackages();
                allAppsCache = await getPackagesInfo(packages);
                allAppsCache.sort((a, b) => 
                    (a.appLabel || a.packageName).localeCompare(b.appLabel || b.packageName)
                );
            }
            if (!viewLoadState['view-modules']) loadModules();
            if (!viewLoadState['view-exclusions']) loadExclusions();
        } catch (e) {
            console.error("Background pre-cache failed", e);
        }
    }, 500);
});
