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
            try {
                ksu.exec(cmd, '{}', key);
            } catch (e) {
                delete window[key];
                resolve({ errno: 1, stdout: '', stderr: e?.message || 'ksu exec failed' });
            }
        } else {
            resolve({ errno: 1, stdout: '', stderr: 'ksu not defined' });
        }
    });
}

function showToast(msg) {
    if (typeof ksu !== 'undefined' && ksu.toast) {
        try {
            ksu.toast(msg);
        } catch (e) {
            console.warn("Toast failed:", e);
        }
    }
}

const ICON_PATHS = {
    account_tree: 'M600-200v-40h-80q-33 0-56.5-23.5T440-320v-320h-80v40q0 33-23.5 56.5T280-520H160q-33 0-56.5-23.5T80-600v-160q0-33 23.5-56.5T160-840h120q33 0 56.5 23.5T360-760v40h240v-40q0-33 23.5-56.5T680-840h120q33 0 56.5 23.5T880-760v160q0 33-23.5 56.5T800-520H680q-33 0-56.5-23.5T600-600v-40h-80v320h80v-40q0-33 23.5-56.5T680-440h120q33 0 56.5 23.5T880-360v160q0 33-23.5 56.5T800-120H680q-33 0-56.5-23.5T600-200ZM160-760v160-160Zm520 400v160-160Zm0-400v160-160Zm0 160h120v-160H680v160Zm0 400h120v-160H680v160ZM160-600h120v-160H160v160Z',
    add: 'M440-440H240q-17 0-28.5-11.5T200-480q0-17 11.5-28.5T240-520h200v-200q0-17 11.5-28.5T480-760q17 0 28.5 11.5T520-720v200h200q17 0 28.5 11.5T760-480q0 17-11.5 28.5T720-440H520v200q0 17-11.5 28.5T480-200q-17 0-28.5-11.5T440-240v-200Z',
    check_circle: 'm424-408-86-86q-11-11-28-11t-28 11q-11 11-11 28t11 28l114 114q12 12 28 12t28-12l226-226q11-11 11-28t-11-28q-11-11-28-11t-28 11L424-408Zm56 328q-83 0-156-31.5T197-197q-54-54-85.5-127T80-480q0-83 31.5-156T197-763q54-54 127-85.5T480-880q83 0 156 31.5T763-763q54 54 85.5 127T880-480q0 83-31.5 156T763-197q-54 54-127 85.5T480-80Zm0-80q134 0 227-93t93-227q0-134-93-227t-227-93q-134 0-227 93t-93 227q0 134 93 227t227 93Zm0-320Z',
    close: 'M480-424 284-228q-11 11-28 11t-28-11q-11-11-11-28t11-28l196-196-196-196q-11-11-11-28t11-28q11-11 28-11t28 11l196 196 196-196q11-11 28-11t28 11q11 11 11 28t-11 28L536-480l196 196q11 11 11 28t-11 28q-11 11-28 11t-28-11L480-424Z',
    delete: 'M280-120q-33 0-56.5-23.5T200-200v-520q-17 0-28.5-11.5T160-760q0-17 11.5-28.5T200-800h160q0-17 11.5-28.5T400-840h160q17 0 28.5 11.5T600-800h160q17 0 28.5 11.5T800-760q0 17-11.5 28.5T760-720v520q0 33-23.5 56.5T680-120H280Zm400-600H280v520h400v-520ZM428.5-291.5Q440-303 440-320v-280q0-17-11.5-28.5T400-640q-17 0-28.5 11.5T360-600v280q0 17 11.5 28.5T400-280q17 0 28.5-11.5Zm160 0Q600-303 600-320v-280q0-17-11.5-28.5T560-640q-17 0-28.5 11.5T520-600v280q0 17 11.5 28.5T560-280q17 0 28.5-11.5ZM280-720v520-520Z',
    error: 'M508.5-291.5Q520-303 520-320t-11.5-28.5Q497-360 480-360t-28.5 11.5Q440-337 440-320t11.5 28.5Q463-280 480-280t28.5-11.5Zm0-160Q520-463 520-480v-160q0-17-11.5-28.5T480-680q-17 0-28.5 11.5T440-640v160q0 17 11.5 28.5T480-440q17 0 28.5-11.5ZM480-80q-83 0-156-31.5T197-197q-54-54-85.5-127T80-480q0-83 31.5-156T197-763q54-54 127-85.5T480-880q83 0 156 31.5T763-763q54 54 85.5 127T880-480q0 83-31.5 156T763-197q-54 54-127 85.5T480-80Zm0-80q134 0 227-93t93-227q0-134-93-227t-227-93q-134 0-227 93t-93 227q0 134 93 227t227 93Zm0-320Z',
    extension: 'M352-120H200q-33 0-56.5-23.5T120-200v-152q48 0 84-30.5t36-77.5q0-47-36-77.5T120-568v-152q0-33 23.5-56.5T200-800h160q0-42 29-71t71-29q42 0 71 29t29 71h160q33 0 56.5 23.5T800-720v160q42 0 71 29t29 71q0 42-29 71t-71 29v160q0 33-23.5 56.5T720-120H568q0-50-31.5-85T460-240q-45 0-76.5 35T352-120Zm-152-80h85q24-66 77-93t98-27q45 0 98 27t77 93h85v-240h80q8 0 14-6t6-14q0-8-6-14t-14-6h-80v-240H480v-80q0-8-6-14t-14-6q-8 0-14 6t-6 14v80H200v88q54 20 87 67t33 105q0 57-33 104t-87 68v88Zm260-260Z',
    filter_list: 'M440-240q-17 0-28.5-11.5T400-280q0-17 11.5-28.5T440-320h80q17 0 28.5 11.5T560-280q0 17-11.5 28.5T520-240h-80ZM280-440q-17 0-28.5-11.5T240-480q0-17 11.5-28.5T280-520h400q17 0 28.5 11.5T720-480q0 17-11.5 28.5T680-440H280ZM160-640q-17 0-28.5-11.5T120-680q0-17 11.5-28.5T160-720h640q17 0 28.5 11.5T840-680q0 17-11.5 28.5T800-640H160Z',
    home: 'M240-200h120v-200q0-17 11.5-28.5T400-440h160q17 0 28.5 11.5T600-400v200h120v-360L480-740 240-560v360Zm-80 0v-360q0-19 8.5-36t23.5-28l240-180q21-16 48-16t48 16l240 180q15 11 23.5 28t8.5 36v360q0 33-23.5 56.5T720-120H560q-17 0-28.5-11.5T520-160v-200h-80v200q0 17-11.5 28.5T400-120H240q-33 0-56.5-23.5T160-200Zm320-270Z',
    memory: 'M360-400v-160q0-17 11.5-28.5T400-600h160q17 0 28.5 11.5T600-560v160q0 17-11.5 28.5T560-360H400q-17 0-28.5-11.5T360-400Zm80-40h80v-80h-80v80Zm-80 280v-40h-80q-33 0-56.5-23.5T200-280v-80h-40q-17 0-28.5-11.5T120-400q0-17 11.5-28.5T160-440h40v-80h-40q-17 0-28.5-11.5T120-560q0-17 11.5-28.5T160-600h40v-80q0-33 23.5-56.5T280-760h80v-40q0-17 11.5-28.5T400-840q17 0 28.5 11.5T440-800v40h80v-40q0-17 11.5-28.5T560-840q17 0 28.5 11.5T600-800v40h80q33 0 56.5 23.5T760-680v80h40q17 0 28.5 11.5T840-560q0 17-11.5 28.5T800-520h-40v80h40q17 0 28.5 11.5T840-400q0 17-11.5 28.5T800-360h-40v80q0 33-23.5 56.5T680-200h-80v40q0 17-11.5 28.5T560-120q-17 0-28.5-11.5T520-160v-40h-80v40q0 17-11.5 28.5T400-120q-17 0-28.5-11.5T360-160Zm320-120v-400H280v400h400ZM480-480Z',
    refresh: 'M480-160q-134 0-227-93t-93-227q0-134 93-227t227-93q69 0 132 28.5T720-690v-70q0-17 11.5-28.5T760-800q17 0 28.5 11.5T800-760v200q0 17-11.5 28.5T760-520H560q-17 0-28.5-11.5T520-560q0-17 11.5-28.5T560-600h128q-32-56-87.5-88T480-720q-100 0-170 70t-70 170q0 100 70 170t170 70q68 0 124.5-34.5T692-367q8-14 22.5-19.5t29.5-.5q16 5 23 21t-1 30q-41 80-117 128t-169 48Z',
    search: 'M380-320q-109 0-184.5-75.5T120-580q0-109 75.5-184.5T380-840q109 0 184.5 75.5T640-580q0 44-14 83t-38 69l224 224q11 11 11 28t-11 28q-11 11-28 11t-28-11L532-372q-30 24-69 38t-83 14Zm0-80q75 0 127.5-52.5T560-580q0-75-52.5-127.5T380-760q-75 0-127.5 52.5T200-580q0 75 52.5 127.5T380-400Z',
    settings: 'M433-80q-27 0-46.5-18T363-142l-9-66q-13-5-24.5-12T307-235l-62 26q-25 11-50 2t-39-32l-47-82q-14-23-8-49t27-43l53-40q-1-7-1-13.5v-27q0-6.5 1-13.5l-53-40q-21-17-27-43t8-49l47-82q14-23 39-32t50 2l62 26q11-8 23-15t24-12l9-66q4-26 23.5-44t46.5-18h94q27 0 46.5 18t23.5 44l9 66q13 5 24.5 12t22.5 15l62-26q25-11 50-2t39 32l47 82q14 23 8 49t-27 43l-53 40q1 7 1 13.5v27q0 6.5-2 13.5l53 40q21 17 27 43t-8 49l-48 82q-14 23-39 32t-50-2l-60-26q-11 8-23 15t-24 12l-9 66q-4 26-23.5 44T527-80h-94Zm7-80h79l14-106q31-8 57.5-23.5T639-327l99 41 39-68-86-65q5-14 7-29.5t2-31.5q0-16-2-31.5t-7-29.5l86-65-39-68-99 42q-22-23-48.5-38.5T533-694l-13-106h-79l-14 106q-31 8-57.5 23.5T321-633l-99-41-39 68 86 64q-5 15-7 30t-2 32q0 16 2 31t7 30l-86 65 39 68 99-42q22 23 48.5 38.5T427-266l13 106Zm42-180q58 0 99-41t41-99q0-58-41-99t-99-41q-59 0-99.5 41T342-480q0 58 40.5 99t99.5 41Zm-2-140Z',
    settings_suggest: 'm697-696-56-26q-12-5-12-18t12-18l56-26 26-56q5-12 18-12t18 12l26 56 56 26q12 5 12 18t-12 18l-56 26-26 56q-5 12-18 12t-18-12l-26-56Zm92 308-49-23q-6-3-6-9t6-9l49-23 23-49q3-6 9-6t9 6l23 49 49 23q6 3 6 9t-6 9l-49 23-23 49q-3 6-9 6t-9-6l-23-49ZM336-80q-15 0-26-10t-13-25l-8-59q-7-3-15-8t-13-10l-55 24q-14 6-28.5 1.5T155-185L91-297q-8-14-4.5-28.5T102-349l47-35v-32l-47-35q-12-9-15.5-23.5T91-503l64-112q8-14 22.5-18.5T206-632l55 24q5-5 13-10t15-8l8-59q2-15 13-25t26-10h130q15 0 26 10t13 25l8 59q7 3 15 8t13 10l55-24q14-6 28.5-1.5T647-615l64 112q8 14 4.5 28.5T700-451l-47 35v32l47 35q12 9 15.5 23.5T711-297l-64 112q-8 14-22.5 18.5T596-168l-55-24q-5 5-13 10t-15 8l-8 59q-2 15-13 25t-26 10H336Zm150-235q35-35 35-85t-35-85q-35-35-85-35t-85 35q-35 35-35 85t35 85q35 35 85 35t85-35ZM371-160h60l8-72q29-8 49.5-20.5T529-286l66 30 28-50-58-44q8-23 8-50t-8-50l58-44-28-50-66 30q-20-21-40.5-33.5T439-568l-8-72h-60l-8 72q-29 8-49.5 20.5T273-514l-66-30-28 50 58 44q-8 23-8.5 50t8.5 50l-58 44 28 50 66-30q20 21 40.5 33.5T363-232l8 72Zm30-240Z',
    shield: 'M467-85q-6-1-12-3-135-45-215-166.5T160-516v-189q0-25 14.5-45t37.5-29l240-90q14-5 28-5t28 5l240 90q23 9 37.5 29t14.5 45v189q0 140-80 261.5T505-88q-6 2-12 3t-13 1q-7 0-13-1Zm13-79q104-33 172-132t68-220v-189l-240-90-240 90v189q0 121 68 220t172 132Zm0-316Z'
};

const FILLED_ICON_PATHS = {
    extension: 'M352-120H200q-33 0-56.5-23.5T120-200v-152q48 0 84-30.5t36-77.5q0-47-36-77.5T120-568v-152q0-33 23.5-56.5T200-800h160q0-42 29-71t71-29q42 0 71 29t29 71h160q33 0 56.5 23.5T800-720v160q42 0 71 29t29 71q0 42-29 71t-71 29v160q0 33-23.5 56.5T720-120H568q0-50-31.5-85T460-240q-45 0-76.5 35T352-120Z',
    home: 'M160-200v-360q0-19 8.5-36t23.5-28l240-180q21-16 48-16t48 16l240 180q15 11 23.5 28t8.5 36v360q0 33-23.5 56.5T720-120H600q-17 0-28.5-11.5T560-160v-200q0-17-11.5-28.5T520-400h-80q-17 0-28.5 11.5T400-360v200q0 17-11.5 28.5T360-120H240q-33 0-56.5-23.5T160-200Z',
    settings: 'M433-80q-27 0-46.5-18T363-142l-9-66q-13-5-24.5-12T307-235l-62 26q-25 11-50 2t-39-32l-47-82q-14-23-8-49t27-43l53-40q-1-7-1-13.5v-27q0-6.5 1-13.5l-53-40q-21-17-27-43t8-49l47-82q14-23 39-32t50 2l62 26q11-8 23-15t24-12l9-66q4-26 23.5-44t46.5-18h94q27 0 46.5 18t23.5 44l9 66q13 5 24.5 12t22.5 15l62-26q25-11 50-2t39 32l47 82q14 23 8 49t-27 43l-53 40q1 7 1 13.5v27q0 6.5-2 13.5l53 40q21 17 27 43t-8 49l-48 82q-14 23-39 32t-50-2l-60-26q-11 8-23 15t-24 12l-9 66q-4 26-23.5 44T527-80h-94Zm49-260q58 0 99-41t41-99q0-58-41-99t-99-41q-59 0-99.5 41T342-480q0 58 40.5 99t99.5 41Z',
    shield: 'M467-85q-6-1-12-3-135-45-215-166.5T160-516v-189q0-25 14.5-45t37.5-29l240-90q14-5 28-5t28 5l240 90q23 9 37.5 29t14.5 45v189q0 140-80 261.5T505-88q-6 2-12 3t-13 1q-7 0-13-1Z'
};

function getIconVariant(icon) {
    return icon.closest('.nav-item.active') ? 'filled' : 'outline';
}

function getIconPath(name, variant = 'outline') {
    return variant === 'filled' ? FILLED_ICON_PATHS[name] || ICON_PATHS[name] : ICON_PATHS[name];
}

function setIcon(icon, name, variant = getIconVariant(icon)) {
    if (!icon) return;
    const pathData = getIconPath(name, variant);
    if (!pathData) return;
    if (
        icon.dataset.icon === name
        && icon.dataset.iconVariant === variant
        && icon.firstElementChild?.tagName.toLowerCase() === 'svg'
    ) return;

    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 -960 960 960');
    svg.setAttribute('aria-hidden', 'true');

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', pathData);
    svg.appendChild(path);

    icon.dataset.icon = name;
    icon.dataset.iconVariant = variant;
    icon.setAttribute('aria-hidden', 'true');
    icon.replaceChildren(svg);
}

function createIconElement(name, variant = 'outline') {
    const icon = document.createElement('md-icon');
    icon.dataset.icon = name;
    icon.dataset.iconVariant = variant;
    icon.setAttribute('aria-hidden', 'true');
    setIcon(icon, name, variant);
    return icon;
}

function applyOfflineIcons(root = document) {
    root.querySelectorAll?.('md-icon').forEach(icon => {
        const name = icon.dataset.icon || icon.textContent.trim();
        if (!name) return;
        setIcon(icon, name);
    });
}

let offlineIconsInitialized = false;
function initOfflineIcons() {
    if (offlineIconsInitialized) return;
    offlineIconsInitialized = true;

    applyOfflineIcons();
    new MutationObserver(() => requestAnimationFrame(() => applyOfflineIcons()))
        .observe(document.body || document.documentElement, {
            attributeFilter: ['class'],
            attributes: true,
            childList: true,
            subtree: true
        });
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initOfflineIcons, { once: true });
} else {
    initOfflineIcons();
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

function parseUidList(text) {
    const uids = String(text || '')
        .split('\n')
        .map(line => line.trim())
        .filter(isValidUid);
    return [...new Set(uids)];
}

function buildWriteUidListCommand(uids) {
    const safeUids = [...new Set(uids.map(String).filter(isValidUid))];
    if (safeUids.length === 0) return `: > ${FILES.exclusions}`;
    return `printf '%s\\n' ${safeUids.join(' ')} > ${FILES.exclusions}`;
}

function shellQuote(value) {
    return "'" + String(value).replace(/'/g, "'\\''") + "'";
}

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

function renderTextState(container, className, text) {
    const message = document.createElement('div');
    message.className = className;
    message.textContent = text;
    container.replaceChildren(message);
}

function renderEmptyState(container, face, text) {
    const wrapper = document.createElement('div');
    wrapper.className = 'empty-list-placeholder empty-state';

    const faceEl = document.createElement('div');
    faceEl.className = 'empty-face';
    faceEl.textContent = face;

    const textEl = document.createElement('div');
    textEl.className = 'empty-text';
    textEl.textContent = text;

    wrapper.append(faceEl, textEl);
    container.replaceChildren(wrapper);
}

function applyAppIconFallback(img) {
    img.onerror = () => {
        img.onerror = null;
        img.src = APP_ICON_FALLBACK;
    };
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

function getActiveView() {
    return document.querySelector('.view-content.active');
}

function isCollapsibleTopBarView(view = getActiveView()) {
    return view && (view.id === 'view-modules' || view.id === 'view-exclusions');
}

function clamp(value, min = 0, max = 1) {
    return Math.min(max, Math.max(min, value));
}

function updateTopAppBar() {
    const container = document.querySelector('.page-container');
    const topBar = document.getElementById('top-app-bar');
    const topBarTitle = document.getElementById('top-app-bar-title');
    const activeView = getActiveView();
    if (!container || !topBar || !topBarTitle) return;

    if (!isCollapsibleTopBarView(activeView)) {
        topBarTitle.textContent = '';
        topBar.style.setProperty('--top-app-bar-opacity', '0');
        topBar.style.setProperty('--top-app-title-opacity', '0');
        topBar.classList.remove('visible');
        topBar.classList.remove('show-title');
        topBar.setAttribute('aria-hidden', 'true');
        return;
    }

    const scrollTop = container.scrollTop;
    const title = activeView.querySelector('.header-title')?.textContent?.trim() || '';
    const shouldShow = scrollTop > 0.5;
    const titleOpacity = clamp((scrollTop - 18) / 24);
    const shouldShowTitle = titleOpacity > 0;

    topBarTitle.textContent = title;
    topBar.style.setProperty('--top-app-bar-opacity', shouldShow ? '1' : '0');
    topBar.style.setProperty('--top-app-title-opacity', titleOpacity.toFixed(3));
    topBar.classList.toggle('visible', shouldShow);
    topBar.classList.toggle('show-title', shouldShowTitle);
    topBar.setAttribute('aria-hidden', titleOpacity > 0.5 ? 'false' : 'true');
}

function initTopAppBar() {
    const container = document.querySelector('.page-container');
    if (!container) return;

    let rafId = 0;
    container.addEventListener('scroll', () => {
        if (rafId) cancelAnimationFrame(rafId);
        rafId = requestAnimationFrame(() => {
            rafId = 0;
            updateTopAppBar();
        });
    }, { passive: true });

    updateTopAppBar();
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
            applyOfflineIcons();

            const targetId = item.dataset.target;
            views.forEach(view => {
                view.classList.remove('active');
            });
            document.getElementById(targetId).classList.add('active');
            updateTopAppBar();

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
    setIcon(icon, 'check_circle', 'outline');
}

function setInactiveUI(title, label, box, icon) {
    if (title) title.textContent = "Inactive";
    label?.classList.add('inactive');
    label?.classList.remove('active');
    box?.classList.remove('active');
    box?.classList.add('inactive');
    if (icon) {
        icon.classList.add('inactive');
        setIcon(icon, 'error', 'outline');
    }
}

// Modules
let currentRenderId = 0;
async function loadModules() {
    const listContainer = document.getElementById('modules-list');
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

        listContainer.replaceChildren();

        if (lines.length === 0) {
            renderEmptyState(listContainer, '(._.)', 'No modules found');
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
                    if (listContainer.children.length === 0) {
                        renderEmptyState(listContainer, '(._.)', 'No modules found');
                    }
                }
            });
        };

        processEntries();

    } catch (e) {
        console.error("Error loading modules:", e);
        renderTextState(listContainer, 'error-message', `Error loading modules: ${e.message}`);
    }
}

async function loadModule(modId) {
    if (!isValidModId(modId)) return;
    const TARGET_PARTITIONS = ["system", "vendor", "product", "system_ext", "odm", "oem"];
    const modPath = `${MOD_DIR}/${modId}`;
    const partitionsStr = TARGET_PARTITIONS.map(shellQuote).join(' ');
    const quotedModPath = shellQuote(modPath);
    const quotedNmBin = shellQuote(NM_BIN);
    const loadScript = `
        cd ${quotedModPath} || exit 0
        find -L ${partitionsStr} \\( -type f -o -type l \\) -exec sh -c '
            mod="$1"; shift
            for f do
                printf "/%s\\0%s/%s\\0" "$f" "$mod" "$f"
            done
        ' _ ${quotedModPath} {} + 2>/dev/null | xargs -0 -r -n 500 ${quotedNmBin} add
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
            const escapedTargets = chunk.map(shellQuote).join(' ');
            const batchScript = `printf '%s\\0' ${escapedTargets} | xargs -0 -r -n 500 ${shellQuote(NM_BIN)} del`;
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
const APP_ICON_FALLBACK = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==";

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
let exclusionsLoadId = 0;

async function loadExclusions() {
    const listContainer = document.getElementById('exclusions-list');
    if (!listContainer) return;
    const loadId = ++exclusionsLoadId;

    try {
        const cat = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        const blockedUids = parseUidList(cat.stdout);

        if (blockedUids.length > 0) {
            try {
                await ensureAppsCache();
            } catch (e) {
                console.warn("App cache unavailable, showing UID fallback:", e);
            }
        }

        const appsMap = new Map(allAppsCache.map(app => [String(app.uid), app]));
        const fragment = document.createDocumentFragment();

        blockedUids.forEach(uid => {
            const app = appsMap.get(uid);
            const label = app ? (app.appLabel || app.packageName) : `UID: ${uid}`;
            const pkg = app ? app.packageName : 'System/Unknown';

            const item = document.createElement('div');
            item.className = 'card setting-item';
            item.dataset.uid = uid;

            const appInfo = document.createElement('div');
            appInfo.className = 'exclusion-app';

            const icon = document.createElement('img');
            icon.src = `ksu://icon/${pkg}`;
            icon.className = 'app-icon-img';
            applyAppIconFallback(icon);

            const text = document.createElement('div');
            text.className = 'setting-text';

            const title = document.createElement('h3');
            title.textContent = label;

            const subtitle = document.createElement('p');
            subtitle.textContent = pkg;

            text.append(title, subtitle);
            appInfo.append(icon, text);

            const deleteButton = document.createElement('md-icon-button');
            deleteButton.className = 'btn-delete';
            deleteButton.setAttribute('aria-label', 'Remove exclusion');
            deleteButton.appendChild(createIconElement('delete'));
            deleteButton.onclick = () => removeExclusion(uid, label);

            item.append(appInfo, deleteButton);
            fragment.appendChild(item);
        });

        requestAnimationFrame(() => {
            if (loadId !== exclusionsLoadId) return;
            if (blockedUids.length === 0) {
                renderEmptyState(listContainer, '(._.)', 'No exclusions yet');
                return;
            }

            listContainer.replaceChildren(fragment);
        });
    } catch (e) {
        console.error(e);
        renderTextState(listContainer, 'error-message', 'Error loading exclusions');
        showToast("Error loading exclusions");
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
    if (!modal || !container || !searchInput || !filterMenu || !filterBtn || !sysSwitch || !closeModalBtn) return;

    modal.classList.add('active');

    if (listObserver) listObserver.disconnect();
    filterMenu.classList.remove('active'); 
    searchInput.value = '';
    
    if (sysSwitch) sysSwitch.selected = showSystemApps;

    const closeModal = () => {
        modal.classList.remove('active');
        if (listObserver) listObserver.disconnect();
    };
    closeModalBtn.onclick = closeModal;

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
        renderTextState(container, 'error-message', `Error: ${e.message}`);
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
        container.replaceChildren();
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
        if (appListRenderIndex === 0) {
            renderEmptyState(container, '(._.)', 'No apps found');
        }
        return;
    }

    const fragment = document.createDocumentFragment();

    batch.forEach(app => {
        const item = document.createElement('div');
        item.className = 'app-item segment-card';

        const icon = document.createElement('img');
        icon.src = `ksu://icon/${app.packageName}`;
        icon.className = 'app-icon-img';
        icon.loading = 'lazy';
        applyAppIconFallback(icon);

        const details = document.createElement('div');
        details.className = 'app-details';

        const name = document.createElement('div');
        name.className = 'app-name';
        name.textContent = app.appLabel;

        const pkg = document.createElement('div');
        pkg.className = 'app-pkg';
        pkg.textContent = app.packageName;

        const meta = document.createElement('div');
        meta.className = 'app-meta';

        const uid = document.createElement('div');
        uid.className = 'uid-label';
        uid.textContent = `UID: ${app.uid}`;

        meta.appendChild(uid);
        if (app.isSystem) {
            const chip = document.createElement('span');
            chip.className = 'system-chip';
            chip.textContent = 'SYS';
            meta.appendChild(chip);
        }

        details.append(name, pkg);
        item.append(icon, details, meta);

        item.addEventListener('click', async () => {
            if (item.dataset.busy === 'true') return;
            item.dataset.busy = 'true';
            await addExclusion(app.uid, app.appLabel || app.packageName);
            if (listObserver) listObserver.disconnect();
            document.getElementById('app-selector-modal')?.classList.remove('active');
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
        const cat = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        const remainingUids = parseUidList(cat.stdout).filter(line => line !== String(uid));

        const writeRes = await exec(buildWriteUidListCommand(remainingUids));
        if (writeRes.errno !== 0) throw new Error(writeRes.stderr || "Failed to update exclusion list");

        const unblockRes = await exec(`${NM_BIN} unblock ${uid}`);
        if (unblockRes.errno !== 0) {
            console.warn("Runtime unblock failed:", unblockRes.stderr);
            showToast("Removed; runtime unblock failed");
        }

        await loadExclusions();
    } catch (e) {
        console.error("Error unblocking:", e);
        showToast("Error unblocking");
        await loadExclusions();
    }
}

async function addExclusion(uid, name) {
    if (!isValidUid(uid)) return showToast("Invalid UID");
    const uidString = String(uid);
    try {
        const cat = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        const currentUids = parseUidList(cat.stdout);
        const alreadyBlocked = currentUids.includes(uidString);

        if (!alreadyBlocked) {
            const writeRes = await exec(buildWriteUidListCommand([...currentUids, uidString]));
            if (writeRes.errno !== 0) throw new Error(writeRes.stderr || "Failed to update exclusion list");
        }

        const blockRes = await exec(`${NM_BIN} block ${uidString}`);
        if (blockRes.errno !== 0) {
            console.warn("Runtime block failed:", blockRes.stderr);
            showToast("Saved; runtime block failed");
        } else {
            showToast(alreadyBlocked ? "Already blocked" : `Blocked: ${name}`);
        }

        await loadExclusions();
    } catch (e) {
        console.error("Error blocking:", e);
        showToast("Error blocking");
        await loadExclusions();
    }
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
        swVerbose.onchange = (e) => {
            exec(e.target.selected ? `touch ${FILES.verbose}` : `rm ${FILES.verbose}`);
        };
    }

    if (swSafe) {
        swSafe.onchange = (e) => {
            exec(e.target.selected ? `touch ${FILES.disable}` : `rm ${FILES.disable}`);
        };
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
    
    const indicatorIcon = indicator.querySelector('md-icon');

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
    initOfflineIcons();
    syncSystemBarTheme();
    initNavigation();
    initTopAppBar();
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
