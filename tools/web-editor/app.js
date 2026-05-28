/* Omote OS web configurator v2 */
const API_BASE_KEY = 'omote_api_base';
let API = localStorage.getItem(API_BASE_KEY) || 'http://omote.local';
const EDITOR_PREFS_KEY = 'omote_editor_prefs';
const CONFIG_CACHE_KEY = 'omote_config_cache';
const IR_LIBRARY_CACHE_KEY = 'omote_ir_library_cache';
// OMOTE Rev1-4 reports the top power button as matrix key 'o'.
const POWER_KEY = 'o';
let deviceConnected = false;
const MAX_DEVICE_PAGES = 8;
const HA_DOMAINS = ['light', 'switch', 'cover', 'climate', 'media_player', 'fan', 'scene', 'script'];
/** Domains shown for physical-key HA actions (momentary services only). */
const KEY_HA_DOMAINS = ['light', 'switch', 'cover', 'media_player', 'fan', 'scene', 'script', 'button', 'input_boolean', 'lock'];

/** HA service for entity domain + widget (scenes/scripts use turn_on, not homeassistant.toggle). */
function defaultHaService(domain, widget) {
  const d = domain || 'light';
  if (widget === 'label') return 'turn_on';
  if (widget === 'climate' || widget === 'climate_thermostat') return 'set_temperature';
  if (d === 'scene' || d === 'script') return 'turn_on';
  if (d === 'button') return 'press';
  if (widget === 'toggle') return 'toggle';
  return 'toggle';
}

/** Service options for a physical key (momentary press). */
function haServicesForPhysicalKey(domain) {
  const d = domain || 'light';
  if (d === 'button') return [{ value: 'press', label: 'press' }];
  if (d === 'scene' || d === 'script') {
    return [
      { value: 'turn_on', label: 'turn_on' },
      { value: 'toggle', label: 'toggle' },
    ];
  }
  if (d === 'lock') {
    return [
      { value: 'toggle', label: 'toggle' },
      { value: 'unlock', label: 'unlock' },
      { value: 'lock', label: 'lock' },
    ];
  }
  if (d === 'cover') {
    return [
      { value: 'toggle', label: 'toggle' },
      { value: 'open_cover', label: 'open' },
      { value: 'close_cover', label: 'close' },
    ];
  }
  return [
    { value: 'toggle', label: 'toggle' },
    { value: 'turn_on', label: 'turn_on' },
    { value: 'turn_off', label: 'turn_off' },
  ];
}

function syncKeyHaServiceSelect(domain, selectedService) {
  const sel = $('key-ha-service');
  if (!sel) return;
  const opts = haServicesForPhysicalKey(domain);
  sel.innerHTML = opts.map((o) => `<option value="${o.value}">${o.label}</option>`).join('');
  const preferred = selectedService || defaultHaService(domain, 'push');
  const match = opts.find((o) => o.value === preferred);
  sel.value = match ? match.value : opts[0]?.value || 'toggle';
}

let activeKeyDomain = 'light';

const KEY_MATRIX = [
  ['s', '^', '-', 'm', 'e'],
  ['i', 'r', '+', 'k', 'd'],
  ['4', 'v', '1', '2', '3'],
  ['>', 'o', 'b', 'u', 'l'],
  ['?', 'p', 'c', '<', '='],
];
/* OMOTE Rev1-4 physical layout (see OMOTE keypad_keys_hal comments) */
const KEY_LABELS = {
  '=': 'Stop', '<': 'Rewind', p: 'Play', '>': 'Forward',
  c: 'Menu', i: 'Info', b: 'Back', s: 'Source',
  u: 'Up', d: 'Down', l: 'Left', r: 'Right', k: 'OK', o: 'Power',
  '+': 'Vol+', '-': 'Vol-', m: 'Mute', e: 'Record',
  '^': 'CH+', v: 'CH-',
  '1': 'Red', '2': 'Green', '3': 'Yellow', '4': 'Blue',
  '?': 'Help',
  P: 'Power',
};

/**
 * BLE keys aligned with Android KeyEvent names (see KeyEvent reference).
 * Firmware maps these to standard BLE keyboard + consumer HID.
 * Netflix/YouTube/etc. on the stock Google TV remote use vendor HID — generic BLE
 * Streaming shortcuts use KEYCODE_BUTTON_N (gamepad HID). Netflix → BUTTON_3 on Google TV.
 */
const BLE_KEY_CATALOG = [
  {
    group: 'Navigation (DPAD)',
    keys: [
      { id: 'UP', label: 'DPAD_UP' },
      { id: 'DOWN', label: 'DPAD_DOWN' },
      { id: 'LEFT', label: 'DPAD_LEFT' },
      { id: 'RIGHT', label: 'DPAD_RIGHT' },
      { id: 'ENTER', label: 'DPAD_CENTER / OK' },
      { id: 'BACK', label: 'BACK' },
      { id: 'HOME', label: 'HOME' },
      { id: 'MENU', label: 'MENU' },
      { id: 'SEARCH', label: 'SEARCH' },
      { id: 'APP_SWITCH', label: 'APP_SWITCH (recents)' },
    ],
  },
  {
    group: 'Volume & media',
    keys: [
      { id: 'VOLUME_UP', label: 'VOLUME_UP' },
      { id: 'VOLUME_DOWN', label: 'VOLUME_DOWN' },
      { id: 'MUTE', label: 'VOLUME_MUTE' },
      { id: 'PLAY_PAUSE', label: 'MEDIA_PLAY_PAUSE' },
      { id: 'PLAY', label: 'MEDIA_PLAY' },
      { id: 'PAUSE', label: 'MEDIA_PAUSE' },
      { id: 'STOP', label: 'MEDIA_STOP' },
      { id: 'NEXT', label: 'MEDIA_NEXT' },
      { id: 'PREVIOUS', label: 'MEDIA_PREVIOUS' },
      { id: 'FORWARD', label: 'MEDIA_FAST_FORWARD' },
      { id: 'REWIND', label: 'MEDIA_REWIND' },
    ],
  },
  {
    group: 'TV / live (best-effort on Google TV)',
    keys: [
      { id: 'CHANNEL_UP', label: 'CHANNEL_UP' },
      { id: 'CHANNEL_DOWN', label: 'CHANNEL_DOWN' },
      { id: 'GUIDE', label: 'GUIDE (EPG)' },
      { id: 'INFO', label: 'INFO' },
      { id: 'CAPTIONS', label: 'CAPTIONS' },
      { id: 'SETTINGS', label: 'SETTINGS' },
      { id: 'TV', label: 'TV' },
      { id: 'LIVE_TV', label: 'LIVE / LIVE_TV' },
      { id: 'TV_INPUT', label: 'TV_INPUT' },
      { id: 'DVR', label: 'DVR' },
      { id: 'NOTIFICATION', label: 'NOTIFICATION' },
      { id: 'PROFILE_SWITCH', label: 'PROFILE_SWITCH' },
      { id: 'POWER', label: 'POWER (sleep/wake)' },
      { id: 'TV_POWER', label: 'TV_POWER' },
      { id: 'SLEEP', label: 'SLEEP' },
      { id: 'PROG_RED', label: 'PROG_RED' },
      { id: 'PROG_GREEN', label: 'PROG_GREEN' },
      { id: 'PROG_YELLOW', label: 'PROG_YELLOW' },
      { id: 'PROG_BLUE', label: 'PROG_BLUE' },
      { id: 'MEDIA_AUDIO_TRACK', label: 'MEDIA_AUDIO_TRACK' },
    ],
  },
  {
    group: 'Assistant & editing',
    keys: [
      { id: 'ASSIST', label: 'ASSIST / voice (→ search)' },
      { id: 'VOICE_ASSIST', label: 'VOICE_ASSIST' },
      { id: 'TAB', label: 'TAB' },
      { id: 'PAGE_UP', label: 'PAGE_UP' },
      { id: 'PAGE_DOWN', label: 'PAGE_DOWN' },
      { id: 'BACKSPACE', label: 'BACK / DEL' },
      { id: 'DELETE', label: 'FORWARD_DEL' },
    ],
  },
  {
    group: 'Streaming apps (KEYCODE_BUTTON_N — gamepad)',
    keys: [
      { id: 'NETFLIX', label: 'Netflix → BUTTON_3' },
      { id: 'YOUTUBE', label: 'YouTube → BUTTON_4' },
      { id: 'PRIME_VIDEO', label: 'Prime → BUTTON_5' },
      { id: 'DISNEY_PLUS', label: 'Disney+ → BUTTON_6' },
      { id: 'SPOTIFY', label: 'Spotify → BUTTON_7' },
      { id: 'BUTTON_1', label: 'BUTTON_1' },
      { id: 'BUTTON_2', label: 'BUTTON_2' },
      { id: 'BUTTON_3', label: 'BUTTON_3 (Netflix on GTV)' },
      { id: 'BUTTON_4', label: 'BUTTON_4' },
      { id: 'BUTTON_5', label: 'BUTTON_5' },
      { id: 'BUTTON_6', label: 'BUTTON_6' },
      { id: 'BUTTON_7', label: 'BUTTON_7' },
      { id: 'BUTTON_8', label: 'BUTTON_8' },
    ],
  },
];

function buildBleKeySelectHtml() {
  return BLE_KEY_CATALOG.map((g) => {
    const opts = g.keys.map((k) => `<option value="${k.id}">${k.label}</option>`).join('');
    return `<optgroup label="${g.group}">${opts}</optgroup>`;
  }).join('');
}

function populateBleKeySelects() {
  const html = buildBleKeySelectHtml();
  if ($('key-ble-key')) $('key-ble-key').innerHTML = html;
  if ($('ble-key')) $('ble-key').innerHTML = html;
}

let learnPoll = null;
let learnActive = false;
let learnLastTs = 0;

let config = { schema_version: 2, active_page_id: 'home', pages: [], keymap: [] };
let selectedPageId = 'home';
let selectedButtonId = null;
let selectedKeyChar = null;
let activeDomain = 'light';
let dragState = null;
let irLearnTimer = null;
let irLibrary = [];
const GRID = 8;
const CONTENT_H = 320 - 22;
const haStateCache = new Map();
let haStatePollTimer = null;

function snap(n) {
  if (!$('btn-snap-grid')?.checked) return n;
  return Math.round(n / GRID) * GRID;
}

function hexFromColorInput(id, fallback) {
  const v = $(id)?.value;
  if (!v) return fallback;
  return parseInt(v.slice(1), 16);
}

function colorInputFromHex(id, hex) {
  const el = $(id);
  if (!el) return;
  el.value = '#' + Number(hex).toString(16).padStart(6, '0').slice(-6);
}

function mdiIconUrl(icon) {
  if (!icon) return null;
  const m = String(icon).match(/^mdi:(.+)$/);
  if (!m) return null;
  return `https://api.iconify.design/mdi/${m[1]}.svg?color=%23cccccc`;
}

function entityFriendlyName(opt) {
  if (!opt) return '';
  return opt.dataset.fname || opt.textContent.split('—')[0].trim();
}

function applyEntityToForm() {
  const sel = $('entity-pick');
  const opt = sel?.options[sel.selectedIndex];
  if (!opt?.value) return;
  if ($('btn-auto-label')?.checked) {
    $('btn-label').value = entityFriendlyName(opt);
  }
  const page = currentPage();
  const btn = page?.buttons?.find((b) => b.id === selectedButtonId);
  if (btn) {
    const ic = opt.dataset.icon;
    btn.ha_icon = ic && ic !== 'null' && ic !== 'undefined' ? ic : '';
  }
}

function readStyleFromForm(btn) {
  if (!btn) return;
  btn.style = btn.style || {};
  btn.style.bg = hexFromColorInput('btn-bg', 0x3366cc);
  btn.color = btn.style.bg;
  btn.style.fg = hexFromColorInput('btn-fg', 0xffffff);
  btn.style.radius = parseInt($('btn-radius').value, 10) || 8;
  btn.style.border = hexFromColorInput('btn-border', 0);
  btn.style.borderW = parseInt($('btn-border-w').value, 10) || 0;
  btn.label_below = !!$('btn-label-below')?.checked;
  const sel = $('entity-pick');
  const opt = sel?.options[sel.selectedIndex];
  const icon = opt?.dataset?.icon;
  if (icon && icon !== 'null' && icon !== 'undefined') btn.ha_icon = icon;
  else btn.ha_icon = '';
  const x = parseInt($('btn-x').value, 10);
  const y = parseInt($('btn-y').value, 10);
  const w = parseInt($('btn-w').value, 10);
  const h = parseInt($('btn-h').value, 10);
  if (!Number.isNaN(x)) btn.x = snap(Math.max(0, x));
  if (!Number.isNaN(y)) btn.y = snap(Math.max(0, y));
  if (!Number.isNaN(w)) btn.w = Math.max(20, snap(w));
  if (!Number.isNaN(h)) btn.h = Math.max(16, snap(h));
}

function writeStyleToForm(btn) {
  if (!btn) return;
  const st = btn.style || {};
  colorInputFromHex('btn-bg', st.bg || btn.color || 0x3366cc);
  colorInputFromHex('btn-fg', st.fg || 0xffffff);
  colorInputFromHex('btn-border', st.border || 0);
  $('btn-radius').value = st.radius ?? 8;
  $('btn-border-w').value = st.border_w ?? st.borderW ?? 0;
  $('btn-x').value = btn.x ?? 0;
  $('btn-y').value = btn.y ?? 0;
  $('btn-w').value = btn.w ?? 90;
  $('btn-h').value = btn.h ?? 44;
  if ($('btn-label-below')) $('btn-label-below').checked = !!btn.label_below;
}

function cycleEditorPage(dir) {
  if (!config.pages?.length) return;
  let idx = config.pages.findIndex((p) => p.id === selectedPageId);
  if (idx < 0) idx = 0;
  idx = (idx + dir + config.pages.length) % config.pages.length;
  selectedPageId = config.pages[idx].id;
  config.active_page_id = selectedPageId;
  selectedButtonId = null;
  renderPageList();
  drawCanvas();
  updateEditUi();
  $('editor-page-name').textContent = currentPage() ? '— ' + currentPage().name : '';
  refreshHaToggleStates();
}

async function refreshHaToggleStates() {
  const page = currentPage();
  if (!page?.buttons?.length) return;
  const haBtns = page.buttons.filter(
    (b) => b.action?.target?.entity_id &&
      (b.widget === 'toggle' || b.widget === 'label' || b.widget === 'climate' || b.widget === 'climate_thermostat')
  );
  if (!haBtns.length) return;
  for (const b of haBtns.slice(0, 8)) {
    const eid = b.action.target.entity_id;
    try {
      const data = await api('/api/ha/entity?entity_id=' + encodeURIComponent(eid));
      haStateCache.set(eid, data.on ? 'on' : String(data.state || ''));
    } catch (_) {
      haStateCache.delete(eid);
    }
  }
  drawCanvas();
}

function startHaStatePoll() {
  if (haStatePollTimer) clearInterval(haStatePollTimer);
  haStatePollTimer = setInterval(() => {
    if (!$('tab-layout')?.classList.contains('active')) return;
    refreshHaToggleStates();
  }, 15000);
}

async function api(path, opts = {}) {
  const base = (API || '').replace(/\/+$/, '');
  const r = await fetch(base + path, {
    headers: { 'Content-Type': 'application/json', ...(opts.headers || {}) },
    ...opts,
  });
  const text = await r.text();
  let data = null;
  try { data = JSON.parse(text); } catch { data = text; }
  if (!r.ok) throw new Error(data?.error || (typeof data === 'string' ? data : r.statusText));
  return data;
}

function applyApiBaseFromUi() {
  const v = $('device-url')?.value?.trim();
  if (!v) return;
  API = v.replace(/\/+$/, '');
  localStorage.setItem(API_BASE_KEY, API);
}

function loadEditorPrefs() {
  try {
    return JSON.parse(localStorage.getItem(EDITOR_PREFS_KEY) || '{}') || {};
  } catch (_) {
    return {};
  }
}

function saveEditorPrefs(partial) {
  const cur = loadEditorPrefs();
  const next = { ...cur, ...partial };
  localStorage.setItem(EDITOR_PREFS_KEY, JSON.stringify(next));
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function configButtonCount(cfg = config) {
  return (cfg?.pages || []).reduce((n, p) => n + (p.buttons?.length || 0), 0);
}

function configButtonCountFromData(data) {
  const cfg = configFromBackup(data);
  return cfg ? configButtonCount(cfg) : 0;
}

function cacheConfig() {
  try {
    localStorage.setItem(CONFIG_CACHE_KEY, JSON.stringify({
      savedAt: Date.now(),
      config,
    }));
  } catch (_) {}
}

function restoreConfigCache() {
  try {
    const raw = localStorage.getItem(CONFIG_CACHE_KEY);
    if (!raw) return false;
    const data = JSON.parse(raw);
    if (!data?.config?.pages?.length) return false;
    config = data.config;
    if (!config.pages?.length) config.pages = [];
    selectedPageId = config.active_page_id || config.pages[0]?.id || 'home';
    renderPageList();
    drawCanvas();
    updateEditUi();
    $('editor-page-name').textContent = currentPage() ? '— ' + currentPage().name : '';
    renderRemoteKeymap();
    return true;
  } catch (_) {
    return false;
  }
}

function applyConfigFromDevice() {
  if (!config.pages?.length) config.pages = [];
  if (!config.keymap) config.keymap = [];
  selectedPageId = config.active_page_id || config.pages[0]?.id || 'home';
  renderPageList();
  drawCanvas();
  updateEditUi();
  fillNavPages();
  $('editor-page-name').textContent = currentPage() ? '— ' + currentPage().name : '';
  renderRemoteKeymap();
}

function unwrapMaybeJson(val) {
  if (val == null) return null;
  if (typeof val === 'string') {
    try {
      return JSON.parse(val);
    } catch {
      return null;
    }
  }
  return typeof val === 'object' ? val : null;
}

/** Close truncated JSON (e.g. download cut off mid-keymap). */
function repairTruncatedJson(text) {
  let t = String(text).trim();
  if (t.startsWith('"') && t.endsWith('"')) {
    try {
      return JSON.parse(t);
    } catch (_) {}
  }
  t = t.replace(/,\s*"[^"]*":\s*\{[^}]*$/, '');
  t = t.replace(/,\s*"[^"]*":\s*"[^"]*$/, '');
  const stack = [];
  let inStr = false;
  let esc = false;
  for (let i = 0; i < t.length; i++) {
    const c = t[i];
    if (inStr) {
      if (esc) esc = false;
      else if (c === '\\') esc = true;
      else if (c === '"') inStr = false;
      continue;
    }
    if (c === '"') {
      inStr = true;
      continue;
    }
    if (c === '{' || c === '[') stack.push(c === '{' ? '}' : ']');
    if (c === '}' || c === ']') stack.pop();
  }
  if (inStr) t += '"';
  while (stack.length) t += stack.pop();
  return JSON.parse(t);
}

/**
 * Parse backup file text: plain object, double-encoded JSON string, or truncated export.
 * @returns {{ data: object, repaired: boolean }}
 */
function parseBackupText(text) {
  let data = JSON.parse(text.trim());
  let repaired = false;
  for (let i = 0; i < 4 && typeof data === 'string'; i++) {
    try {
      data = JSON.parse(data);
    } catch {
      data = repairTruncatedJson(data);
      repaired = true;
      break;
    }
  }
  if (Array.isArray(data) && data.length && data.every((x) => typeof x === 'string')) {
    data = JSON.parse(data.join(''));
  }
  if (data && typeof data === 'object' && !Array.isArray(data)) return { data, repaired };
  throw new Error('Backup must be a JSON object (or a JSON string containing one).');
}

function normalizeBackupRoot(data) {
  if (typeof data === 'string') {
    try {
      return normalizeBackupRoot(JSON.parse(data));
    } catch {
      try {
        return normalizeBackupRoot(repairTruncatedJson(data));
      } catch {
        return data;
      }
    }
  }
  return data;
}

/** Find config object in common backup / cache / export shapes. */
function configFromBackup(data) {
  data = normalizeBackupRoot(data);
  const roots = [data, unwrapMaybeJson(data?.config), data?.config, data?.layout];
  const seen = new Set();
  for (const root of roots) {
    if (!root || typeof root !== 'object' || seen.has(root)) continue;
    seen.add(root);
    if (Array.isArray(root.pages)) {
      return normalizeConfigShape(root);
    }
    const inner = unwrapMaybeJson(root.config);
    if (inner && Array.isArray(inner.pages)) {
      return normalizeConfigShape(inner);
    }
  }
  return null;
}

function normalizeConfigShape(cfg) {
  const out = { ...cfg };
  if (!Array.isArray(out.pages)) out.pages = [];
  if (!Array.isArray(out.keymap)) out.keymap = [];
  if (!out.schema_version) out.schema_version = 2;
  if (!out.active_page_id && out.pages[0]?.id) out.active_page_id = out.pages[0].id;
  return out;
}

function irLibraryFromBackup(data) {
  if (!data || typeof data !== 'object') return null;
  if (Array.isArray(data.ir_library)) return data.ir_library;
  const ir = data.ir_library;
  if (ir && Array.isArray(ir.entries)) return ir.entries;
  if (Array.isArray(data.entries)) return data.entries;
  return null;
}

function backupHasImportableContent(data) {
  data = normalizeBackupRoot(data);
  if (!data || typeof data !== 'object' || Array.isArray(data)) return false;
  if (configFromBackup(data)) return true;
  if (data.ha && typeof data.ha === 'object') return true;
  if (data.wifi && typeof data.wifi === 'object') return true;
  if (data.device_settings && typeof data.device_settings === 'object') return true;
  if (irLibraryFromBackup(data)) return true;
  if (data.omote_backup_version != null) return true;
  if (data.config && typeof data.config === 'object') return true;
  return false;
}

/** Rebuild a clean omote backup JSON for the device API. */
function buildBackupPayload(data) {
  data = normalizeBackupRoot(data);
  const out = { omote_backup_version: data.omote_backup_version ?? 1 };
  const cfg = configFromBackup(data);
  if (cfg) out.config = cfg;
  if (data.ha && typeof data.ha === 'object') out.ha = data.ha;
  if (data.wifi && typeof data.wifi === 'object') out.wifi = data.wifi;
  if (data.device_settings && typeof data.device_settings === 'object') {
    out.device_settings = data.device_settings;
  }
  const ir = irLibraryFromBackup(data);
  if (ir) out.ir_library = ir;
  return JSON.stringify(out);
}

function applyBackupToEditor(data) {
  data = normalizeBackupRoot(data);
  const cfg = configFromBackup(data);
  if (cfg) {
    config = cfg;
    if (!config.keymap) config.keymap = [];
    applyConfigFromDevice();
    cacheConfig();
  }
  if (data.ha?.ha_url) $('ha-url').value = data.ha.ha_url;
  if (data.ha?.ha_token) $('ha-token').value = data.ha.ha_token;
  saveEditorPrefs({
    ha_url: $('ha-url').value.trim(),
    ha_token: $('ha-token').value.trim() || loadEditorPrefs().ha_token || '',
  });
  const ir = irLibraryFromBackup(data);
  if (ir) {
    irLibrary = ir;
    cacheIrLibrary();
    populateIrSelects();
    renderIrLibraryList();
  }
}

async function reloadFromDeviceAfterImport() {
  const timeoutMs = 20000;
  await Promise.race([
    (async () => {
      await loadSettings();
      await loadConfig({ allowCacheFallback: false });
      await loadIrLibrary();
      await loadDeviceSettings();
      await refreshStatus();
    })(),
    sleep(timeoutMs).then(() => {
      throw new Error(`device reload timed out after ${timeoutMs / 1000}s`);
    }),
  ]);
}

function getGlobalKeyBinding(ch) {
  return config.keymap?.find((kb) => kb.key === ch);
}

function getGlobalKeyAction(ch) {
  return getGlobalKeyBinding(ch)?.action;
}

function setGlobalKeyAction(ch, action, pageId = '') {
  if (!config.keymap) config.keymap = [];
  const idx = config.keymap.findIndex((kb) => kb.key === ch);
  const entry = { key: ch, page_id: pageId, action };
  if (idx >= 0) config.keymap[idx] = entry;
  else config.keymap.push(entry);
}

function getKeyAction(ch) {
  if (ch === POWER_KEY) return getGlobalKeyAction(ch);
  return currentPage()?.keys?.[ch];
}

function cacheIrLibrary() {
  try {
    localStorage.setItem(IR_LIBRARY_CACHE_KEY, JSON.stringify({ savedAt: Date.now(), entries: irLibrary }));
  } catch (_) {}
}

function restoreIrLibraryCache() {
  try {
    const raw = localStorage.getItem(IR_LIBRARY_CACHE_KEY);
    if (!raw) return false;
    const data = JSON.parse(raw);
    if (!Array.isArray(data?.entries)) return false;
    irLibrary = data.entries;
    populateIrSelects();
    renderIrLibraryList();
    return true;
  } catch (_) {
    return false;
  }
}

function populateIrSelects() {
  const options = ['<option value="">(select IR code)</option>']
    .concat(
      irLibrary.map((e) => {
        const label = `${e.name || e.id} (${e.protocol} ${e.code})`;
        return `<option value="${e.id}">${label.replace(/"/g, '&quot;')}</option>`;
      })
    )
    .join('');
  ['ir-lib-select', 'key-ir-lib'].forEach((id) => {
    const sel = $(id);
    if (!sel) return;
    const prev = sel.value;
    sel.innerHTML = options;
    if (prev) sel.value = prev;
  });
}

function renderIrLibraryList() {
  const list = $('ir-library-list');
  if (!list) return;
  list.innerHTML = '';
  if (!irLibrary.length) {
    list.innerHTML = '<li class="muted">No IR codes yet — learn one above.</li>';
    return;
  }
  irLibrary.forEach((e) => {
    const li = document.createElement('li');
    li.className = 'ir-library-item';
    li.innerHTML = `
      <div class="ir-lib-meta">
        <strong>${e.name || e.id}</strong>
        <span class="muted">${e.protocol} ${e.code}</span>
      </div>
      <div class="ir-lib-actions">
        <button type="button" data-act="layout" data-id="${e.id}">Add touch button</button>
        <button type="button" data-act="key" data-id="${e.id}">Assign to key…</button>
        <button type="button" data-act="delete" data-id="${e.id}">Delete</button>
      </div>`;
    list.appendChild(li);
  });
  list.querySelectorAll('button[data-act]').forEach((btn) => {
    btn.onclick = () => handleIrLibraryAction(btn.dataset.act, btn.dataset.id);
  });
}

async function handleIrLibraryAction(act, id) {
  const entry = irLibrary.find((e) => e.id === id);
  if (!entry) return;
  if (act === 'delete') {
    if (!confirm(`Delete "${entry.name || entry.id}"?`)) return;
    await api('/api/ir/library/delete', { method: 'POST', body: JSON.stringify({ id }) });
    await loadIrLibrary();
    return;
  }
  if (act === 'layout') {
    showTab('layout');
    $('btn-label').value = entry.name || 'IR';
    $('action-type').value = 'ir';
    $('btn-widget').value = 'push';
    syncActionPanels();
    if ($('ir-lib-select')) $('ir-lib-select').value = id;
    $('editor-msg').textContent = `Ready — place "${entry.name}" and click Add new or Update.`;
    return;
  }
  if (act === 'key') {
    showTab('keys');
    $('key-action-type').value = 'ir';
    $('key-action-type').dispatchEvent(new Event('change'));
    if ($('key-ir-lib')) $('key-ir-lib').value = id;
    $('learn-key-status').textContent = 'Pick a physical key (or Power above the screen), then Assign.';
  }
}

async function startIrLearn(onCaptured) {
  await api('/api/ir/learn/start', { method: 'POST' });
  return new Promise((resolve, reject) => {
    let tries = 0;
    const timer = setInterval(async () => {
      tries++;
      try {
        const r = await api('/api/ir/learn/poll');
        if (r.ok) {
          clearInterval(timer);
          await api('/api/ir/learn/stop', { method: 'POST' });
          resolve(r);
          if (onCaptured) onCaptured(r);
          return;
        }
      } catch (e) {
        clearInterval(timer);
        await api('/api/ir/learn/stop', { method: 'POST' }).catch(() => {});
        reject(e);
        return;
      }
      if (tries > 120) {
        clearInterval(timer);
        api('/api/ir/learn/stop', { method: 'POST' }).catch(() => {});
        reject(new Error('Timed out — no IR signal detected'));
      }
    }, 400);
  });
}

async function saveIrCaptureToLibrary(capture, defaultName) {
  const name = prompt('Name for this IR code:', defaultName || 'TV Power');
  if (!name) return null;
  const r = await api('/api/ir/library', {
    method: 'POST',
    body: JSON.stringify({
      name,
      protocol: capture.protocol || 'NEC',
      code: capture.code || '0',
      bits: capture.bits || 32,
    }),
  });
  await loadIrLibrary();
  return r;
}

function $(id) { return document.getElementById(id); }

function showTab(name) {
  document.querySelectorAll('.tab').forEach((t) => t.classList.remove('active'));
  document.querySelectorAll('#nav button').forEach((b) => b.classList.remove('active'));
  $(`tab-${name}`).classList.add('active');
  document.querySelector(`#nav button[data-tab="${name}"]`).classList.add('active');
}

function currentPage() {
  return config.pages.find((p) => p.id === selectedPageId);
}

function btnColor(b) {
  if (b.style?.bg) return '#' + Number(b.style.bg).toString(16).padStart(6, '0').slice(-6);
  if (b.color) return '#' + Number(b.color).toString(16).padStart(6, '0').slice(-6);
  return '#3366cc';
}

function updateEditUi() {
  const has = !!selectedButtonId;
  $('btn-update-button').disabled = !has;
  $('btn-delete-button').disabled = !has;
  $('editing-label').textContent = has
    ? `Editing: ${currentPage()?.buttons?.find((b) => b.id === selectedButtonId)?.label || selectedButtonId}`
    : 'Click a button to edit, or Add new';
}

function actionFromUi() {
  const type = $('action-type').value;
  const widget = $('btn-widget').value;
  if (type === 'ha_service' || widget === 'toggle' || widget === 'label' || widget === 'climate' || widget === 'climate_thermostat') {
    const ent = $('entity-pick').value;
    if (!ent) throw new Error('Select an entity first');
    const domain = ent.split('.')[0] || 'light';
    const service =
      widget === 'toggle' || widget === 'label' || widget === 'climate' || widget === 'climate_thermostat'
        ? defaultHaService(domain, widget)
        : $('ha-service').value || defaultHaService(domain, widget);
    return { type: 'ha_service', domain, service, target: { entity_id: ent } };
  }
  if (type === 'ble_key' || type === 'ble_media') return { type, key: $('ble-key').value };
  if (type === 'ir') {
    const irId = $('ir-lib-select')?.value || '';
    if (irId) {
      const entry = irLibrary.find((e) => e.id === irId);
      return {
        type: 'ir',
        ir_id: irId,
        protocol: entry?.protocol || 'NEC',
        code: entry?.code || '0',
        bits: entry?.bits || 32,
      };
    }
    return { type: 'ir', protocol: $('ir-protocol').value || 'NEC', code: $('ir-code').value || '0', bits: 32 };
  }
  if (type === 'navigate_page') return { type: 'navigate_page', page_id: $('nav-page').value };
  if (type === 'open_keyboard') return { type: 'open_keyboard' };
  return { type: 'ha_service', domain: 'homeassistant', service: 'restart' };
}

function syncActionPanels() {
  const widget = $('btn-widget').value;
  const haWidget = widget === 'toggle' || widget === 'label' || widget === 'climate' || widget === 'climate_thermostat';
  if (haWidget) $('action-type').value = 'ha_service';
  const t = $('action-type').value;
  $('action-ha').classList.toggle('hidden', t !== 'ha_service' && !haWidget);
  $('action-ble').classList.toggle('hidden', t !== 'ble_key' && t !== 'ble_media');
  $('action-ir').classList.toggle('hidden', t !== 'ir');
  $('action-nav').classList.toggle('hidden', t !== 'navigate_page');
  if (t === 'open_keyboard') {
    $('action-ha').classList.add('hidden');
    $('action-ble').classList.add('hidden');
    $('action-ir').classList.add('hidden');
    $('action-nav').classList.add('hidden');
  }
  const hideService =
    widget === 'toggle' || widget === 'label' || widget === 'climate' || widget === 'climate_thermostat';
  if ($('ha-service')) {
    $('ha-service').closest('label').style.display = hideService ? 'none' : '';
  }
}

function setHaServiceSelect(service) {
  const sel = $('ha-service');
  if (!sel) return;
  const svc = service || 'toggle';
  if (!sel.querySelector(`option[value="${svc}"]`)) {
    const o = document.createElement('option');
    o.value = svc;
    o.textContent = svc;
    sel.appendChild(o);
  }
  sel.value = svc;
}

function fillNavPages() {
  ['nav-page', 'key-nav-page'].forEach((id) => {
    const sel = $(id);
    if (!sel) return;
    sel.innerHTML = '';
    config.pages.forEach((p) => {
      const o = document.createElement('option');
      o.value = p.id;
      o.textContent = p.name;
      sel.appendChild(o);
    });
  });
  const kp = $('keymap-page');
  if (kp) {
    kp.innerHTML = '';
    config.pages.forEach((p) => {
      const o = document.createElement('option');
      o.value = p.id;
      o.textContent = p.name;
      if (p.id === selectedPageId) o.selected = true;
      kp.appendChild(o);
    });
  }
}

function selectPage(pageId) {
  selectedPageId = pageId;
  config.active_page_id = pageId;
  const p = config.pages.find((x) => x.id === pageId);
  renderPageList();
  drawCanvas();
  $('editor-page-name').textContent = p ? '— ' + p.name : '';
  $('keymap-page-name').textContent = p?.name || '';
  if ($('keymap-page')) $('keymap-page').value = pageId;
  renderRemoteKeymap();
  refreshHaToggleStates();
}

function renderPageList() {
  const ul = $('page-list');
  ul.innerHTML = '';
  config.pages.forEach((p) => {
    const li = document.createElement('li');
    if (p.id === selectedPageId) li.classList.add('active');
    const nameIn = document.createElement('input');
    nameIn.type = 'text';
    nameIn.className = 'page-name-input';
    nameIn.value = p.name || p.id;
    nameIn.onclick = (ev) => ev.stopPropagation();
    nameIn.onchange = () => {
      p.name = nameIn.value.trim() || p.id;
      if (p.id === selectedPageId) $('editor-page-name').textContent = '— ' + p.name;
      fillNavPages();
    };
    const meta = document.createElement('div');
    meta.className = 'page-meta';
    const keys = p.keys ? Object.keys(p.keys).length : 0;
    meta.textContent = `${p.buttons?.length || 0} widgets · ${keys} keys`;
    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'page-del';
    del.textContent = 'Delete page';
    del.onclick = (ev) => {
      ev.stopPropagation();
      if (config.pages.length <= 1) {
        alert('Keep at least one page.');
        return;
      }
      if (!confirm(`Delete page "${p.name}"?`)) return;
      config.pages = config.pages.filter((x) => x.id !== p.id);
      if (selectedPageId === p.id) selectedPageId = config.pages[0]?.id || '';
      selectPage(selectedPageId);
    };
    li.appendChild(nameIn);
    li.appendChild(meta);
    li.appendChild(del);
    li.onclick = () => selectPage(p.id);
    ul.appendChild(li);
  });
  fillNavPages();
}

function drawCanvas() {
  const c = $('preview');
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#111';
  ctx.fillRect(0, 0, 240, 320);
  ctx.fillStyle = '#1a1a1a';
  ctx.fillRect(0, 0, 240, 22);
  ctx.fillStyle = '#888';
  ctx.font = '10px sans-serif';
  const page = currentPage();
  ctx.fillText(page?.name || 'status bar', 8, 14);
  if ($('btn-show-grid')?.checked) {
    ctx.strokeStyle = '#222';
    ctx.lineWidth = 1;
    for (let x = 0; x <= 240; x += GRID) {
      ctx.beginPath();
      ctx.moveTo(x, 22);
      ctx.lineTo(x, 320);
      ctx.stroke();
    }
    for (let y = 22; y <= 320; y += GRID) {
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(240, y);
      ctx.stroke();
    }
  }
  if (!page?.buttons) return;
  const thermo = page.buttons.find((b) => b.widget === 'climate_thermostat');
  if (thermo) {
    const eid = thermo.action?.target?.entity_id;
    const st = eid ? haStateCache.get(eid) : '';
    ctx.fillStyle = '#2a1a3a';
    ctx.fillRect(0, 22, 240, 298);
    ctx.strokeStyle = '#664422';
    ctx.lineWidth = 10;
    ctx.beginPath();
    ctx.arc(120, 120, 70, Math.PI * 0.75, Math.PI * 2.25);
    ctx.stroke();
    ctx.fillStyle = '#ddd';
    ctx.font = '11px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(String(st || 'HVAC').slice(0, 12) + ' (tap)', 120, 88);
    ctx.font = 'bold 16px sans-serif';
    ctx.fillStyle = '#ffaa66';
    ctx.fillText('69°', 70, 108);
    ctx.fillStyle = '#66aaff';
    ctx.fillText('76°', 170, 108);
    ctx.font = '12px sans-serif';
    ctx.fillStyle = '#bbb';
    ctx.fillText('Room temp', 120, 128);
    ctx.strokeStyle = '#ff8844';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(88, 290, 14, 0, Math.PI * 2);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(152, 290, 14, 0, Math.PI * 2);
    ctx.stroke();
    ctx.fillStyle = '#ddd';
    ctx.fillText('−', 88, 294);
    ctx.fillText('+', 152, 294);
    ctx.textAlign = 'left';
    return;
  }
  page.buttons.forEach((b) => {
    const y = b.y + 22;
    const r = b.style?.radius ?? 8;
    const fg = b.style?.fg != null ? '#' + Number(b.style.fg).toString(16).padStart(6, '0').slice(-6) : '#fff';
    const iconUrl = mdiIconUrl(b.ha_icon);
    if (b.widget === 'climate') {
      ctx.fillStyle = btnColor(b);
      roundRect(ctx, b.x, y, b.w, b.h, r);
      ctx.fill();
      ctx.fillStyle = fg;
      ctx.font = '10px sans-serif';
      ctx.fillText(b.label || 'Climate', b.x + 4, y + 12);
      ctx.fillText('± temp  Off Heat Cool Auto', b.x + 4, y + 28);
    } else if (b.widget === 'label') {
      ctx.fillStyle = btnColor(b);
      roundRect(ctx, b.x, y, b.w, b.h, r);
      ctx.fill();
      ctx.fillStyle = fg;
      ctx.font = '11px sans-serif';
      const stateTxt = b.action?.target?.entity_id ? (haStateCache.get(b.action.target.entity_id) || '…') : 'text';
      const capY = b.label_below ? y + b.h - 6 : y + 12;
      const valY = b.label_below ? y + 14 : y + 26;
      if (b.label && !b.label_below) ctx.fillText(b.label.slice(0, 14), b.x + 4, y + 12);
      ctx.fillText(String(stateTxt).slice(0, 16), b.x + 4, b.label_below ? y + 14 : valY);
      if (b.label && b.label_below) ctx.fillText(b.label.slice(0, 14), b.x + 4, capY);
    } else if (b.widget === 'toggle') {
      const eid = b.action?.target?.entity_id;
      const on = eid && haStateCache.get(eid) === 'on';
      if (iconUrl) {
        // icon hint left of switch
        ctx.fillStyle = '#888';
        ctx.fillRect(b.x, y + 8, 16, 16);
      }
      ctx.fillStyle = on ? '#95d5b2' : '#666';
      ctx.fillRect(b.x + (iconUrl ? 20 : 4), y + 10, 18, 10);
      ctx.fillStyle = on ? '#2d6a4f' : '#444';
      ctx.fillRect(b.x + (iconUrl ? 20 : 4) + (on ? 8 : 0), y + 11, 8, 8);
      ctx.fillStyle = fg;
      ctx.font = '11px sans-serif';
      const ty = b.label_below ? y + 28 : y + 18;
      if (b.label) ctx.fillText(b.label.slice(0, 18), b.x + (iconUrl ? 42 : 26), ty);
      if (b.label_below && b.label) ctx.fillText(b.label.slice(0, 18), b.x + 4, y + 14);
    } else {
      ctx.fillStyle = btnColor(b);
      roundRect(ctx, b.x, y, b.w, b.h, r);
      ctx.fill();
      if ((b.style?.border_w ?? b.style?.borderW ?? 0) > 0) {
        ctx.strokeStyle = b.style?.border != null
          ? '#' + Number(b.style.border).toString(16).padStart(6, '0').slice(-6)
          : '#000';
        ctx.lineWidth = b.style.border_w ?? b.style.borderW ?? 1;
        roundRect(ctx, b.x, y, b.w, b.h, r);
        ctx.stroke();
      }
      ctx.fillStyle = fg;
      ctx.font = '11px sans-serif';
      ctx.fillText(b.label || '', b.x + 4, y + Math.min(16, b.h - 4));
    }
    ctx.strokeStyle = b.id === selectedButtonId ? '#fff' : '#444';
    ctx.lineWidth = b.id === selectedButtonId ? 2 : 1;
    ctx.strokeRect(b.x, y, b.w, b.h);
  });
}

function roundRect(ctx, x, y, w, h, r) {
  const rad = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + rad, y);
  ctx.lineTo(x + w - rad, y);
  ctx.quadraticCurveTo(x + w, y, x + w, y + rad);
  ctx.lineTo(x + w, y + h - rad);
  ctx.quadraticCurveTo(x + w, y + h, x + w - rad, y + h);
  ctx.lineTo(x + rad, y + h);
  ctx.quadraticCurveTo(x, y + h, x, y + h - rad);
  ctx.lineTo(x, y + rad);
  ctx.quadraticCurveTo(x, y, x + rad, y);
  ctx.closePath();
}

function canvasCoords(ev) {
  const rect = $('preview').getBoundingClientRect();
  const mx = Math.floor((ev.clientX - rect.left) * (240 / rect.width));
  const my = Math.floor((ev.clientY - rect.top) * (320 / rect.height));
  return { mx, my };
}

function canvasHit(mx, my) {
  const page = currentPage();
  if (!page?.buttons || my < 22) return null;
  const cy = my - 22;
  for (let i = page.buttons.length - 1; i >= 0; i--) {
    const b = page.buttons[i];
    if (mx >= b.x && mx <= b.x + b.w && cy >= b.y && cy <= b.y + b.h) return b;
  }
  return null;
}

function loadButtonToForm(btn) {
  if (!btn) return;
  $('btn-label').value = btn.label || '';
  $('btn-widget').value = btn.widget || 'push';
  writeStyleToForm(btn);
  const a = btn.action || {};
  $('action-type').value = a.type || 'ha_service';
  syncActionPanels();
  if (a.type === 'ha_service' && a.target?.entity_id) {
    $('entity-pick').value = a.target.entity_id;
    const dom = a.target.entity_id.split('.')[0];
    if (dom && HA_DOMAINS.includes(dom)) {
      activeDomain = dom;
      initDomainTabs();
    }
    setHaServiceSelect(a.service);
  }
  if (a.type === 'ble_key' || a.type === 'ble_media') $('ble-key').value = a.key || 'UP';
  if (a.type === 'ir') {
    if ($('ir-lib-select')) $('ir-lib-select').value = a.ir_id || '';
    $('ir-protocol').value = a.protocol || 'NEC';
    $('ir-code').value = a.code || '';
  }
  if (a.type === 'navigate_page') $('nav-page').value = a.page_id || '';
  updateEditUi();
}

async function loadIrLibrary() {
  try {
    irLibrary = await api('/api/ir/library');
    if (!Array.isArray(irLibrary)) irLibrary = [];
    cacheIrLibrary();
    populateIrSelects();
    renderIrLibraryList();
  } catch (_) {
    restoreIrLibraryCache();
  }
}

function initDomainTabsFor(containerId, domains, activeDomainRef, onSelect) {
  const el = $(containerId);
  if (!el) return;
  el.innerHTML = '';
  domains.forEach((d) => {
    const b = document.createElement('button');
    b.textContent = d;
    if (d === activeDomainRef.value) b.classList.add('active');
    b.onclick = () => {
      activeDomainRef.value = d;
      el.querySelectorAll('button').forEach((x) => x.classList.remove('active'));
      b.classList.add('active');
      onSelect();
    };
    el.appendChild(b);
  });
}

function initDomainTabs() {
  initDomainTabsFor('domain-tabs', HA_DOMAINS, { get value() { return activeDomain; }, set value(v) { activeDomain = v; } }, loadEntities);
}

function initKeyDomainTabs() {
  initDomainTabsFor('key-domain-tabs', KEY_HA_DOMAINS, { get value() { return activeKeyDomain; }, set value(v) { activeKeyDomain = v; } }, loadKeyEntities);
}

async function loadHaEntities(opts) {
  const searchId = opts.searchId || 'entity-search';
  const statusId = opts.statusId || 'entity-status';
  const pickId = opts.pickId || 'entity-pick';
  const domain = opts.domain ?? activeDomain;
  const selectEntityId = opts.selectEntityId || '';
  const status = $(statusId);
  if (!status) return;
  status.textContent = 'Loading...';
  status.classList.remove('error');
  try {
    const q = encodeURIComponent($(searchId)?.value || '');
    const dom = encodeURIComponent(domain);
    const data = await api(`/api/ha/entities?domain=${dom}&search=${q}`);
    const list = data.entities || [];
    const sel = $(pickId);
    if (!sel) return;
    sel.innerHTML = '';
    list.forEach((e) => {
      const o = document.createElement('option');
      o.value = e.entity_id;
      o.dataset.fname = e.friendly_name || '';
      o.dataset.icon = e.icon && e.icon !== 'null' ? e.icon : '';
      o.textContent = `${e.friendly_name || e.entity_id} — ${e.state}`;
      sel.appendChild(o);
    });
    if (selectEntityId) {
      sel.value = selectEntityId;
      if (sel.value !== selectEntityId) {
        const o = document.createElement('option');
        o.value = selectEntityId;
        o.textContent = selectEntityId;
        sel.insertBefore(o, sel.firstChild);
        sel.value = selectEntityId;
      }
    }
    status.textContent = list.length ? `${list.length} entities` : 'No entities (try another domain)';
    if (opts.onLoaded) opts.onLoaded(domain, sel.value);
  } catch (err) {
    status.textContent = err.message;
    status.classList.add('error');
  }
}

async function loadEntities() {
  await loadHaEntities({ domain: activeDomain });
}

async function loadKeyEntities(selectEntityId, selectedService) {
  await loadHaEntities({
    searchId: 'key-entity-search',
    statusId: 'key-entity-status',
    pickId: 'key-entity-pick',
    domain: activeKeyDomain,
    selectEntityId,
    onLoaded: (domain, ent) => {
      const d = ent ? ent.split('.')[0] : domain;
      syncKeyHaServiceSelect(d, selectedService);
    },
  });
}

const GOOGLE_TV_KEYS = {
  '^': { type: 'ble_key', key: 'UP' },
  u: { type: 'ble_key', key: 'UP' },
  d: { type: 'ble_key', key: 'DOWN' },
  l: { type: 'ble_key', key: 'LEFT' },
  r: { type: 'ble_key', key: 'RIGHT' },
  k: { type: 'ble_key', key: 'ENTER' },
  '+': { type: 'ble_key', key: 'VOLUME_UP' },
  '-': { type: 'ble_key', key: 'VOLUME_DOWN' },
  b: { type: 'ble_key', key: 'BACK' },
  e: { type: 'ble_key', key: 'HOME' },
  m: { type: 'ble_key', key: 'MENU' },
  '>': { type: 'ble_key', key: 'PLAY_PAUSE' },
  '1': { type: 'navigate_page', page_id: 'home' },
};

function googleTvTemplate() {
  return {
    id: 'google_tv',
    name: 'Google TV',
    keys: { ...GOOGLE_TV_KEYS },
    buttons: [
      { id: 'back', label: 'Back', x: 10, y: 8, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'BACK' } },
      { id: 'home', label: 'Home', x: 160, y: 8, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'HOME' } },
      { id: 'up', label: 'Up', x: 85, y: 8, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'UP' } },
      { id: 'left', label: 'Left', x: 10, y: 83, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'LEFT' } },
      { id: 'ok', label: 'OK', x: 85, y: 83, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'ENTER' } },
      { id: 'right', label: 'Right', x: 160, y: 83, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'RIGHT' } },
      { id: 'down', label: 'Down', x: 85, y: 158, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'DOWN' } },
      { id: 'vol_up', label: 'Vol+', x: 10, y: 158, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'VOLUME_UP' } },
      { id: 'vol_down', label: 'Vol-', x: 160, y: 158, w: 70, h: 40, color: 0x2255aa, action: { type: 'ble_key', key: 'VOLUME_DOWN' } },
      { id: 'keyboard', label: 'KB', x: 10, y: 218, w: 70, h: 36, color: 0x555588, action: { type: 'open_keyboard' } },
      { id: 'to_home', label: 'HA', x: 90, y: 218, w: 140, h: 36, color: 0x33aa66, action: { type: 'navigate_page', page_id: 'home' } },
    ],
  };
}

function describeKeyAction(a) {
  if (!a) return '—';
  if (a.type === 'ble_key' || a.type === 'ble_media') return a.key || a.type;
  if (a.type === 'navigate_page') return '→ ' + (a.page_id || '?');
  if (a.type === 'open_keyboard') return '⌨ keyboard';
  if (a.type === 'ir') {
    if (a.ir_id) {
      const e = irLibrary.find((x) => x.id === a.ir_id);
      return e ? `IR ${e.name}` : `IR ${a.ir_id}`;
    }
    return `IR ${a.protocol || ''}`.trim();
  }
  if (a.type === 'ha_service') {
    const eid = a.target?.entity_id || a.entity_id || '';
    const svc = a.service ? `${a.service} ` : '';
    return 'HA ' + svc + (eid.split('.').pop() || eid || '?');
  }
  return a.type;
}

function makeRemoteKeyBtn(ch, label, shapeClass) {
  const mapped = getKeyAction(ch);
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'remote-key' + (shapeClass ? ' ' + shapeClass : '');
  if ('1234'.includes(ch)) btn.classList.add('color-' + ch);
  btn.innerHTML =
    `<span class="rk-lbl">${label}</span>` +
    `<span class="rk-map">${describeKeyAction(mapped)}</span>`;
  btn.title = `Key '${ch}'`;
  if (mapped) btn.classList.add('mapped');
  if (ch === selectedKeyChar) btn.classList.add('selected');
  if (learnActive) btn.classList.add('learning');
  btn.onclick = () => selectPhysicalKey(ch, label);
  return btn;
}

function renderRemoteKeymap() {
  const root = $('remote-keymap');
  if (!root) return;
  root.innerHTML = '';
  const page = currentPage();
  if (!page) return;
  if (!page.keys) page.keys = {};

  const powerRow = document.createElement('div');
  powerRow.className = 'remote-power-row';
  powerRow.appendChild(makeRemoteKeyBtn(POWER_KEY, 'Power', 'shape-power'));
  root.appendChild(powerRow);

  const screen = document.createElement('div');
  screen.className = 'remote-screen';
  screen.textContent = 'Touch screen';
  root.appendChild(screen);

  const face = document.createElement('div');
  face.className = 'remote-face';

  const media = document.createElement('div');
  media.className = 'remote-media';
  [['=', 'Stop'], ['<', 'Rewind'], ['p', 'Play'], ['>', 'Forward']].forEach(([ch, lbl]) => {
    media.appendChild(makeRemoteKeyBtn(ch, lbl, 'shape-round'));
  });
  face.appendChild(media);

  const nav = document.createElement('div');
  nav.className = 'remote-nav';
  nav.appendChild(makeRemoteKeyBtn('c', 'Menu', 'shape-corner corner-tl'));
  nav.appendChild(makeRemoteKeyBtn('i', 'Info', 'shape-corner corner-tr'));
  nav.appendChild(makeRemoteKeyBtn('b', 'Back', 'shape-corner corner-bl'));
  nav.appendChild(makeRemoteKeyBtn('s', 'Source', 'shape-corner corner-br'));

  const dpad = document.createElement('div');
  dpad.className = 'remote-dpad';
  dpad.appendChild(makeRemoteKeyBtn('u', 'Up', 'shape-dpad dpad-up'));
  dpad.appendChild(makeRemoteKeyBtn('l', 'Left', 'shape-dpad dpad-left'));
  dpad.appendChild(makeRemoteKeyBtn('k', 'OK', 'shape-dpad-ok dpad-ok'));
  dpad.appendChild(makeRemoteKeyBtn('r', 'Right', 'shape-dpad dpad-right'));
  dpad.appendChild(makeRemoteKeyBtn('d', 'Down', 'shape-dpad dpad-down'));
  nav.appendChild(dpad);
  face.appendChild(nav);

  const rockers = document.createElement('div');
  rockers.className = 'remote-rockers';
  const vol = document.createElement('div');
  vol.className = 'remote-rocker';
  vol.appendChild(makeRemoteKeyBtn('+', 'Vol+', 'shape-rocker-tall'));
  vol.appendChild(makeRemoteKeyBtn('-', 'Vol-', 'shape-rocker-tall'));
  const mid = document.createElement('div');
  mid.className = 'remote-rocker mid';
  mid.appendChild(makeRemoteKeyBtn('m', 'Mute', 'shape-round'));
  mid.appendChild(makeRemoteKeyBtn('e', 'Record', 'shape-round'));
  const ch = document.createElement('div');
  ch.className = 'remote-rocker';
  ch.appendChild(makeRemoteKeyBtn('^', 'CH+', 'shape-rocker-tall'));
  ch.appendChild(makeRemoteKeyBtn('v', 'CH-', 'shape-rocker-tall'));
  rockers.appendChild(vol);
  rockers.appendChild(mid);
  rockers.appendChild(ch);
  face.appendChild(rockers);

  const colors = document.createElement('div');
  colors.className = 'remote-colors';
  ['1', '2', '3', '4'].forEach((ch) => {
    colors.appendChild(makeRemoteKeyBtn(ch, KEY_LABELS[ch], 'shape-round'));
  });
  face.appendChild(colors);

  root.appendChild(face);
}

function selectPhysicalKey(ch, label) {
  selectedKeyChar = ch;
  $('key-selected-label').textContent = `${label} (key '${ch}')`;
  $('key-action-panel').classList.remove('hidden');
  const act = getKeyAction(ch);
  if (act) {
    $('key-action-type').value = act.type || 'ble_key';
    if (act.type === 'ble_key' || act.type === 'ble_media') $('key-ble-key').value = act.key || 'UP';
    if (act.type === 'navigate_page') $('key-nav-page').value = act.page_id || '';
    if (act.type === 'ha_service') {
      const eid = act.target?.entity_id || act.entity_id || '';
      const dom = eid.split('.')[0] || 'light';
      if (KEY_HA_DOMAINS.includes(dom)) activeKeyDomain = dom;
      initKeyDomainTabs();
      loadKeyEntities(eid, act.service);
    }
    if (act.type === 'ir' && $('key-ir-lib')) $('key-ir-lib').value = act.ir_id || '';
  }
  $('key-action-type').dispatchEvent(new Event('change'));
  renderRemoteKeymap();
}

function stopLearnMode(msg) {
  learnActive = false;
  if (learnPoll) {
    clearInterval(learnPoll);
    learnPoll = null;
  }
  if (msg && $('learn-key-status')) $('learn-key-status').textContent = msg;
  renderRemoteKeymap();
}

function startLearnMode() {
  stopLearnMode('Press any key on the Omote now…');
  learnActive = true;
  learnLastTs = 0;
  renderRemoteKeymap();
  learnPoll = setInterval(async () => {
    try {
      const ev = await api('/api/key/last');
      if (!ev.pressed || !ev.key) return;
      if (ev.ts && ev.ts === learnLastTs) return;
      learnLastTs = ev.ts;
      const ch = String(ev.key);
      const label = KEY_LABELS[ch] || ch;
      selectPhysicalKey(ch, label);
      stopLearnMode(`Captured ${label} ('${ch}') — set action below, then Assign.`);
    } catch (_) {}
  }, 150);
}

function applyKeyProfile() {
  const page = currentPage();
  if (!page) return;
  const prof = $('key-profile').value;
  if (prof === 'google_tv') page.keys = { ...GOOGLE_TV_KEYS };
  else if (prof === 'home') {
    page.keys = { '1': { type: 'navigate_page', page_id: 'home' }, '2': { type: 'navigate_page', page_id: 'google_tv' } };
  }
  renderRemoteKeymap();
}

async function loadConfig(opts = {}) {
  const { allowCacheFallback = true } = opts;
  try {
    config = await api('/api/config');
    applyConfigFromDevice();
    cacheConfig();
    return true;
  } catch (e) {
    if (allowCacheFallback && restoreConfigCache()) {
      $('editor-msg').textContent = 'Device unreachable — showing cached layout from this browser.';
      $('editor-msg').classList.add('error');
    }
    throw e;
  }
}

async function connectToDevice(opts = {}) {
  const retries = opts.retries ?? 35;
  const silent = !!opts.silent;
  applyApiBaseFromUi();
  saveEditorPrefs({ device_url: API });
  deviceConnected = false;
  for (let i = 0; i < retries; i++) {
    try {
      if (!silent) {
        $('status-bar').textContent = `Connecting to ${API} (${i + 1}/${retries})…`;
      }
      await refreshStatus();
      await loadSettings();
      await loadConfig({ allowCacheFallback: false });
      await loadIrLibrary();
      deviceConnected = true;
      const st = await api('/api/status');
      const pages = st.config_pages != null ? `${st.config_pages} pages` : '';
      const btns = st.config_buttons != null ? `, ${st.config_buttons} buttons` : '';
      $('status-bar').textContent = `Connected — ${API}${pages ? ` (${pages}${btns})` : ''}`;
      $('setup-msg').textContent = '';
      $('setup-msg').classList.remove('error');
      $('editor-msg').classList.remove('error');
      return true;
    } catch (e) {
      if (i === retries - 1) {
        restoreConfigCache();
        $('status-bar').textContent = `Cannot reach ${API} — ${e.message}`;
        if (!silent) {
          $('setup-msg').textContent = 'Saved layout shown from browser cache. Retry when the device is online.';
          $('setup-msg').classList.add('error');
        }
      } else {
        await sleep(1000);
      }
    }
  }
  return false;
}

function formatBleStatusHtml(b) {
  if (!b) return 'BLE status unavailable';
  const lines = [
    `Connected: ${b.connected ? 'yes' : 'no'}`,
    `Discoverable: ${b.advertising ? 'yes' : 'no'}`,
    `Pairing mode: ${b.pairing_mode ? 'on' : 'off'}`,
    `Bonded devices: ${b.bond_count ?? 0}`,
  ];
  if (b.bonds?.length) lines.push(`Bonds: ${b.bonds.join(', ')}`);
  return lines.map((l) => `<p>${l}</p>`).join('');
}

async function refreshBleUi(msgEl) {
  const targets = ['ble-status', 'device-ble-status'].filter((id) => $(id));
  try {
    const b = await api('/api/ble/status');
    const html = formatBleStatusHtml(b);
    targets.forEach((id) => { $(id).innerHTML = html; });
    if (msgEl && $(msgEl)) $(msgEl).textContent = '';
    return b;
  } catch (e) {
    const err = e.message || String(e);
    targets.forEach((id) => { $(id).textContent = err; });
    if (msgEl && $(msgEl)) {
      $(msgEl).textContent = err;
      $(msgEl).classList.add('error');
    }
    throw e;
  }
}

async function bleControl(path, okMsg, msgEl) {
  const el = msgEl && $(msgEl);
  try {
    await api(path, { method: 'POST' });
    await refreshBleUi();
    if (el) {
      el.textContent = okMsg;
      el.classList.remove('error');
    }
    await refreshStatus();
  } catch (e) {
    if (el) {
      el.textContent = e.message;
      el.classList.add('error');
    }
  }
}

async function loadDeviceSettings() {
  const d = await api('/api/device/settings');
  $('dev-brightness').value = d.brightness ?? 180;
  $('dev-display-timeout').value = Math.round(
    (d.display_timeout_ms || d.sleep_timeout_ms || 60000) / 1000
  );
  $('dev-deep-sleep').value = Math.round((d.deep_sleep_timeout_ms || 900000) / 1000);
  $('dev-motion').checked = d.motion_wake_enabled !== false;
  $('device-status').innerHTML = `
    <p>Battery: ${d.battery_percent ?? '?'}% ${d.battery_charging ? '(charging)' : ''}</p>
    <p>WiFi: ${d.wifi_connected ? d.wifi_ssid + ' @ ' + d.ip : 'offline'}</p>
    <p>Screen: ${d.display_off ? 'off (WiFi on)' : 'on'}</p>`;
  refreshBleUi().catch(() => {});
}

async function refreshLog() {
  try {
    const d = await api('/api/device/log');
    $('debug-log').textContent = d.log || '(empty)';
  } catch (e) {
    $('debug-log').textContent = e.message;
  }
}

async function refreshStatus() {
  const s = await api('/api/status');
  const bat = s.battery_percent != null ? ` | Bat ${s.battery_percent}%` : '';
  $('status-bar').textContent =
    `WiFi: ${s.wifi?.connected ? s.wifi.ssid : 'offline'} | HA: ${s.ha_configured ? 'yes' : 'no'} | BLE: ${s.ble?.connected ? 'on' : 'off'}${bat}`;
}

document.querySelectorAll('#nav button').forEach((b) => {
  b.onclick = () => showTab(b.dataset.tab);
});

$('action-type').onchange = syncActionPanels;
$('btn-widget').onchange = () => {
  const w = $('btn-widget').value;
  if (w === 'toggle' || w === 'label' || w === 'climate' || w === 'climate_thermostat') {
    $('action-type').value = 'ha_service';
    if (w === 'climate' || w === 'climate_thermostat') activeDomain = 'climate';
  }
  syncActionPanels();
  applyEntityToForm();
};
$('entity-pick').onchange = () => {
  applyEntityToForm();
  drawCanvas();
};
$('key-action-type').onchange = () => {
  const t = $('key-action-type').value;
  $('key-ble-key').style.display = t === 'ble_key' ? '' : 'none';
  $('key-nav-page').style.display = t === 'navigate_page' ? '' : 'none';
  $('key-ha-wrap').classList.toggle('hidden', t !== 'ha_service');
  $('key-ir-wrap')?.classList.toggle('hidden', t !== 'ir');
  if (t === 'ha_service' && deviceConnected) loadKeyEntities();
};
$('entity-search').oninput = () => loadEntities();
$('key-entity-search')?.addEventListener('input', () => loadKeyEntities());
$('key-entity-pick')?.addEventListener('change', () => {
  const ent = $('key-entity-pick')?.value;
  if (ent) syncKeyHaServiceSelect(ent.split('.')[0], $('key-ha-service')?.value);
});

$('btn-save-settings').onclick = async () => {
  try {
    const body = { ha_url: $('ha-url').value.trim() };
    const tok = $('ha-token').value.trim();
    if (tok) body.ha_token = tok;
    // Keep local browser values so refresh does not force re-entry.
    saveEditorPrefs({ ha_url: body.ha_url, ha_token: tok || loadEditorPrefs().ha_token || '' });
    await api('/api/settings', { method: 'POST', body: JSON.stringify(body) });
    $('setup-msg').textContent = 'Settings saved.';
    $('setup-msg').classList.remove('error');
    await refreshStatus();
  } catch (e) {
    $('setup-msg').textContent = e.message;
    $('setup-msg').classList.add('error');
  }
};

$('btn-export-backup').onclick = async () => {
  try {
    let data = await api('/api/backup/export');
    if (typeof data === 'string') {
      try {
        data = JSON.parse(data);
      } catch {
        data = parseBackupText(JSON.stringify(data)).data;
      }
    }
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `omote-backup-${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(a.href);
    $('setup-msg').textContent = 'Backup downloaded (includes HA token — keep it private).';
    $('setup-msg').classList.remove('error');
  } catch (e) {
    $('setup-msg').textContent = e.message;
    $('setup-msg').classList.add('error');
  }
};

$('backup-file').onchange = async (ev) => {
  const file = ev.target.files?.[0];
  if (!file) return;
  const msg = $('setup-msg');
  const editorMsg = $('editor-msg');
  try {
    const text = await file.text();
    let data;
    let backupRepaired = false;
    try {
      ({ data, repaired: backupRepaired } = parseBackupText(text));
    } catch (e) {
      throw new Error(e.message === 'Unexpected end of JSON input' || e.message?.includes('JSON')
        ? 'Invalid or incomplete JSON backup file'
        : e.message || 'Invalid JSON file');
    }
    const btnCount = configButtonCountFromData(data);
    if (!backupHasImportableContent(data)) {
      const hint =
        typeof data === 'string'
          ? 'file is a JSON-encoded string — re-export with Export all settings'
          : `top-level keys: ${Object.keys(data).slice(0, 8).join(', ') || '(empty)'}`;
      throw new Error(`Unrecognized backup (${hint}). Use Export all settings, or a config JSON with "pages".`);
    }
    if (btnCount === 0 && configFromBackup(data)?.pages?.some((p) => p.keys && Object.keys(p.keys).length)) {
      msg.textContent =
        'Warning: backup has remote keys but no touchscreen buttons — layout will be empty until you re-add widgets.';
      msg.classList.remove('error');
    }
    const payload = buildBackupPayload(data);
    msg.textContent = 'Uploading backup to device…';
    msg.classList.remove('error');
    await api('/api/backup/import', { method: 'POST', body: payload });
    applyBackupToEditor(data);
    showTab('layout');
    const summary = `${config.pages?.length || 0} page(s), ${configButtonCount()} button(s)`;
    let reloadNote = '';
    try {
      msg.textContent = 'Backup on device — syncing editor…';
      await reloadFromDeviceAfterImport();
    } catch (reloadErr) {
      reloadNote = ` (device sync: ${reloadErr.message})`;
    }
    const repairNote = backupRepaired ? ' (file was truncated — partial keymap restored)' : '';
    const done = `Backup restored — ${summary}${repairNote}${reloadNote}`;
    msg.textContent = done;
    if (editorMsg) {
      editorMsg.textContent = done;
      editorMsg.classList.remove('error');
    }
    ev.target.value = '';
  } catch (e) {
    const err = 'Import failed: ' + e.message;
    msg.textContent = err;
    msg.classList.add('error');
    if (editorMsg) {
      editorMsg.textContent = err;
      editorMsg.classList.add('error');
    }
    ev.target.value = '';
  }
};

$('btn-test-ha').onclick = async () => {
  try {
    await $('btn-save-settings').onclick();
    await api('/api/settings/test', { method: 'POST' });
    $('setup-msg').textContent = 'HA connection OK';
    await loadEntities();
  } catch (e) {
    $('setup-msg').textContent = e.message;
    $('setup-msg').classList.add('error');
  }
};

$('btn-add-page').onclick = () => {
  if (config.pages.length >= MAX_DEVICE_PAGES) {
    $('editor-msg').textContent = `Device supports at most ${MAX_DEVICE_PAGES} pages.`;
    $('editor-msg').classList.add('error');
    return;
  }
  const id = 'page_' + Date.now().toString(36);
  config.pages.push({ id, name: 'New page', buttons: [], keys: {} });
  selectPage(id);
  $('editor-msg').classList.remove('error');
};

function hvacPageTemplate() {
  const id = 'hvac_' + Date.now().toString(36);
  return {
    id,
    name: 'HVAC',
    buttons: [{
      id: 'thermo_' + Date.now().toString(36),
      label: 'Thermostat',
      widget: 'climate_thermostat',
      x: 0,
      y: 0,
      w: 240,
      h: CONTENT_H,
      style: { bg: 0x2a1a3a, fg: 0xffffff, radius: 0 },
      action: { type: 'ha_service', domain: 'climate', service: 'set_temperature', target: { entity_id: '' } },
    }],
    keys: {},
  };
}

$('btn-add-hvac-page').onclick = () => {
  const tpl = hvacPageTemplate();
  config.pages.push(tpl);
  selectPage(tpl.id);
  activeDomain = 'climate';
  initDomainTabs();
  loadEntities();
  $('btn-widget').value = 'climate_thermostat';
  syncActionPanels();
};

$('btn-add-google-tv').onclick = () => {
  const tpl = googleTvTemplate();
  const idx = config.pages.findIndex((p) => p.id === 'google_tv');
  if (idx >= 0) config.pages[idx] = tpl;
  else config.pages.push(tpl);
  selectPage('google_tv');
};

$('btn-add-button').onclick = () => {
  const page = currentPage();
  if (!page) return;
  if (!page.buttons) page.buttons = [];
  const id = 'btn_' + Date.now().toString(36);
  page.buttons.push({
    id,
    label: $('btn-label').value || 'Button',
    widget: $('btn-widget').value,
    x: snap(20),
    y: snap(30),
    w: $('btn-widget').value === 'climate_thermostat' ? 240 : $('btn-widget').value === 'climate' ? 220 : 90,
    h: $('btn-widget').value === 'climate_thermostat' ? CONTENT_H : $('btn-widget').value === 'climate' ? 88 : 44,
    color: 0x3366cc,
    style: { bg: 0x3366cc, radius: 8 },
    action: actionFromUi(),
  });
  const nb = page.buttons[page.buttons.length - 1];
  readStyleFromForm(nb);
  applyEntityToForm();
  selectedButtonId = id;
  drawCanvas();
  updateEditUi();
  cacheConfig();
};

$('btn-update-button').onclick = () => {
  const page = currentPage();
  const btn = page?.buttons?.find((b) => b.id === selectedButtonId);
  if (!btn) return;
  btn.label = $('btn-label').value || btn.label;
  btn.widget = $('btn-widget').value;
  btn.action = actionFromUi();
  readStyleFromForm(btn);
  drawCanvas();
  if (btn.widget === 'toggle') refreshHaToggleStates();
  $('editor-msg').textContent = 'Updated (deploy to apply on device)';
  cacheConfig();
};

['btn-bg', 'btn-fg', 'btn-radius', 'btn-border', 'btn-border-w', 'btn-x', 'btn-y', 'btn-w', 'btn-h'].forEach((id) => {
  const el = $(id);
  if (!el) return;
  el.addEventListener('change', () => {
    const btn = currentPage()?.buttons?.find((b) => b.id === selectedButtonId);
    if (!btn) return;
    readStyleFromForm(btn);
    drawCanvas();
  });
});
$('btn-snap-grid')?.addEventListener('change', drawCanvas);
$('btn-show-grid')?.addEventListener('change', drawCanvas);
$('btn-prev-page')?.addEventListener('click', () => cycleEditorPage(-1));
$('btn-next-page')?.addEventListener('click', () => cycleEditorPage(1));

$('btn-delete-button').onclick = () => {
  const page = currentPage();
  if (!page || !selectedButtonId) return;
  page.buttons = page.buttons.filter((b) => b.id !== selectedButtonId);
  selectedButtonId = null;
  drawCanvas();
  updateEditUi();
};

let deployInFlight = false;

$('btn-deploy').onclick = async () => {
  if (deployInFlight) return;
  deployInFlight = true;
  const deployBtn = $('btn-deploy');
  if (deployBtn) deployBtn.disabled = true;
  try {
    if (config.pages.length > MAX_DEVICE_PAGES) {
      throw new Error(`Too many pages (${config.pages.length}). Device max is ${MAX_DEVICE_PAGES}.`);
    }
    if (!config.pages.length) {
      throw new Error('Add at least one page before deploying.');
    }
    const selBtn = currentPage()?.buttons?.find((b) => b.id === selectedButtonId);
    if (selBtn) readStyleFromForm(selBtn);
    config.active_page_id = selectedPageId;
    config.schema_version = 2;
    cacheConfig();
    const expectedButtons = configButtonCount();
    const payload = JSON.stringify(config);
    if (payload.length < 32) {
      throw new Error('Config payload is empty — reload the page and try again.');
    }
    $('editor-msg').textContent = `Deploying ${expectedButtons} button(s)…`;
    $('editor-msg').classList.remove('error');
    await api('/api/config', { method: 'POST', body: payload });
    $('editor-msg').textContent = 'Deployed — device rebooting…';
    deviceConnected = false;
    for (let i = 0; i < 40; i++) {
      await sleep(1000);
      if (await connectToDevice({ retries: 1, silent: true })) {
        const onDevice = configButtonCount();
        if (onDevice < expectedButtons) {
          $('editor-msg').textContent =
            `Device rebooted but only shows ${onDevice}/${expectedButtons} buttons — deploy may have failed. Check serial log.`;
          $('editor-msg').classList.add('error');
          return;
        }
        $('editor-msg').textContent =
          `Deployed — "${config.active_page_id}" has ${currentPage()?.buttons?.length || 0} button(s) on this page (${onDevice} total).`;
        return;
      }
      $('editor-msg').textContent = `Waiting for device… (${i + 1}/40)`;
    }
    $('editor-msg').textContent =
      'Deploy sent — device did not respond in time. Refresh later or check WiFi.';
    $('editor-msg').classList.add('error');
  } catch (e) {
    $('editor-msg').textContent = e.message;
    $('editor-msg').classList.add('error');
  } finally {
    deployInFlight = false;
    if (deployBtn) deployBtn.disabled = false;
  }
};

$('btn-save-keymap').onclick = async () => {
  $('learn-key-status').textContent = 'Deploying keymap to device...';
  await $('btn-deploy').onclick();
  if ($('editor-msg')?.textContent?.startsWith('Deployed')) {
    $('learn-key-status').textContent = 'Keymap deployed to device.';
  } else {
    $('learn-key-status').textContent = `Deploy failed: ${$('editor-msg')?.textContent || 'unknown error'}`;
  }
};
$('keymap-page').onchange = () => {
  selectedPageId = $('keymap-page').value;
  renderRemoteKeymap();
  $('keymap-page-name').textContent = currentPage()?.name || '';
};
$('btn-apply-profile').onclick = applyKeyProfile;
$('btn-learn-key').onclick = startLearnMode;

$('btn-assign-key').onclick = () => {
  const page = currentPage();
  if (!page || !selectedKeyChar) return;
  if (!page.keys) page.keys = {};
  const t = $('key-action-type').value;
  let action = null;
  if (t === 'ble_key') action = { type: 'ble_key', key: $('key-ble-key').value };
  else if (t === 'navigate_page') action = { type: 'navigate_page', page_id: $('key-nav-page').value };
  else if (t === 'ha_service') {
    const ent = $('key-entity-pick')?.value?.trim();
    if (!ent) {
      alert('Select a Home Assistant entity');
      return;
    }
    const domain = ent.split('.')[0] || 'light';
    const service = $('key-ha-service')?.value || defaultHaService(domain, 'push');
    action = {
      type: 'ha_service',
      domain,
      service,
      target: { entity_id: ent },
    };
  } else if (t === 'ir') {
    const irId = $('key-ir-lib')?.value;
    if (!irId) {
      alert('Select an IR library entry');
      return;
    }
    const entry = irLibrary.find((e) => e.id === irId);
    action = {
      type: 'ir',
      ir_id: irId,
      protocol: entry?.protocol || 'NEC',
      code: entry?.code || '0',
      bits: entry?.bits || 32,
    };
  }
  if (!action) return;
  if (selectedKeyChar === POWER_KEY) setGlobalKeyAction(POWER_KEY, action);
  else page.keys[selectedKeyChar] = action;
  cacheConfig();
  renderRemoteKeymap();
  $('learn-key-status').textContent = `Assigned ${KEY_LABELS[selectedKeyChar] || selectedKeyChar} — click Save & deploy to push.`;
};

$('btn-clear-key').onclick = () => {
  if (!selectedKeyChar) return;
  if (selectedKeyChar === POWER_KEY) {
    if (config.keymap) config.keymap = config.keymap.filter((kb) => kb.key !== POWER_KEY);
  } else {
    const page = currentPage();
    if (page?.keys) delete page.keys[selectedKeyChar];
  }
  cacheConfig();
  renderRemoteKeymap();
};

const preview = $('preview');
preview.onmousedown = (ev) => {
  const { mx, my } = canvasCoords(ev);
  const hit = canvasHit(mx, my);
  if (hit) {
    selectedButtonId = hit.id;
    loadButtonToForm(hit);
    dragState = { id: hit.id, ox: mx - hit.x, oy: my - 22 - hit.y };
    drawCanvas();
    return;
  }
};
preview.onmousemove = (ev) => {
  if (!dragState) return;
  const page = currentPage();
  const btn = page?.buttons?.find((b) => b.id === dragState.id);
  if (!btn) return;
  const { mx, my } = canvasCoords(ev);
  btn.x = snap(Math.max(0, Math.min(mx - dragState.ox, 240 - btn.w)));
  btn.y = snap(Math.max(0, Math.min(my - 22 - dragState.oy, CONTENT_H - btn.h)));
  $('btn-x').value = btn.x;
  $('btn-y').value = btn.y;
  drawCanvas();
};
preview.onmouseup = () => { dragState = null; };
preview.onclick = (ev) => {
  if (dragState) return;
  const { mx, my } = canvasCoords(ev);
  const hit = canvasHit(mx, my);
  if (hit) {
    selectedButtonId = hit.id;
    loadButtonToForm(hit);
    drawCanvas();
    return;
  }
};

let touchSwipeStart = null;
preview.addEventListener('touchstart', (ev) => {
  if (ev.touches.length !== 1) return;
  touchSwipeStart = { x: ev.touches[0].clientX, y: ev.touches[0].clientY };
}, { passive: true });
preview.addEventListener('touchend', (ev) => {
  if (!touchSwipeStart || !ev.changedTouches.length) return;
  const dx = ev.changedTouches[0].clientX - touchSwipeStart.x;
  const dy = ev.changedTouches[0].clientY - touchSwipeStart.y;
  touchSwipeStart = null;
  if (Math.abs(dx) < 50 || Math.abs(dx) < Math.abs(dy)) return;
  cycleEditorPage(dx < 0 ? 1 : -1);
}, { passive: true });

$('btn-ir-learn').onclick = async () => {
  $('ir-learn-status').textContent = 'Listening… point remote at Omote';
  try {
    const r = await startIrLearn();
    $('ir-protocol').value = r.protocol;
    $('ir-code').value = r.code;
    $('ir-learn-status').textContent = 'Captured — save to library or assign';
    const saved = await saveIrCaptureToLibrary(r, $('btn-label').value || '');
    if (saved?.id && $('ir-lib-select')) $('ir-lib-select').value = saved.id;
  } catch (e) {
    $('ir-learn-status').textContent = e.message;
  }
};

$('btn-ir-tab-learn')?.addEventListener('click', async () => {
  const status = $('ir-tab-learn-status');
  if (status) status.textContent = 'Listening… press a button on your IR remote';
  try {
    const r = await startIrLearn();
    const saved = await saveIrCaptureToLibrary(r, 'IR button');
    if (status) {
      if (!saved) status.textContent = 'Capture cancelled.';
      else {
        const entry = irLibrary.find((e) => e.id === saved.id);
        status.textContent = `Saved "${entry?.name || 'IR code'}" on device. Assign it below.`;
      }
    }
  } catch (e) {
    if (status) status.textContent = e.message;
  }
});

$('btn-ir-export')?.addEventListener('click', () => {
  const blob = new Blob([JSON.stringify(irLibrary, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `omote-ir-library-${new Date().toISOString().slice(0, 10)}.json`;
  a.click();
  URL.revokeObjectURL(a.href);
});

$('ir-import-file')?.addEventListener('change', async (ev) => {
  const file = ev.target.files?.[0];
  if (!file) return;
  try {
    const entries = JSON.parse(await file.text());
    if (!Array.isArray(entries)) throw new Error('Expected a JSON array');
    await api('/api/ir/library/import', { method: 'POST', body: JSON.stringify(entries) });
    await loadIrLibrary();
    $('ir-tab-learn-status').textContent = `Imported ${entries.length} code(s) to device.`;
    ev.target.value = '';
  } catch (e) {
    $('ir-tab-learn-status').textContent = 'Import failed: ' + e.message;
  }
});

$('btn-ir-save-lib')?.addEventListener('click', async () => {
  const name = prompt('IR name (e.g. TV Power):', $('btn-label').value || '');
  if (!name) return;
  const protocol = $('ir-protocol').value || 'NEC';
  const code = $('ir-code').value || '0';
  const bits = 32;
  const r = await api('/api/ir/library', {
    method: 'POST',
    body: JSON.stringify({ name, protocol, code, bits }),
  });
  await loadIrLibrary();
  if ($('ir-lib-select') && r?.id) $('ir-lib-select').value = r.id;
  $('ir-learn-status').textContent = 'Saved to library';
});

$('btn-ir-delete-lib')?.addEventListener('click', async () => {
  const id = $('ir-lib-select')?.value;
  if (!id) return;
  if (!confirm('Delete selected IR library entry?')) return;
  await api('/api/ir/library/delete', { method: 'POST', body: JSON.stringify({ id }) });
  await loadIrLibrary();
});

$('btn-refresh-ble')?.addEventListener('click', () => refreshBleUi('ble-tab-msg'));
$('btn-device-ble-refresh')?.addEventListener('click', () => refreshBleUi('device-ble-msg'));
const bindBleBtn = (id, path, msg, confirmText) => {
  $(id)?.addEventListener('click', async () => {
    if (confirmText && !confirm(confirmText)) return;
    await bleControl(path, msg, id.includes('device') ? 'device-ble-msg' : 'ble-tab-msg');
  });
};
bindBleBtn('btn-ble-pair', '/api/ble/start', 'Pairing on — add Omote Remote on your Google TV.');
bindBleBtn('btn-device-ble-pair', '/api/ble/start', 'Pairing on — add Omote Remote on your Google TV.');
bindBleBtn('btn-ble-disconnect', '/api/ble/disconnect', 'Disconnected active BLE clients.');
bindBleBtn('btn-device-ble-disconnect', '/api/ble/disconnect', 'Disconnected active BLE clients.');
bindBleBtn(
  'btn-ble-forget',
  '/api/ble/forget',
  'Bonds cleared. Pair your Google TV now.',
  'Forget all paired phones and TVs? You will need to pair again.'
);
bindBleBtn(
  'btn-device-ble-forget',
  '/api/ble/forget',
  'Bonds cleared. Pair your Google TV now.',
  'Forget all paired phones and TVs? You will need to pair again.'
);

let bleIdentitiesCache = null;
async function loadBleIdentities() {
  const sel = $('ble-identity-select');
  if (!sel) return;
  try {
    const data = await api('/api/ble/identities');
    bleIdentitiesCache = data;
    sel.innerHTML = '';
    for (const id of data.identities || []) {
      const opt = document.createElement('option');
      opt.value = id.key;
      opt.textContent = id.name + (id.recommended ? ' \u2605' : '');
      sel.appendChild(opt);
    }
    sel.value = data.current;
    updateBleIdentityDesc();
  } catch (e) {
    sel.innerHTML = '<option value="">(unavailable)</option>';
    const msg = $('ble-identity-msg');
    if (msg) { msg.textContent = e.message || String(e); msg.classList.add('error'); }
  }
}
function updateBleIdentityDesc() {
  const sel = $('ble-identity-select');
  const desc = $('ble-identity-desc');
  if (!sel || !desc || !bleIdentitiesCache) return;
  const id = (bleIdentitiesCache.identities || []).find((i) => i.key === sel.value);
  if (!id) { desc.textContent = ''; return; }
  const vid = id.vid.toString(16).padStart(4, '0').toUpperCase();
  const pid = id.pid.toString(16).padStart(4, '0').toUpperCase();
  desc.textContent = `${id.description} (VID 0x${vid} / PID 0x${pid})`;
}
$('ble-identity-select')?.addEventListener('change', updateBleIdentityDesc);
$('btn-ble-identity-apply')?.addEventListener('click', async () => {
  const sel = $('ble-identity-select');
  const msg = $('ble-identity-msg');
  if (!sel || !sel.value) return;
  if (sel.value === bleIdentitiesCache?.current) {
    if (msg) { msg.textContent = 'Already using that identity.'; msg.classList.remove('error'); }
    return;
  }
  if (!confirm(
      'Switching the BLE identity wipes the current bond on the Omote. ' +
      'You will also need to forget the old Omote on the TV (Settings → ' +
      'Remotes & accessories) and pair again. Continue?')) return;
  if (msg) { msg.textContent = 'Applying…'; msg.classList.remove('error'); }
  try {
    await api('/api/ble/identity', { method: 'POST', body: JSON.stringify({ profile: sel.value }) });
    await loadBleIdentities();
    await refreshBleUi('ble-identity-msg');
    if (msg) {
      msg.textContent = 'Identity applied. On the TV, forget Omote Remote and pair again.';
      msg.classList.remove('error');
    }
  } catch (e) {
    if (msg) { msg.textContent = e.message || String(e); msg.classList.add('error'); }
  }
});
loadBleIdentities();

async function bleTestSend(body, label) {
  const msg = $('ble-test-msg');
  try {
    const res = await api('/api/ble/test', { method: 'POST', body: JSON.stringify(body) });
    if (msg) {
      msg.textContent = `Sent ${res.sent || label}.`;
      msg.classList.remove('error');
    }
  } catch (e) {
    if (msg) { msg.textContent = e.message || String(e); msg.classList.add('error'); }
  }
}
$('btn-ble-test-usage')?.addEventListener('click', () => {
  const raw = ($('ble-test-usage')?.value || '').trim();
  if (!raw) return;
  bleTestSend({ usage: raw }, raw);
});
$('btn-ble-test-button')?.addEventListener('click', () => {
  const n = parseInt($('ble-test-button')?.value || '0', 10);
  if (!n) return;
  bleTestSend({ button: n }, `BUTTON_${n}`);
});

$('btn-save-device').onclick = async () => {
  await api('/api/device/settings', {
    method: 'POST',
    body: JSON.stringify({
      brightness: parseInt($('dev-brightness').value, 10),
      display_timeout_ms: parseInt($('dev-display-timeout').value, 10) * 1000,
      deep_sleep_timeout_ms: parseInt($('dev-deep-sleep').value, 10) * 1000,
      motion_wake_enabled: $('dev-motion').checked,
    }),
  });
  alert('Device settings saved');
};

$('btn-reboot').onclick = async () => {
  if (confirm('Reboot Omote?')) await api('/api/device/reboot', { method: 'POST' });
};
$('btn-display-off').onclick = () => api('/api/device/display-off', { method: 'POST' });
$('btn-deep-sleep').onclick = () => {
  if (confirm('Deep sleep? Wake with key, touch, or motion.')) api('/api/device/sleep', { method: 'POST' });
};
$('btn-refresh-log').onclick = refreshLog;

async function init() {
  const prefs = loadEditorPrefs();
  API = prefs.device_url || localStorage.getItem(API_BASE_KEY) || API;
  $('device-url').value = prefs.device_url || API;
  if (prefs.ha_url && !$('ha-url').value) $('ha-url').value = prefs.ha_url;
  if (prefs.ha_token && !$('ha-token').value) $('ha-token').value = prefs.ha_token;
  restoreConfigCache();
  restoreIrLibraryCache();
  const persistDeviceUrl = () => {
    applyApiBaseFromUi();
    saveEditorPrefs({ device_url: API });
  };
  $('device-url')?.addEventListener('change', persistDeviceUrl);
  $('device-url')?.addEventListener('input', persistDeviceUrl);
  $('ha-url')?.addEventListener('change', () => {
    saveEditorPrefs({ ha_url: $('ha-url').value.trim() });
  });
  $('ha-token')?.addEventListener('change', () => {
    saveEditorPrefs({ ha_token: $('ha-token').value.trim() });
  });
  $('btn-save-device-url')?.addEventListener('click', () => connectToDevice({ retries: 20 }));
  initDomainTabs();
  initKeyDomainTabs();
  syncActionPanels();
  $('key-action-type').dispatchEvent(new Event('change'));
  populateBleKeySelects();
  document.querySelectorAll('#nav button').forEach((b) => {
    b.addEventListener('click', async () => {
      if (!deviceConnected) {
        await connectToDevice({ retries: 8, silent: true });
      }
      if (b.dataset.tab === 'layout') {
        loadEntities();
        refreshHaToggleStates();
        startHaStatePoll();
      }
      if (b.dataset.tab === 'ble') refreshBleUi('ble-tab-msg').catch(() => {});
      if (b.dataset.tab === 'device') { loadDeviceSettings(); refreshLog(); }
      if (b.dataset.tab === 'ir') loadIrLibrary();
      if (b.dataset.tab === 'keys') {
        fillNavPages();
        renderRemoteKeymap();
        if ($('key-action-type')?.value === 'ha_service') loadKeyEntities();
      }
    });
  });
  updateEditUi();
  await connectToDevice({ retries: 35 });
}

document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && !deviceConnected) {
    connectToDevice({ retries: 12, silent: true });
  }
});

async function loadSettings() {
  const s = await api('/api/settings');
  $('ha-url').value = s.ha_url || '';
  if (!s.ha_token_set) $('ha-token').placeholder = '(unchanged if empty)';
  saveEditorPrefs({
    ha_url: $('ha-url').value.trim(),
    ha_token: $('ha-token').value.trim() || loadEditorPrefs().ha_token || '',
  });
}

init();
