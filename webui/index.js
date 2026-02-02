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
    const script = `

        uname -r
        echo "|||"

        ${NM_BIN} v
        echo "|||"

        grep "version=" ${MOD_DIR}/nomount/module.prop | head -n1 | cut -d= -f2
        echo "|||"

        val=$(getprop ro.product.vendor.model)
        [ -z "$val" ] && val=$(getprop ro.product.model)
        echo "$val"
        echo "|||"

        getprop ro.build.version.release
        echo "|||"

        getprop ro.build.version.sdk
        echo "|||"

        active_rules=$(${NM_BIN} list)
        
        count=0
        cd ${MOD_DIR}
        for mod in *; do
            if [ ! -d "$mod" ] || [ "$mod" = "nomount" ]; then continue; fi
            if echo "$active_rules" | grep -qF "${MOD_DIR}/$mod/"; then
                count=$((count + 1))
            fi
        done
        echo "$count"
    `;

    try {
        const result = await exec(script);

        const parts = result.stdout.split('|||').map(s => s.trim());

        if (parts.length < 7) throw new Error("Incomplete system data");

        const [
            kernelVer, 
            driverVer, 
            modVer, 
            deviceModel, 
            androidVer, 
            apiLvl,
            activeModulesCount
        ] = parts;

        document.getElementById('kernel-version').textContent = kernelVer || "Unknown";
        
        const driverText = driverVer || "Unknown";
        const modText = modVer || "v0.0.0";
        const indicator = document.getElementById('status-indicator');
        const versionDisplay = document.getElementById('nomount-version');

        versionDisplay.textContent = `${modText} (${driverText})`;

        if (driverText !== "Unknown") {
            indicator.textContent = "Active";
            indicator.style.color = "var(--md-sys-color-primary)";
        } else {
            indicator.textContent = "Inactive";
            indicator.style.color = "var(--md-sys-color-error)";
        }

        document.getElementById('device-model').textContent = deviceModel || "Unknown Device";
        document.getElementById('android-ver').textContent = `Android ${androidVer} (API ${apiLvl})`;

        document.getElementById('injection-stats').textContent = `${activeModulesCount} modules injecting`;

    } catch (e) {
        console.error("Error loading Home:", e);
    }
}

async function loadModules() {
    const listContainer = document.getElementById('modules-list');
    const emptyBanner = document.getElementById('modules-empty');
    
    const script = `
        active_rules=$(${NM_BIN} list)
        
        cd ${MOD_DIR}
        for mod in *; do
            if [ ! -d "$mod" ] || [ "$mod" = "nomount" ]; then continue; fi
            if [ -d "$mod/system" ] || [ -d "$mod/vendor" ] || \
               [ -d "$mod/product" ] || [ -d "$mod/system_ext" ] || \
               [ -d "$mod/oem" ] || [ -d "$mod/odm" ]; then
               
               name=$(grep "^name=" "$mod/module.prop" | head -n1 | cut -d= -f2-)
               if [ -f "$mod/disable" ]; then enabled="false"; else enabled="true"; fi

               file_list=$(find "$mod/system" "$mod/vendor" "$mod/product" "$mod/system_ext" -type f 2>/dev/null)
               potential_count=$(echo "$file_list" | wc -l)

               if echo "$active_rules" | grep -qF "${MOD_DIR}/$mod/"; then
                   is_loaded="true"
                   count=$potential_count
               else
                   is_loaded="false"
                   count=0
               fi
               
               echo "$mod|$name|$enabled|$count|$is_loaded"
            fi
        done
    `;

    try {
        const result = await exec(script);
        const lines = result.stdout.split('\n').filter(line => line.trim() !== '');
        
        const newModuleData = new Map(lines.map(line => {
            const [modId, realName, enabledStr, fileCount, loadedStr] = line.split('|');
            return [modId, {
                realName: (realName || modId).trim(),
                isEnabled: enabledStr.trim() === 'true',
                isLoaded: loadedStr.trim() === 'true',
                fileCount: parseInt(fileCount) || 0,
            }];
        }));

        const existingModuleIds = new Set(Array.from(listContainer.querySelectorAll('.module-card')).map(card => card.dataset.moduleId));

        // Remove modules that are no longer present
        for (const card of listContainer.querySelectorAll('.module-card')) {
            const modId = card.dataset.moduleId;
            if (!newModuleData.has(modId)) {
                card.style.animation = 'fadeOut 0.3s ease-out forwards';
                setTimeout(() => card.remove(), 300);
            }
        }

        const fragment = document.createDocumentFragment();
        
        // Add or update modules
        for (const [modId, data] of newModuleData.entries()) {
            const existingCard = listContainer.querySelector(`[data-module-id="${modId}"]`);

            if (existingCard) {
                // Update existing card
                const toggle = existingCard.querySelector(`#switch-${modId}`);
                if (toggle.selected !== data.isEnabled) toggle.selected = data.isEnabled;

                const fileCountEl = existingCard.querySelector('.file-count span');
                const newFileCountText = `${data.fileCount} file${data.fileCount !== 1 ? 's' : ''} injected`;
                if (fileCountEl.textContent !== newFileCountText) fileCountEl.textContent = newFileCountText;
                
                const hotBtn = existingCard.querySelector(`#btn-hot-${modId}`);
                const newHotBtnText = data.isLoaded ? 'Hot Unload' : 'Hot Load';
                if (hotBtn.textContent !== newHotBtnText) hotBtn.textContent = newHotBtnText;
                
                if (data.isLoaded) hotBtn.classList.add('unload');
                else hotBtn.classList.remove('unload');

            } else {
                // Create new card
                const card = document.createElement('div');
                card.className = 'card module-card';
                card.dataset.moduleId = modId;
                card.style.animation = 'fadeIn 0.3s ease-in-out';
                
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
                            <span>${data.fileCount} file${data.fileCount !== 1 ? 's' : ''} injected</span>
                        </div>
                        <button class="btn-hot-action ${data.isLoaded ? 'unload' : ''}" id="btn-hot-${modId}">
                            ${data.isLoaded ? 'Hot Unload' : 'Hot Load'}
                        </button>
                    </div>
                `;

                const toggle = card.querySelector(`#switch-${modId}`);
                toggle.addEventListener('change', async () => {
                    toggle.disabled = true;
                    try {
                        if (toggle.selected) {
                            await exec(`rm ${MOD_DIR}/${modId}/disable`);
                            await loadModule(modId);
                            showToast(`${data.realName} Enabled & Loaded`);
                        } else {
                            await unloadModule(modId);
                            await exec(`touch ${MOD_DIR}/${modId}/disable`);
                            showToast(`${data.realName} Unloaded & Disabled`);
                        }
                    } catch (e) {
                        showToast(`Error: ${e.message}`);
                    } finally {
                        loadModules();
                    }
                });

                const hotBtn = card.querySelector(`#btn-hot-${modId}`);
                hotBtn.addEventListener('click', () => { // No 'async' to make it non-blocking
                    hotBtn.textContent = "...";
                    hotBtn.disabled = true;

                    const performAction = async () => {
                        try {
                            // Check the live status instead of relying on potentially stale data
                            const active_rules = (await exec(`${NM_BIN} list`)).stdout;
                            const isCurrentlyLoaded = active_rules.includes(`${MOD_DIR}/${modId}/`);
                            
                            if (isCurrentlyLoaded) {
                                await unloadModule(modId);
                                showToast(`${data.realName} Unloaded`);
                            } else {
                                await loadModule(modId);
                                showToast(`${data.realName} Loaded`);
                            }
                        } catch (e) {
                            showToast(`Action failed: ${e.message}`);
                        } finally {
                            loadModules(); // Refresh view to ensure consistency
                        }
                    };

                    performAction(); // Fire-and-forget the promise
                });

                fragment.appendChild(card);
            }
        }
        
        if (fragment.children.length > 0) {
            emptyBanner.classList.remove('active');
            listContainer.appendChild(fragment);
        }

        if (newModuleData.size === 0) {
            emptyBanner.classList.add('active');
        } else {
            emptyBanner.classList.remove('active');
        }

    } catch (e) {
        console.error("Error loading modules:", e);
        listContainer.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error loading modules: ${e.message}</div>`;
    }                                     
}

async function loadModule(modId) {
    const script = `
        cd ${MOD_DIR}/${modId}
        for part in system vendor product system_ext oem odm;
 do
            if [ -d "$part" ]; then
                find "$part" -type f | while read -r file; do
                    target="/$file"
                    source="${MOD_DIR}/${modId}/$file"
                    ${NM_BIN} add "$target" "$source"
                done
            fi
        done
    `;
    await exec(script);
}

async function unloadModule(modId) {
    const script = `
        active_rules=$(${NM_BIN} list)
        echo "$active_rules" | while read -r rule; do
            if [ -z "$rule" ]; then continue; fi
            real_path=\\\${rule%%->*}
            virtual_path=\\\${rule#*->}
            if echo "$real_path" | grep -qF "${MOD_DIR}/${modId}/"; then
                ${NM_BIN} del "$virtual_path"
            fi
        done
    `;
    await exec(script);
}

let allAppsCache = [];
let showSystemApps = false;

// Virtualized App List State
let currentlyDisplayedApps = [];
let appListRenderIndex = 0;
const APP_RENDER_BATCH_SIZE = 50;
let listObserver = null;

async function loadExclusions() {
    const listContainer = document.getElementById('exclusions-list');
    
    try {
        const cat = await exec(`cat ${FILES.exclusions}`);
        const blockedUids = new Set(cat.stdout.split('\n').filter(u => u.trim() !== ''));

        if (allAppsCache.length === 0 && blockedUids.size > 0) {
            listContainer.innerHTML = '<div style="padding:20px; text-align:center; opacity:0.6;">Loading apps...</div>';
            try {
                // Check cache again in case pre-cache just finished
                if (allAppsCache.length === 0) {
                    const packages = await listPackages();
                    allAppsCache = await getPackagesInfo(packages);
                }
            } catch (e) {
                console.warn("Error getting app info:", e);
                listContainer.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error loading app info: ${e.message}</div>`;
                return;
            }
        }

        const existingUids = new Set(Array.from(listContainer.children).map(child => child.dataset.uid));
        
        // Remove old entries
        for (const child of listContainer.children) {
            const uid = child.dataset.uid;
            if (uid && !blockedUids.has(uid)) {
                child.style.animation = 'fadeOut 0.3s ease-out forwards';
                setTimeout(() => child.remove(), 300);
            }
        }

        // Add new entries
        const fragment = document.createDocumentFragment();
        for (const uid of blockedUids) {
            if (!existingUids.has(uid)) {
                const appInfo = allAppsCache.find(a => a.uid == uid);
                const label = appInfo ? (appInfo.appLabel || appInfo.packageName) : `Unknown (UID: ${uid})`;
                const pkg = appInfo ? appInfo.packageName : 'App not installed or found';
                const iconSrc = appInfo ? `ksu://icon/${appInfo.packageName}` : '';
                const fallbackIcon = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==";

                const item = document.createElement('div');
                item.className = 'card setting-item';
                item.dataset.uid = uid;
                item.style.animation = 'fadeIn 0.3s ease-in-out';
                item.innerHTML = `
                    <div style="display:flex; align-items:center; gap:16px; overflow: hidden;">
                        <img src="${iconSrc}" class="app-icon-img" style="width: 40px; height: 40px; border-radius: 10px;" loading="lazy" onerror="this.src='${fallbackIcon}'" />
                        
                        <div class="setting-text" style="overflow: hidden;">
                            <h3 style="white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">${label}</h3>
                            <p style="font-size: 12px; opacity: 0.7; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">${pkg}</p>
                            <p style="font-size: 10px; color: var(--md-sys-color-error); margin-top: 2px;">Blocked</p>
                        </div>
                    </div>

                    <md-icon-button class="btn-delete">
                        <md-icon>delete</md-icon>
                    </md-icon-button>
                `;
                
                item.querySelector('.btn-delete').addEventListener('click', async () => {
                    item.style.opacity = '0.5';
                    item.style.pointerEvents = 'none';
                    
                    await exec(`sed -i "/${uid}/d" ${FILES.exclusions}`);
                    await exec(`${NM_BIN} unblock ${uid}`);
                    loadExclusions();
                });

                fragment.appendChild(item);
            }
        }
        if (fragment.children.length > 0) {
            // If the placeholder was there, remove it
            const placeholder = listContainer.querySelector('.empty-list-placeholder, div[style*="text-align:center"]');
            if(placeholder) placeholder.remove();

            listContainer.appendChild(fragment);
        }
        
        // Handle empty case
        if (blockedUids.size === 0) {
            listContainer.innerHTML = '<div class="empty-list-placeholder" style="opacity:0.5; text-align:center; padding:20px;">No exclusions yet</div>';
        }

    } catch (e) {
        console.error("Error loading exclusions:", e);
        listContainer.innerHTML = `<div style="padding:20px; color:var(--md-sys-color-error);">Error: ${e.message}</div>`;
    }
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
    const term = searchTerm.toLowerCase();
    
    currentlyDisplayedApps = allAppsCache.filter(app => {
        const matchesSearch = (app.appLabel || "").toLowerCase().includes(term) || 
                              (app.packageName || "").toLowerCase().includes(term);

        const matchesType = showSystemApps ? true : !app.isSystem;
        return matchesSearch && matchesType;
    });

    const container = document.getElementById('app-list-container');
    container.innerHTML = '';
    appListRenderIndex = 0;
    if (listObserver) listObserver.disconnect();

    if (currentlyDisplayedApps.length === 0) {
        container.innerHTML = '<div style="padding:20px; text-align:center; opacity:0.6;">No apps found</div>';
        return;
    }
    
    renderNextAppBatch();
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

async function addExclusion(uid, name) {
    const current = await exec(`grep "^${uid}$" ${FILES.exclusions}`);
    if (current.stdout.trim().length > 0) {
        showToast(`${name} is already blocked!`);
        return;
    }
    await exec(`echo "${uid}" >> ${FILES.exclusions}`);
    await exec(`${NM_BIN} block ${uid}`);
    showToast(`Blocked: ${name}`);
    loadExclusions();
}

async function loadOptions() {
    const swVerbose = document.getElementById('setting-verbose');
    const swSafe = document.getElementById('setting-safemode');
    const btnClear = document.getElementById('btn-clear-rules');

    const checkVerbose = await exec(`[ -f ${FILES.verbose} ] && echo yes`);
    swVerbose.selected = checkVerbose.stdout.includes('yes');

    const checkSafe = await exec(`[ -f ${FILES.disable} ] && echo yes`);
    swSafe.selected = checkSafe.stdout.includes('yes');

    swVerbose.onchange = async () => {
        if (swVerbose.selected) await exec(`touch ${FILES.verbose}`);
        else await exec(`rm ${FILES.verbose}`);
    };

    swSafe.onchange = async () => {
        if (swSafe.selected) await exec(`touch ${FILES.disable}`);
        else await exec(`rm ${FILES.disable}`);
    };

    btnClear.onclick = async () => {
        await exec(`${NM_BIN} clear`);
        showToast("All rules cleared!");
    };
}

document.addEventListener('DOMContentLoaded', () => {
    initNavigation();
    document.getElementById('fab-add-exclusion').addEventListener('click', openAppSelector);

    // Pre-cache apps in the background for faster loading later
    (async () => {
        try {
            if (!allAppsCache || allAppsCache.length === 0) {
                const packages = await listPackages();
                allAppsCache = await getPackagesInfo(packages);
                allAppsCache.sort((a, b) => (a.appLabel || a.packageName).localeCompare(b.appLabel || b.packageName));
                console.log(`Pre-cached ${allAppsCache.length} apps.`);
            }
        } catch (e) {
            console.error("Failed to pre-cache apps:", e);
            // Don't bother the user with a toast for a background task
        }
        loadModules();
        loadExclusions();
    })();

    // Load initial view
    viewLoadState['view-home'] = true;
    loadHome();
});
