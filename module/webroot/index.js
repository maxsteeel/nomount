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
            try { ksu.exec(cmd, '{}', key); } 
            catch (e) { delete window[key]; resolve({ errno: 1, stdout: '', stderr: e?.message || 'failed' }); }
        } else resolve({ errno: 1, stdout: '', stderr: 'ksu not defined' });
    });
}

function showToast(msg) {
    if (typeof ksu !== 'undefined' && ksu.toast) {
        try { ksu.toast(msg); } catch (e) {}
    }
}

// i18n Engine
const LOCALE_NAMES = {
    en: 'English',
    es: 'Español',
    id: 'Bahasa Indonesia',
    zh: '简体中文',
    ru: 'Русский',
    tr: 'Türkçe'
};
let activeLocale = 'en', translations = {};

const TPL_RE = /{{\s*([^\s}]+(?:[ \t]+[^\s}]+)*)\s*}}/g;
const translate = (key, reps) => {
    const str = translations[key] ?? key;
    return reps ? String(str).replace(TPL_RE, (_, n) => reps[n] ?? '') : str;
};

const translationsCache = {}; 
let cachedI18nNodes = null;

async function setAppLocale(locale) {
    activeLocale = locale in LOCALE_NAMES ? locale : 'en';

    if (!translationsCache[activeLocale]) {
        try {
            const response = await fetch(`./locales/${activeLocale}.json`);
            translationsCache[activeLocale] = response.ok ? await response.json() : {};
        } catch { translationsCache[activeLocale] = {}; }
    }

    translations = translationsCache[activeLocale];
    document.documentElement.lang = activeLocale;
    localStorage.setItem('nm_locale', activeLocale);
    if (!cachedI18nNodes) cachedI18nNodes = document.querySelectorAll('[data-i18n]');

    cachedI18nNodes.forEach(el => {
        const text = translate(el.dataset.i18n);
        const targetAttr = el.dataset.i18nAttr;
        
        if (targetAttr) el.setAttribute(targetAttr, text);
        else if ('placeholder' in el) el.placeholder = text;
        else el.textContent = text;
    });

    renderLanguagePicker();
    const activeViewId = document.querySelector('.view-content.active')?.id;

    for (const id in viewLoadState) viewLoadState[id] = id === activeViewId;
    if (activeViewId && activeViewId !== 'view-options') refreshCurrentView();
}

function renderLanguagePicker() {
    const wrapper = document.getElementById('lang-select-wrapper');
    const valueDisplay = document.getElementById('lang-select-value');
    const menu = document.getElementById('lang-select-menu');
    if (!wrapper || !valueDisplay || !menu) return;

    menu.replaceChildren();
    for (const loc in LOCALE_NAMES) {
        const optionEl = document.createElement('div');
        optionEl.className = `custom-select-option ${loc === activeLocale ? 'selected' : ''}`;
        optionEl.textContent = LOCALE_NAMES[loc];
        if (loc === activeLocale) valueDisplay.textContent = LOCALE_NAMES[loc];
        
        optionEl.onclick = (e) => {
            e.stopPropagation();
            wrapper.classList.remove('open');
            if (loc !== activeLocale) setTimeout(() => setAppLocale(loc), 0);
        };
        menu.appendChild(optionEl);
    }

    if (!wrapper.dataset.listenerAttached) {
        document.getElementById('lang-select-trigger').onclick = (e) => { 
            e.stopPropagation(); 
            wrapper.classList.toggle('open'); 
        };
        document.addEventListener('click', () => wrapper.classList.remove('open'));
        wrapper.dataset.listenerAttached = 'true';
    }
}

// Constants & Helpers
const MOD_DIR = "/data/adb/modules";
const NM_DATA = "/data/adb/nomount";
const NM_BIN = "/data/adb/modules/nomount/bin/nm";
const FILES = { verbose: `${NM_DATA}/.verbose`, disable: `${NM_DATA}/disable`, exclusions: `${NM_DATA}/.exclusion_list` };
const APP_ICON_FALLBACK = "data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0iIzgwODA4MCI+PHBhdGggZD0iTTEyIDJDNi40OCAyIDIgNi40OCAyIDEyczQuNDggMTAgMTAgMTAgMTAtNC40OCAxMC0xMFMxNy41MiAyIDEyIDJ6bTAgMThjLTQuNDEgMC04LTMuNTktOC04czMuNTktOCA4LTggOCAzLjU5IDggOC0zLjU5IDgtOCA4eiIvPjwvc3ZnPg==";
const viewLoadState = { 'view-home': false, 'view-modules': false, 'view-exclusions': false, 'view-options': false };

const normalizeUidList = uids => [...new Set(uids.map(value => {
    const uid = String(value ?? '').trim();
    if (!/^\d+$/.test(uid)) return null;
    const numericUid = Number(uid);
    return Number.isSafeInteger(numericUid) && numericUid <= 0xffffffff ? String(numericUid) : null;
}).filter(Boolean))];
const parseUidList = text => normalizeUidList(String(text ?? '').split(/[\s,]+/));
const serializeUidList = uids => {
    const safe = normalizeUidList(uids);
    return safe.join('\n');
};
const buildWriteUidListCmd = uids => {
    const safe = normalizeUidList(uids);
    const tempFile = `${FILES.exclusions}.tmp`;
    const write = safe.length ? `printf '%s\\n' ${safe.join(' ')}` : ':';
    return `mkdir -p ${NM_DATA} && { ${write} > ${tempFile} && mv -f ${tempFile} ${FILES.exclusions}; }`;
};
const renderTextState = (el, cls, text) => { el.className = cls; el.textContent = text; };
const renderEmptyState = (el, face, text) => el.innerHTML = `<div class="empty-list-placeholder empty-state"><div class="empty-face">${face}</div><div class="empty-text">${text}</div></div>`;
const delay = ms => new Promise(r => setTimeout(r, ms));

// Icons
customElements.define('md-icon', class extends HTMLElement {});
const ICON_PATHS = {
    account_tree: 'M600-200v-40h-80q-33 0-56.5-23.5T440-320v-320h-80v40q0 33-23.5 56.5T280-520H160q-33 0-56.5-23.5T80-600v-160q0-33 23.5-56.5T160-840h120q33 0 56.5 23.5T360-760v40h240v-40q0-33 23.5-56.5T680-840h120q33 0 56.5 23.5T880-760v160q0 33-23.5 56.5T800-520H680q-33 0-56.5-23.5T600-600v-40h-80v320h80v-40q0-33 23.5-56.5T680-440h120q33 0 56.5 23.5T880-360v160q0 33-23.5 56.5T800-120H680q-33 0-56.5-23.5T600-200ZM160-760v160-160Zm520 400v160-160Zm0-400v160-160Zm0 160h120v-160H680v160Zm0 400h120v-160H680v160ZM160-600h120v-160H160v160Z',
    add: 'M440-440H240q-17 0-28.5-11.5T200-480q0-17 11.5-28.5T240-520h200v-200q0-17 11.5-28.5T480-760q17 0 28.5 11.5T520-720v200h200q17 0 28.5 11.5T760-480q0 17-11.5 28.5T720-440H520v200q0 17-11.5 28.5T480-200q-17 0-28.5-11.5T440-240v-200Z',
    arrow_drop_down: 'M480-360 280-560h400L480-360Z',
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
    shield: 'M467-85q-6-1-12-3-135-45-215-166.5T160-516v-189q0-25 14.5-45t37.5-29l240-90q14-5 28-5t28 5l240 90q23 9 37.5 29t14.5 45v189q0 140-80 261.5T505-88q-6 2-12 3t-13 1q-7 0-13-1Zm13-79q104-33 172-132t68-220v-189l-240-90-240 90v189q0 121 68 220t172 132Zm0-316Z',
    smartphone: 'M680-920H280c-44 0-80 36-80 80v720c0 44 36 80 80 80h400c44 0 80-36 80-80v-720c0-44-36-80-80-80Zm0 720H280v-600h400v600Zm-120 80h-160v-40h160v40Z',
};

const FILLED_ICON_PATHS = {
    extension: 'M352-120H200q-33 0-56.5-23.5T120-200v-152q48 0 84-30.5t36-77.5q0-47-36-77.5T120-568v-152q0-33 23.5-56.5T200-800h160q0-42 29-71t71-29q42 0 71 29t29 71h160q33 0 56.5 23.5T800-720v160q42 0 71 29t29 71q0 42-29 71t-71 29v160q0 33-23.5 56.5T720-120H568q0-50-31.5-85T460-240q-45 0-76.5 35T352-120Z',
    home: 'M160-200v-360q0-19 8.5-36t23.5-28l240-180q21-16 48-16t48 16l240 180q15 11 23.5 28t8.5 36v360q0 33-23.5 56.5T720-120H600q-17 0-28.5-11.5T560-160v-200q0-17-11.5-28.5T520-400h-80q-17 0-28.5 11.5T400-360v200q0 17-11.5 28.5T360-120H240q-33 0-56.5-23.5T160-200Z',
    settings: 'M433-80q-27 0-46.5-18T363-142l-9-66q-13-5-24.5-12T307-235l-62 26q-25 11-50 2t-39-32l-47-82q-14-23-8-49t27-43l53-40q-1-7-1-13.5v-27q0-6.5 1-13.5l-53-40q-21-17-27-43t8-49l47-82q14-23 39-32t50 2l62 26q11-8 23-15t24-12l9-66q4-26 23.5-44t46.5-18h94q27 0 46.5 18t23.5 44l9 66q13 5 24.5 12t22.5 15l62-26q25-11 50-2t39 32l47 82q14 23 8 49t-27 43l-53 40q1 7 1 13.5v27q0 6.5-2 13.5l53 40q21 17 27 43t-8 49l-48 82q-14 23-39 32t-50-2l-60-26q-11 8-23 15t-24 12l-9 66q-4 26-23.5 44T527-80h-94Zm49-260q58 0 99-41t41-99q0-58-41-99t-99-41q-59 0-99.5 41T342-480q0 58 40.5 99t99.5 41Z',
    shield: 'M467-85q-6-1-12-3-135-45-215-166.5T160-516v-189q0-25 14.5-45t37.5-29l240-90q14-5 28-5t28 5l240 90q23 9 37.5 29t14.5 45v189q0 140-80 261.5T505-88q-6 2-12 3t-13 1q-7 0-13-1Z'
};

const svgCache = {};
function setIcon(icon, name, variant = 'outline') {
    if (!icon) return;
    if (icon.dataset.icon === name && icon.dataset.iconVariant === variant) return;

    const pathData = (variant === 'filled' ? FILLED_ICON_PATHS[name] : null) || ICON_PATHS[name];
    if (!pathData) return;

    const cacheKey = `${name}_${variant}`;
    let svgMarkup = svgCache[cacheKey];
    if (!svgMarkup) {
        svgMarkup = `<svg viewBox="0 -960 960 960" aria-hidden="true"><path d="${pathData}"/></svg>`;
        svgCache[cacheKey] = svgMarkup;
    }

    icon.dataset.icon = name;
    icon.dataset.iconVariant = variant;
    if (!icon.hasAttribute('aria-hidden')) icon.setAttribute('aria-hidden', 'true');
    icon.innerHTML = svgMarkup;
}

let cachedIconNodes = null;
function applyIcons() {
    if (!cachedIconNodes) cachedIconNodes = document.querySelectorAll('md-icon');
    cachedIconNodes.forEach(icon => {
        const name = icon.dataset.icon || (icon.textContent ? icon.textContent.trim() : null);
        if (name) setIcon(icon, name);
    });
}

let cachedMetaTheme = null;
function syncSystemBarTheme() {
    if (!cachedMetaTheme) cachedMetaTheme = document.querySelector('meta[name="theme-color"]');
    if (!cachedMetaTheme) return;

    const cs = getComputedStyle(document.documentElement);
    const surfaceColor = cs.getPropertyValue('--md-sys-color-background').trim() ||
                         cs.getPropertyValue('--md-sys-color-surface').trim();
    if (surfaceColor) cachedMetaTheme.setAttribute('content', surfaceColor);
}

const homeUI = {};
function getHomeElements() {
    if (!homeUI.kernel) {
        homeUI.stats = document.getElementById('injection-stats');
        homeUI.kernel = document.getElementById('kernel-version');
        homeUI.device = document.getElementById('device-model');
        homeUI.android = document.getElementById('android-ver');
        homeUI.statusTitle = document.getElementById('status-title');
        homeUI.statusLabel = document.getElementById('status-indicator');
        homeUI.statusCard = document.querySelector('.home-status-card');
        homeUI.statusIcon = document.getElementById('status-icon');
    }
    return homeUI;
}

const UI = {};
let currentActiveViewId = 'view-home';
let currentActiveViewTitle = '';

function updateTopAppBar() {
    if (!UI.c) {
        UI.c = document.querySelector('.page-container');
        UI.bar = document.getElementById('top-app-bar');
        UI.title = document.getElementById('top-app-bar-title');
    }

    if (!['view-modules', 'view-exclusions'].includes(currentActiveViewId)) {
        if (UI.title.textContent !== '') UI.title.textContent = '';
        UI.bar.style.setProperty('--top-app-bar-opacity', '0');
        UI.bar.style.setProperty('--top-app-title-opacity', '0');
        UI.bar.classList.remove('visible', 'show-title');
        UI.bar.setAttribute('aria-hidden', 'true');
        return;
    }

    const titleOpacity = Math.max(0, Math.min(1, (UI.c.scrollTop - 18) / 24));

    if (UI.title.textContent !== currentActiveViewTitle)
        UI.title.textContent = currentActiveViewTitle;
 
    UI.bar.style.setProperty('--top-app-bar-opacity', UI.c.scrollTop > 0.5 ? '1' : '0');
    UI.bar.style.setProperty('--top-app-title-opacity', titleOpacity.toFixed(3));
    UI.bar.classList.toggle('visible', UI.c.scrollTop > 0.5);
    UI.bar.classList.toggle('show-title', titleOpacity > 0);
    UI.bar.setAttribute('aria-hidden', titleOpacity > 0.5 ? 'false' : 'true');
}

function initNavigation() {
    const navItems = document.querySelectorAll('.nav-item');
    const views = document.querySelectorAll('.view-content');
    const fab = document.getElementById('fab-container');

    navItems.forEach(item => {
        const iconEl = item.querySelector('md-icon');
        if (iconEl) {
            const iconName = iconEl.dataset.icon || iconEl.textContent.trim();
            setIcon(iconEl, iconName, item.classList.contains('active') ? 'filled' : 'outline');
        }

        item.addEventListener('click', () => {
            navItems.forEach(nav => {
                nav.classList.remove('active');
                const i = nav.querySelector('md-icon');
                if (i) setIcon(i, (i.dataset.icon || i.textContent.trim()), nav === item ? 'filled' : 'outline');
            });
            item.classList.add('active');
            const target = item.dataset.target;
            
            views.forEach(v => v.classList.remove('active'));
            const targetView = document.getElementById(target);
            targetView.classList.add('active');
            
            currentActiveViewId = target;
            currentActiveViewTitle = targetView.querySelector('.header-title')?.textContent?.trim() || '';
            
            updateTopAppBar();
            fab.classList.toggle('visible', target === 'view-exclusions');

            setTimeout(() => {
                if (!viewLoadState[target]) {
                    viewLoadState[target] = true;
                    if (target === 'view-home') loadHome();
                    else if (target === 'view-modules') loadModules();
                    else if (target === 'view-exclusions') loadExclusions();
                    else if (target === 'view-options') loadOptions();
                }
            }, 0);
        });
    });
}

// Home
async function loadHome() {
    try { applyHomeData(JSON.parse(localStorage.getItem('nm_home_cache'))); } catch (e) { console.error("Error loading cache:", e); }

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
        const parts = (await exec(script)).stdout.split('|||').map(s => s.trim());
        if (!parts[6]) parts[6] = "[]";
        let activeModulesCount = 0;
        try {
            const rules = JSON.parse(parts[6]);
            const modCounts = {};
            rules.forEach(r => {
                if (r?.real?.startsWith(MOD_DIR)) {
                    const modName = r.real.split('/')[4];
                    if (modName && modName !== 'nomount') modCounts[modName] = 1;
                }
            });
            activeModulesCount = Object.keys(modCounts).length;
        } catch (e) { console.error("Error parsing rules:", e); }

        const unk = translate('unknown_value');
        const raw = parts.slice(0, 6);
        const kVer = raw[0] || unk,
              model = raw[1] || unk,
              aRel = raw[2] || unk,
              aSdk = raw[3] || unk,
              mVer = raw[4] || unk,
              dVer = raw[5] || unk;

        const homeData = {
            kernelVer: kVer, deviceModel: model,
            androidInfo: `Android ${aRel} (API ${aSdk})`,
            versionFull: `${mVer} (${dVer})`,
            active: dVer !== unk
        };

        requestAnimationFrame(() => {
            applyHomeData(homeData, activeModulesCount === 1 ? translate('module_injected_count') : translate('modules_injected_count', { count: activeModulesCount }));
            localStorage.setItem('nm_home_cache', JSON.stringify(homeData));
        });
    } catch (e) { console.error("Delayed Home update error:", e); }
}

function applyHomeData(data, statsText) {
    const el = getHomeElements();
    if (el.kernel) el.kernel.textContent = data.kernelVer || translate('unknown_value');
    if (el.device) el.device.textContent = data.deviceModel || translate('unknown_value');
    if (el.android) el.android.textContent = data.androidInfo || translate('unknown_value');
    if (el.statusLabel) el.statusLabel.textContent = data.versionFull || translate('unknown_value');
    if (statsText && el.stats) el.stats.textContent = statsText;

    if (el.statusTitle) el.statusTitle.textContent = translate(data.active ? 'status_active' : 'status_inactive');
    [el.statusLabel, el.statusCard].forEach(e => {
        if (e) { e.classList.toggle('active', data.active); e.classList.toggle('inactive', !data.active); }
    });
    if (el.statusIcon) {
        el.statusIcon.classList.toggle('inactive', !data.active);
        setIcon(el.statusIcon, data.active ? 'check_circle' : 'error', 'outline');
    }
}

// Modules
let currentRenderId = 0;
async function loadModules() {
    const listContainer = document.getElementById('modules-list');
    if (!listContainer) return;
    const renderId = ++currentRenderId;

    try {
        const script = `
            ${NM_BIN} list json; echo "|||"
            cd ${MOD_DIR}
            for mod in *; do
                [ ! -d "$mod" ] || [ "$mod" = "nomount" ] || [ ! -f "$mod/module.prop" ] && continue
                has_injectable=0
                for p in system vendor product system_ext oem odm my_* tran_*; do
                    if [ -d "$mod/$p" ]; then has_injectable=1; break; fi
                done
                [ $has_injectable -eq 0 ] && continue
                echo "$mod|$(grep "^name=" "$mod/module.prop" | head -n1 | cut -d= -f2-)|$([ -f "$mod/disable" ] && echo true || echo false)|$([ -f "$mod/skip_mount" ] && echo true || echo false)"
            done
        `;

        const { stdout } = await exec(script);
        const [jsonPart, modulesPart] = stdout.split('|||');
        
        const activeRules = JSON.parse(jsonPart?.trim() || "[]");
        const lines = (modulesPart || '').split('\n').filter(Boolean);
        
        const ruleCountByMod = {};
        activeRules.forEach(r => {
            if (r?.real?.startsWith(MOD_DIR)) {
                const parts = r.real.split('/');
                if (parts[4] && parts[4] !== 'nomount') ruleCountByMod[parts[4]] = (ruleCountByMod[parts[4]] || 0) + 1;
            }
        });

        const htmlArr = lines.map(line => {
            const [modId, realName, disableStr, skipStr] = line.split('|');
            const hasDisable = disableStr === 'true', hasSkipMount = skipStr === 'true';
            const fileCount = ruleCountByMod[modId] || 0;
            const isLoaded = fileCount > 0;
            const statusKey = isLoaded ? (hasDisable ? 'status_loaded' : 'status_active') : (hasDisable ? 'status_disabled' : (hasSkipMount ? 'status_skipped' : 'status_inactive'));
            return `
                <div class="card module-card" data-module-id="${modId}">
                    <div class="module-header">
                        <div class="module-info">
                            <h3>${realName || modId}</h3>
                            <p>${translate('status_label')}: ${translate(statusKey)}</p>
                            <div class="file-count"><span>${translate('modules_injected_files', { count: fileCount })}</span></div>
                        </div>
                        <label class="custom-switch" id="switch-${modId}">
                            <input type="checkbox" class="switch-input" aria-label="Toggle module" ${!hasDisable ? 'checked' : ''}>
                            <span class="switch-track">
                                <span class="switch-thumb"></span>
                            </span>
                        </label>
                    </div>
                    <div class="module-divider"></div>
                    <div class="module-extension">
                        <button class="btn-hot-action ${isLoaded ? 'unload' : ''}" id="btn-hot-${modId}">
                            <span>${translate(isLoaded ? 'modules_hot_unload' : 'modules_hot_load')}</span>
                        </button>
                    </div>
                </div>
            `;
        });

        if (renderId === currentRenderId) {
            lines.length === 0 ? renderEmptyState(listContainer, '(._.)', translate('no_modules_found')) : listContainer.innerHTML = htmlArr.join('');
        }
    } catch (e) {
        renderTextState(listContainer, 'error-message', `Error: ${e.message}`);
    }
}

async function loadModule(modId) {
    const modPath = `${MOD_DIR}/${modId}`;

    const script = `
        cd "${modPath}" || exit 0
        find -L system vendor product system_ext odm oem \\( -type f -o -type l \\) -exec sh -c '
            mod="$1"; shift
            for f do
                printf "/%s\\0%s/%s\\0" "$f" "$mod" "$f"

                case "$f" in
                    vendor/*|product/*|system_ext/*|odm/*|oem/*)
                        [ ! -e "$mod/system/$f" ] && [ ! -L "$mod/system/$f" ] && printf "/system/%s\\0%s/%s\\0" "$f" "$mod" "$f" ;;
                    system/vendor/*|system/product/*|system/system_ext/*|system/odm/*|system/oem/*)
                        [ ! -e "$mod/\${f#system/}" ] && [ ! -L "$mod/\${f#system/}" ] && printf "/%s\\0%s/%s\\0" "\${f#system/}" "$mod" "$f" ;;
                esac
            done
        ' _ "${modPath}" {} + 2>/dev/null | xargs -0 -r -n 500 ${NM_BIN} add
    `;
    
    try { await exec(script); } catch (e) { throw e; }
}

async function unloadModule(modId) {
    try {
        const rules = JSON.parse((await exec(`${NM_BIN} list json`)).stdout || "[]");
        const targets = rules
            .filter(r => r?.real?.startsWith(`${MOD_DIR}/${modId}/`))
            .map(r => r.virtual);
        if (targets.length === 0) return;
        const cmdChunks = [];
        for (let i = 0; i < targets.length; i += 500) {
            const chunk = targets.slice(i, i + 500).join('\n');
            cmdChunks.push(`cat << 'EOF' | tr '\\n' '\\0' | xargs -0 -r -n 500 ${NM_BIN} del\n${chunk}\nEOF`);
        }
        await exec(cmdChunks.join('\n'));
    } catch (e) { throw e; }
}

// Apps & Exclusions
let allAppsCache = [], showSystemApps = false,
    appLoadingPromise = null, currentlyDisplayedApps = [],
    appListRenderIndex = 0, listObserver = null, filterTimeout, exclusionsLoadId = 0;

async function loadExclusions() {
    const listContainer = document.getElementById('exclusions-list');
    if (!listContainer) return;
    const loadId = ++exclusionsLoadId;

    try {
        const readResult = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        if (readResult.errno !== 0) throw new Error(readResult.stderr || 'Failed to read exclusions');

        const blockedUids = parseUidList(readResult.stdout);
        if (readResult.stdout !== serializeUidList(blockedUids)) {
            const migrationResult = await exec(buildWriteUidListCmd(blockedUids));
            if (migrationResult.errno !== 0) throw new Error(migrationResult.stderr || 'Failed to migrate exclusions');
        }

        if (blockedUids.length > 0) try { await ensureAppsCache(); } catch {}
        const appsMap = new Map(allAppsCache.map(app => [app.uid, app]));
        const htmlArr = blockedUids.map(uid => {
            const app = appsMap.get(uid);
            const label = app ? app.appLabel : `UID: ${uid}`;
            const pkg = app ? app.packageName : 'System/Unknown';
            return `
                <div class="card setting-item" data-uid="${uid}" data-label="${label}">
                    <div class="exclusion-app">
                        <img src="ksu://icon/${pkg}" class="app-icon-img" onerror="this.src='${APP_ICON_FALLBACK}'" />
                        <div class="setting-text"><h3>${label}</h3><p>${pkg}</p></div>
                    </div>
                    <md-icon-button class="btn-delete" aria-label="Remove exclusion">
                        <md-icon data-icon="delete" data-icon-variant="outline" aria-hidden="true">
                            <svg viewBox="0 -960 960 960" aria-hidden="true" style="width:100%; height:100%; fill:currentColor;">
                                <path d="${ICON_PATHS['delete']}"/>
                            </svg>
                        </md-icon>
                    </md-icon-button>
                </div>
            `;
        });

        requestAnimationFrame(() => {
            if (loadId !== exclusionsLoadId) return;
            blockedUids.length === 0 ? renderEmptyState(listContainer, '(._.)', translate('no_exclusions_yet')) : listContainer.innerHTML = htmlArr.join('');
        });
    } catch (e) {
        renderTextState(listContainer, 'error-message', translate('error_loading_exclusions'));
        showToast(translate('error_loading_exclusions'));
    }
}

async function ensureAppsCache(force = false) {
    if (!force && allAppsCache.length > 0) return;
    if (appLoadingPromise) {
        await appLoadingPromise;
        if (!force || allAppsCache.length > 0) return;
    }

    appLoadingPromise = (async () => {
        try {
            if (force) allAppsCache = [];
            let ksuWait = 10;
            while ((typeof ksu === 'undefined' || !ksu.listPackages) && ksuWait > 0) {
                await delay(200); ksuWait--;
            }
            if (typeof ksu === 'undefined' || !ksu.listPackages) throw new Error("KSU interface not ready");

            let pkgsRaw = ksu.listPackages("all");
            let retries = 6;
            while ((!pkgsRaw || pkgsRaw === '[]' || pkgsRaw === '') && retries > 0) {
                await delay(500);
                pkgsRaw = ksu.listPackages("all");
                retries--;
            }

            if (!pkgsRaw || pkgsRaw === '[]' || pkgsRaw === '') return;
            const pkgs = JSON.parse(pkgsRaw);
            const chunkSize = 200;
            const tempCache = [];

            for (let i = 0; i < pkgs.length; i += chunkSize) {
                const chunkInfo = ksu.getPackagesInfo(JSON.stringify(pkgs.slice(i, i + chunkSize)));
                if (chunkInfo) tempCache.push(...JSON.parse(chunkInfo));
                await delay(15); 
            }

            allAppsCache = tempCache.map(app => ({
                ...app, 
                appLabel: app.appLabel || app.packageName, 
                uid: String(app.uid),
                _search: (app.appLabel || app.packageName).toLowerCase() + app.packageName.toLowerCase()
            })).sort((a, b) => a.appLabel < b.appLabel ? -1 : (a.appLabel > b.appLabel ? 1 : 0));

        } catch (e) {
            console.error("Error in ensureAppsCache:", e);
        } finally { 
            appLoadingPromise = null; 
        }
    })();
    return appLoadingPromise;
}

function openAppSelector() {
    const modal = document.getElementById('app-selector-modal'), 
          container = document.getElementById('app-list-container'), 
          searchInput = document.getElementById('app-search-input');
          
    const switchWrapper = document.getElementById('switch-system-apps');
    const sysSwitch = switchWrapper ? (switchWrapper.tagName === 'INPUT' ? switchWrapper : switchWrapper.querySelector('input')) : null;
    if (!modal) return;

    modal.classList.add('active');
    if (listObserver) listObserver.disconnect();
    document.getElementById('filter-menu').classList.remove('active'); 
    searchInput.value = '';
    if (sysSwitch) sysSwitch.checked = showSystemApps;

    document.getElementById('btn-close-modal').onclick = () => { 
        modal.classList.remove('active'); 
        if (listObserver) listObserver.disconnect(); 
    };

    container.innerHTML = `<div class="loading-spinner">${translate('loading') || 'Loading apps...'}</div>`;
    listObserver = new IntersectionObserver((entries) => { 
        if (entries[0].isIntersecting) renderNextAppBatch(); 
    }, { root: container, rootMargin: '200px' });

    setTimeout(async () => {
        try {
            await ensureAppsCache();
            filterAndRender();
            searchInput.oninput = (e) => filterAndRender(e.target.value);
            document.getElementById('btn-filter-toggle').onclick = () => document.getElementById('filter-menu').classList.toggle('active');
            if (sysSwitch) sysSwitch.onchange = (e) => { showSystemApps = e.target.checked; filterAndRender(searchInput.value); };
        } catch (e) { 
            renderTextState(container, 'error-message', `${translate('load_failed') || 'Failed'}`); 
        }
    }, 250);
}

function filterAndRender(searchTerm = '') {
    return new Promise(resolve => {
        clearTimeout(filterTimeout);
        filterTimeout = setTimeout(() => {
            const term = searchTerm.toLowerCase();
            currentlyDisplayedApps = allAppsCache.filter(app => app._search.includes(term) && (showSystemApps || !app.isSystem));
            document.getElementById('app-list-container').replaceChildren();
            appListRenderIndex = 0;
            renderNextAppBatch();
            resolve();
        }, 200);
    });
}

function renderNextAppBatch() {
    const container = document.getElementById('app-list-container');
    const batch = currentlyDisplayedApps.slice(appListRenderIndex, appListRenderIndex + 50);

    if (batch.length === 0) {
        if (listObserver) listObserver.disconnect();
        if (appListRenderIndex === 0) renderEmptyState(container, '(._.)', translate('no_apps_found'));
        return;
    }

    const htmlStr = batch.map(app => `
        <div class="app-item segment-card" data-uid="${app.uid}" data-label="${app.appLabel}">
            <img src="ksu://icon/${app.packageName}" class="app-icon-img" loading="lazy" onerror="this.src='${APP_ICON_FALLBACK}'" />
            <div class="app-details"><div class="app-name">${app.appLabel}</div><div class="app-pkg">${app.packageName}</div></div>
            <div class="app-meta"><div class="uid-label">UID: ${app.uid}</div>${app.isSystem ? '<span class="system-chip">SYS</span>' : ''}</div>
        </div>
    `).join('');

    container.insertAdjacentHTML('beforeend', htmlStr);
    appListRenderIndex += batch.length;
    const lastEl = container.lastElementChild;
    if (lastEl && listObserver) listObserver.observe(lastEl);
}

async function removeExclusion(uid, name) {
    showToast(translate('unblocking_name', { name }));
    try {
        const uidStr = normalizeUidList([uid])[0];
        if (!uidStr) throw new Error('Invalid UID');

        const readResult = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        if (readResult.errno !== 0) throw new Error(readResult.stderr || 'Failed to read exclusions');

        const remainingUids = parseUidList(readResult.stdout).filter(value => value !== uidStr);
        const writeResult = await exec(buildWriteUidListCmd(remainingUids));
        if (writeResult.errno !== 0) throw new Error(writeResult.stderr || 'Failed to update exclusions');

        const unblockResult = await exec(`${NM_BIN} unblock ${uidStr}`);
        if (unblockResult.errno !== 0) throw new Error(unblockResult.stderr || 'Failed to unblock UID');
    } catch { showToast(translate('error_unblocking')); }
    await loadExclusions();
}

async function addExclusion(uid, name) {
    const uidStr = normalizeUidList([uid])[0];
    if (!uidStr) {
        showToast(translate('error_blocking'));
        return;
    }

    try {
        const readResult = await exec(`cat ${FILES.exclusions} 2>/dev/null || echo ""`);
        if (readResult.errno !== 0) throw new Error(readResult.stderr || 'Failed to read exclusions');

        const currentUids = parseUidList(readResult.stdout);
        const alreadyBlocked = currentUids.includes(uidStr);
        if (!alreadyBlocked) {
            const writeResult = await exec(buildWriteUidListCmd([...currentUids, uidStr]));
            if (writeResult.errno !== 0) throw new Error(writeResult.stderr || 'Failed to update exclusions');
        }

        const blockResult = await exec(`${NM_BIN} block ${uidStr}`);
        if (blockResult.errno !== 0) showToast(translate('blocked_saved'));
        else showToast(alreadyBlocked ? translate('blocked_already') : translate('blocked', { name }));
    } catch { showToast(translate('error_blocking')); }
    await loadExclusions();
}

// Options
async function loadOptions() {
    const swVerbose = document.querySelector('#setting-verbose input'),
          swSafe = document.querySelector('#setting-safemode input'),
          btnClear = document.getElementById('btn-clear-rules');

    if (swVerbose) { 
        swVerbose.checked = (await exec(`[ -f ${FILES.verbose} ] && echo yes`)).stdout.includes('yes');
        swVerbose.onchange = e => exec(e.target.checked ? `touch ${FILES.verbose}` : `rm ${FILES.verbose}`);
    }

    if (swSafe) {
        swSafe.checked = (await exec(`[ -f ${FILES.disable} ] && echo yes`)).stdout.includes('yes');
        swSafe.onchange = e => exec(e.target.checked ? `touch ${FILES.disable}` : `rm ${FILES.disable}`);
    }

    if (btnClear) {
        btnClear.onclick = async () => {
            showToast(translate('clear_rules_toast'));
            try {
                const persistResult = await exec(buildWriteUidListCmd([]));
                if (persistResult.errno !== 0) throw new Error(persistResult.stderr || 'Failed to clear exclusions');
                const clearResult = await exec(`${NM_BIN} clear`);
                if (clearResult.errno !== 0) throw new Error(clearResult.stderr || 'Failed to clear runtime rules');
                showToast(translate('clear_rules_done'));
                loadModules();
                loadExclusions();
            } catch { showToast(translate('save_failed')); }
        };
    }
}

let isGlobalLoading = false;

// Pull-to-Refresh
function attachPullToRefresh(containerSelector, indicatorSelector, threshold, refreshCallback, canRefresh = () => true) {
    const container = document.querySelector(containerSelector);
    const indicator = document.querySelector(indicatorSelector);
    const indicatorIcon = indicator?.querySelector('md-icon');
    if (!container || !indicator) return;

    let startY = 0, pullDist = 0;

    container.addEventListener('touchstart', (e) => {
        if (!isGlobalLoading && container.scrollTop <= 0 && canRefresh()) {
            startY = e.touches[0].pageY; 
            indicator.style.transition = 'none';
        } else {
            startY = 0;
        }
    }, { passive: true });

    container.addEventListener('touchmove', (e) => {
        if (startY === 0 || isGlobalLoading) return;
        pullDist = (e.touches[0].pageY - startY) * 0.4;
        
        if (pullDist > 0 && container.scrollTop <= 0) {
            if (e.cancelable) e.preventDefault(); 
            indicator.style.transform = `translate3d(0, ${Math.min(pullDist, threshold)}px, 0)`;
            indicator.style.opacity = Math.min(1, pullDist / threshold);
            if (indicatorIcon) indicatorIcon.style.transform = `rotate(${Math.min(180, (pullDist / threshold) * 180)}deg)`;
        }
    }, { passive: false });

    container.addEventListener('touchend', async () => {
        if (startY === 0 || isGlobalLoading) return;
        indicator.style.transition = 'all 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275)';

        if (pullDist >= threshold) {
            isGlobalLoading = true; 
            indicator.classList.add('refreshing');
            indicator.style.transform = `translate3d(0, ${threshold - 6}px, 0)`; 
            indicator.style.opacity = '1';

            try { 
                await refreshCallback(); 
                await new Promise(resolve => setTimeout(resolve, 400)); 
            } catch { 
                showToast(translate('refresh_failed')); 
            } finally { 
                resetIndicator(); 
            }
        } else {
            resetIndicator();
        }
        startY = pullDist = 0;
    });

    function resetIndicator() {
        isGlobalLoading = false; 
        indicator.classList.remove('refreshing');
        indicator.style.transform = 'translate3d(0, 0, 0)'; 
        indicator.style.opacity = '0';
        setTimeout(() => { if (indicatorIcon) indicatorIcon.style.transform = 'rotate(0deg)'; }, 300);
    }
}

async function refreshCurrentView() {
    const id = document.querySelector('.view-content.active')?.id;
    if (id === 'view-home') await loadHome();
    else if (id === 'view-modules') await loadModules();
    else if (id === 'view-exclusions') await loadExclusions();
    else if (id === 'view-options') await loadOptions();
}

function initDelegationAndAttach() {
     document.getElementById('modules-list')?.addEventListener('change', (e) => {
        const toggle = e.target.closest('.switch-input');
        if (toggle) {
            const labelContainer = toggle.closest('.custom-switch');
            if (labelContainer.dataset.busy) {
                e.preventDefault();
                return;
            }
            const modId = toggle.closest('.module-card').dataset.moduleId;
            labelContainer.dataset.busy = 'true';
            labelContainer.classList.add('switch-busy');
            const isChecked = toggle.checked;
            setTimeout(async () => {
                try {
                    if (isChecked) { await exec(`rm ${MOD_DIR}/${modId}/disable`); await loadModule(modId); }
                    else { await unloadModule(modId); await exec(`touch ${MOD_DIR}/${modId}/disable`); }
                } finally {
                    await new Promise(r => setTimeout(r, 200));
                    labelContainer.classList.remove('switch-busy');
                    delete labelContainer.dataset.busy;
                    loadModules();
                }
            }, 0);
        }
    });

    document.getElementById('modules-list')?.addEventListener('click', (e) => {
        const hotBtn = e.target.closest('.btn-hot-action');
        if (hotBtn && !hotBtn.disabled) {
            hotBtn.disabled = true;
            const card = hotBtn.closest('.module-card');
            const modId = card.dataset.moduleId;
            const isLoaded = hotBtn.classList.contains('unload');
            setTimeout(async () => {
                try { isLoaded ? await unloadModule(modId) : await loadModule(modId); } 
                finally { loadModules(); }
            }, 0);
        }
    });

    document.getElementById('exclusions-list')?.addEventListener('click', (e) => {
        const delBtn = e.target.closest('.btn-delete');
        if (delBtn) {
            const item = delBtn.closest('.setting-item');
            const uid = item.dataset.uid;
            const label = item.dataset.label;
            item.style.opacity = '0.5';
            item.style.pointerEvents = 'none';
            setTimeout(() => {
                removeExclusion(uid, label);
            }, 0);
        }
    });

    document.getElementById('app-list-container')?.addEventListener('click', (e) => {
        const item = e.target.closest('.app-item');
        if (item && !item.dataset.busy) {
            item.dataset.busy = 'true';
            const uid = item.dataset.uid;
            const label = item.dataset.label;
            if (listObserver) listObserver.disconnect();
            document.getElementById('app-selector-modal')?.classList.remove('active');
            setTimeout(async () => {
                await addExclusion(uid, label);
            }, 50);
        }
    });

    document.getElementById('app-selector-modal')?.addEventListener('click', (e) => {
        if (e.target === e.currentTarget) {
            e.currentTarget.classList.remove('active');
            if (listObserver) listObserver.disconnect(); 
        }
    });

    document.getElementById('fab-add-exclusion')?.addEventListener('click', openAppSelector);

    attachPullToRefresh('.page-container', '.pull-to-refresh-indicator', 90, async () => { await refreshCurrentView(); },
    () => {
        const activeId = document.querySelector('.view-content.active')?.id;
        return activeId === 'view-modules' || activeId === 'view-exclusions';
    });

    attachPullToRefresh('#app-list-container', '#app-modal-refresh-indicator', 70, async () => {
        document.getElementById('app-list-container').innerHTML = `<div class="loading-spinner">${translate('loading') || 'Refreshing...'}</div>`;
        await ensureAppsCache(true);
        await filterAndRender(document.getElementById('app-search-input')?.value || '');
    });
}

function initScrollListener() {
    const pageContainer = document.querySelector('.page-container');
    if (pageContainer) {
        let isTicking = false;
        pageContainer.addEventListener('scroll', () => {
            if (!isTicking) {
                window.requestAnimationFrame(() => {
                    updateTopAppBar();
                    isTicking = false;
                });
                isTicking = true;
            }
        }, { passive: true });
    }
}

// Init
document.addEventListener('DOMContentLoaded', async () => {
    await setAppLocale((localStorage.getItem('nm_locale') || navigator.language || 'en').split('-')[0]);
    applyIcons();
    syncSystemBarTheme();
    initNavigation();
    initDelegationAndAttach();
    initScrollListener();
    updateTopAppBar();
    viewLoadState['view-home'] = true;
    loadHome();
    document.body.classList.remove('loading');

    try {
        if (!viewLoadState['view-modules']) loadModules();
        if (!viewLoadState['view-exclusions']) loadExclusions();
        if (!viewLoadState['view-options']) loadOptions();
    } catch {}
});
