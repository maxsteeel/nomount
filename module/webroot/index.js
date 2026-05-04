// ── KernelSU exec wrapper ──────────
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
      return; 
  }
  console.log(`[TOAST]: ${msg}`);
  // Fallback
  const el = document.getElementById('toast');
  if (el) {
      el.textContent = msg;
      el.classList.add('show');
      clearTimeout(el._t);
      el._t = setTimeout(() => el.classList.remove('show'), 2800);
  }
}

// ── Variables ──────────
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

// ── Helpers ──────────
function isValidUid(uid) { return /^\d+$/.test(String(uid)); }
function shEscape(str) { return "'" + String(str).replace(/'/g, "'\\''") + "'"; }

function isValidModId(modId) {
    const s = String(modId);
    if (s.includes('..')) return false;
    return /^[a-zA-Z0-9._-]+$/.test(s);
}

function escapeHTML(str) {
    if (typeof str !== 'string') return str;
    return str.replace(/[&<>"']/g, function(m) {
        switch (m) {
            case '&': return '&amp;';
            case '<': return '&lt;';
            case '>': return '&gt;';
            case '"': return '&quot;';
            case "'": return '&#39;';
            default: return m;
        }
    });
}

// ── Navegation ──────────
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

// ── HOME ──────────
async function loadHome() {
    const statsDisplay = document.getElementById('injection-stats');
    const kernelDisplay = document.getElementById('kernel-version');
    const deviceDisplay = document.getElementById('device-model');
    const androidDisplay = document.getElementById('android-ver');
    const versionDisplay = document.getElementById('nomount-version');
    const indicator = document.getElementById('status-indicator');
    const indicator_box = document.querySelector('.card.status-card-compact');
    const indicator_icon = document.querySelector('.status-icon-box');

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
            } else {
                setInactiveUI(indicator, indicator_box, indicator_icon);
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
            
            let jsonRaw = parts[6];
            if (!jsonRaw) jsonRaw = "[]";
            let activeModulesCount = 0;
            let dVer = "Unknown";

            try {
                const rules = JSON.parse(jsonRaw);
                const uniqueMods = new Set();
                rules.forEach(r => {
                    if (r.real.includes(MOD_DIR)) {
                        const rParts = r.real.split('/');
                        const modName = rParts[4];
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

                if (dVer && dVer !== "Unknown") {
                    indicator.textContent = "Active";
                    indicator.style.color = "var(--md-sys-color-primary)";
                } else {
                    setInactiveUI(indicator, indicator_box, indicator_icon);
                }

                localStorage.setItem('nm_home_cache', JSON.stringify({
                    kernelVer: kVer,
                    deviceModel: model,
                    androidInfo: androidInfo,
                    versionFull: versionFull,
                    active: dVer && dVer !== "Unknown"
                }));
            });
        } catch (e) {
            console.error("Delayed Home update error:", e);
        }
    })();
}

function setInactiveUI(indicator, box, icon) {
    indicator.textContent = "Inactive";
    indicator.style.color = "var(--md-sys-color-on-error)";
    if(box) box.style.backgroundColor = "var(--md-sys-color-error-container)";
    if(icon) {
        icon.style.backgroundColor = "var(--md-sys-color-error)";
        icon.innerHTML = `
            <svg xmlns="http://www.w3.org/2000/svg" height="32" viewBox="0 -960 960 960" width="32" fill="var(--md-sys-color-on-error)">
                <path d="M480-280q17 0 28.5-11.5T520-320q0-17-11.5-28.5T480-360q-17 0-28.5 11.5T440-320q0 17 11.5 28.5T480-280Zm-40-160h80v-240h-80v240Zm40 360q-83 0-156-31.5T197-197q-54-54-85.5-127T80-480q0-83 31.5-156T197-763q54-54 127-85.5T480-880q83 0 156 31.5T763-763q54 54 85.5 127T880-480q0 83-31.5 156T763-197q-54 54-127 85.5T480-80Z"/>
            </svg>`;
    }
}

// ── MODULES ──────────
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
            if(emptyBanner) emptyBanner.classList.add('active');
            return;
        }

        const entries = lines.map(line => {
            const [modId, realName, disableStr, skipStr] = line.split('|');
            const hasDisable = disableStr === 'true';
            const hasSkipMount = skipStr === 'true';

            const moduleRules = activeRules.filter(r => r && r.real && r.real.includes(`${MOD_DIR}/${modId}/`));
            const isLoaded = moduleRules.length > 0;

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
                fileCount: moduleRules.length,
            }];
        });

        const processEntries = () => {
            if (renderId !== currentRenderId) return;
            const chunk = entries.splice(0, 3);
            
            chunk.forEach(([modId, data]) => {
                const card = document.createElement('div');
                card.className = 'card module-card';
                card.dataset.moduleId = modId;
                card.innerHTML = `
                    <div class="module-header">
                        <div class="module-info">
                            <h3>${escapeHTML(data.realName)}</h3>
                            <p>${escapeHTML(modId)} • ${escapeHTML(data.status)}</p>
                        </div>
                        <md-switch id="switch-${escapeHTML(modId)}" ${!data.hasDisable ? 'selected' : ''}></md-switch>
                    </div>
                    <div class="module-divider"></div>
                    <div class="module-extension">
                        <div class="file-count">
                            <md-icon style="font-size:16px;">description</md-icon>
                            <span>${data.fileCount} files injected</span>
                        </div>
                        <button class="btn-hot-action ${data.isLoaded ? 'unload' : ''}" id="btn-hot-${escapeHTML(modId)}">
                            ${data.isLoaded ? 'Hot Unload' : 'Hot Load'}
                        </button>
                    </div>
                `;

                const toggle = card.querySelector('md-switch');
                toggle.addEventListener('change', async () => {
                    if (!isValidModId(modId)) return;
                    const targetState = toggle.selected;
                    toggle.disabled = true;
                    try {
                        if (targetState) {
                            await exec(`rm ${shEscape(`${MOD_DIR}/${modId}/disable`)}`);
                            await loadModule(modId);
                        } else {
                            await unloadModule(modId);
                            await exec(`touch ${shEscape(`${MOD_DIR}/${modId}/disable`)}`);
                        }
                    } finally {
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
                setTimeout(processEntries, 16);
            } else {
                emptyBanner?.classList.toggle('active', listContainer.children.length === 0);
            }
        };

        processEntries();

    } catch (e) {
        console.error("Error loading modules:", e);
        listContainer.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error loading modules: ${e.message}</div>`;
    }
}

async function loadModule(modId) {
    if (!isValidModId(modId)) return;
    const TARGET_PARTITIONS = ["system", "vendor", "product", "system_ext", "odm", "oem"];
    const partitionPaths = TARGET_PARTITIONS.map(p => shEscape(`${MOD_DIR}/${modId}/${p}`)).join(' ');
    const findScript = `find ${partitionPaths} -type f 2>/dev/null`;

    const res = await exec(findScript);
    const files = res.stdout.split('\n').filter(f => f.trim() !== '');

    if (files.length === 0) return;

    const batchScript = files.map(file => {
        const relativePath = file.replace(`${MOD_DIR}/${modId}/`, '');
        return `${NM_BIN} add ${shEscape("/" + relativePath)} ${shEscape(file)}`;
    }).join('\n');

    await exec(batchScript);
}

async function unloadModule(modId) {
    if (!isValidModId(modId)) return;
    try {
        const res = await exec(`${NM_BIN} list json`);
        const rules = JSON.parse(res.stdout || "[]");

        const modulePath = `${MOD_DIR}/${modId}/`;
        const targets = rules
            .filter(r => r && r.real && r.real.includes(modulePath))
            .map(r => r.virtual);

        if (targets.length === 0) return;

        const chunkSize = 100;
        for (let i = 0; i < targets.length; i += chunkSize) {
            const chunk = targets.slice(i, i + chunkSize);
            const batch = chunk.map(t => `${NM_BIN} del ${shEscape(t)}`).join('\n');
            await exec(batch);
        }
    } catch (e) {
        console.error("Error in unloadModule:", e);
        throw e;
    }
}

// ── APPS & EXCLUSIONS ──────────
let allAppsCache = [];
let showSystemApps = false;

async function ensureAppsCache() {
    if (allAppsCache.length > 0) return;

    const res3 = await exec('pm list packages -U -3');
    const resS = await exec('pm list packages -U -s');
    
    const parseOutput = (out, isSys) => {
        return out.split('\n').map(l => l.trim()).filter(l => l !== '').map(line => {
            const match = line.match(/package:(.+?)\s+uid:(\d+)/);
            if (match) {
                return {
                    packageName: match[1],
                    appLabel: match[1], // Fallback label since getting actual names via shell is slow
                    uid: match[2],
                    isSystem: isSys,
                    _searchLabel: match[1].toLowerCase(),
                    _searchPackage: match[1].toLowerCase()
                };
            }
            return null;
        }).filter(Boolean);
    };

    let apps = parseOutput(res3.stdout, false).concat(parseOutput(resS.stdout, true));
    apps.sort((a, b) => a.packageName.localeCompare(b.packageName));
    allAppsCache = apps;
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
                        <div style="display:flex; align-items:center; gap:16px;">
                            <img src="ksu://icon/${pkg}" style="width: 40px; height: 40px; border-radius: 10px;" 
                                onerror="this.src='data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg=='" />
                            <div class="setting-text">
                                <h3 style="margin:0; font-size:16px;">${escapeHTML(label)}</h3>
                                <p style="margin:0; opacity:0.7; font-size:14px;">${escapeHTML(pkg)}</p>
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
                    listContainer.innerHTML = '<div style="padding:20px; opacity:0.5; text-align:center;" class="empty-list-placeholder">No exclusions yet</div>';
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
    
    if(sysSwitch) sysSwitch.checked = showSystemApps;

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
        container.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error: ${e.message}</div>`;
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
        item.className = 'app-item card';
        item.style.display = 'flex';
        item.style.alignItems = 'center';
        item.style.padding = '12px';
        item.style.gap = '16px';
        item.style.cursor = 'pointer';
        
        const iconSrc = `ksu://icon/${app.packageName}`;
        const fallback = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==";

        item.innerHTML = `
            <img src="${iconSrc}" class="app-icon-img" style="width: 40px; height: 40px; border-radius: 10px;" loading="lazy" onerror="this.src='${fallback}'" /> 
            <div class="app-details" style="flex:1;">
                <div class="app-name" style="font-weight:bold; font-size:16px;">${escapeHTML(app.appLabel)}</div>
                <div class="app-pkg" style="font-size:12px; opacity:0.7;">${escapeHTML(app.packageName)}</div>
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
    if (!isValidUid(uid)) return showToast("Invalid UID");
    showToast(`Unblocking ${name}...`);
    try {
        const cat = await exec(`cat ${FILES.exclusions}`);
        const lines = cat.stdout.split('\n')
                                .map(l => l.trim())
                                .filter(l => l !== '' && l !== String(uid) && isValidUid(l));
        const newContent = lines.join('\n');
        await exec(`echo ${shEscape(newContent)} > ${FILES.exclusions}`);

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

// ── OPTIONS ──────────
async function loadOptions() {
    const swVerbose = document.getElementById('setting-verbose');
    const swSafe = document.getElementById('setting-safemode');
    const btnClear = document.getElementById('btn-clear-rules');

    const v = await exec(`[ -f ${FILES.verbose} ] && echo yes`);
    const s = await exec(`[ -f ${FILES.disable} ] && echo yes`);
    
    if(swVerbose) swVerbose.checked = v.stdout.includes('yes');
    if(swSafe) swSafe.checked = s.stdout.includes('yes');

    if(swVerbose) {
        swVerbose.onchange = (e) => {
            exec(e.target.checked ? `touch ${FILES.verbose}` : `rm ${FILES.verbose}`);
        };
    }

    if(swSafe) {
        swSafe.onchange = (e) => {
            exec(e.target.checked ? `touch ${FILES.disable}` : `rm ${FILES.disable}`);
        };
    }

    if(btnClear) {
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

// ── PULL TO REFRESH ──────────
let isGlobalLoading = false;
function initPullToRefresh() {
    const container = document.querySelector('.page-container');
    const indicator = document.querySelector('.pull-to-refresh-indicator');
    if(!container || !indicator) return;
    
    const indicatorIcon = indicator.querySelector('.icon');

    let startY = 0;
    let pullDistance = 0;
    const pullThreshold = 90; 

    const isRefreshableView = () => document.querySelector('.view-content.active') !== null;

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
            if(indicatorIcon) indicatorIcon.style.transform = `rotate(${rotation}deg)`;
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
        setTimeout(() => { if(indicatorIcon) indicatorIcon.style.transform = 'rotate(0deg)'; }, 300);
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

// ── INIT ──────────
document.addEventListener('DOMContentLoaded', () => {
    initNavigation();
    initPullToRefresh();
    
    const fab = document.getElementById('fab-add-exclusion');
    if(fab) fab.addEventListener('click', openAppSelector);

    const cache = localStorage.getItem('nm_home_cache');
    if (cache) {
        try {
            const data = JSON.parse(cache);
            document.getElementById('device-model').textContent = data.deviceModel;
            document.getElementById('kernel-version').textContent = data.kernelVer;
            document.getElementById('android-ver').textContent = data.androidInfo;
            document.getElementById('nomount-version').textContent = data.versionFull;
            
            const indicator = document.getElementById('status-indicator');
            if (data.active) {
                indicator.textContent = "Active";
                indicator.style.color = "var(--md-sys-color-primary)";
            } else {
                setInactiveUI(indicator, 
                    document.querySelector('.card.status-card-compact'), 
                    document.querySelector('.status-icon-box')
                );
            }
        } catch (e) {
            console.error("Error parsing Home cache", e);
        }
    }

    viewLoadState['view-home'] = true;
    loadHome(); 

    // Pre-cache
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
