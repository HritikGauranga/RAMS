#include "AP.h"
#include "Shared.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>

static AsyncWebServer server(80);
static String gAuthSessionToken = "";
static const char *AUTH_COOKIE_NAME = "RAMS_AUTH";
static const char *AP_PASS_FIXED = "MSys@1234";
static bool serverStarted = false;
static bool serverRoutesSetup = false;
static String gRequestBody;
static String gRequestBodyRecipients;

static String htmlPage() {
  return R"RAW(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RAMS Dashboard</title>
<link rel="icon" type="image/png" href="/logo.png?v=2">
<style>
  * { box-sizing: border-box; }
  html,body { height: 100%; margin: 0; font-family: Arial, sans-serif; background: #f3f5f7; }
  .app { display: flex; height: 100%; }
  .sidebar { width: 220px; background: #0f1724; color: #fff; padding: 18px 12px; box-shadow: 2px 0 8px rgba(0,0,0,0.08); }
  .brand { font-size: 18px; font-weight: 700; margin-bottom: 18px; color: #fff; }
  .nav { display: block; }
  .nav a { display: block; padding: 12px 14px; margin-bottom: 8px; color: #e6eefc; text-decoration: none; border-radius: 6px; font-weight: 600; }
  .nav a:hover { background: rgba(255,255,255,0.04); color: #fff; }
  .nav a.active { background: #1e40af; color: #fff; }
  .nav .danger { background: #7f1d1d; }
  .content { flex: 1; padding: 22px; overflow: auto; }
  .card { background: #fff; border-radius: 10px; padding: 18px; box-shadow: 0 6px 20px rgba(15,23,42,0.06); max-width: 1100px; margin: 0 auto; }
  .grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px }
  .item { background:#f8fbff; border:1px solid #dce8f8; border-radius:8px; padding:12px }
  .label { font-size:13px; color:#546179 }
  .value { font-size:16px; font-weight:700; color:#1f2d3d }
  @media(max-width:900px){ .sidebar { position: relative; width: 100%; display:flex; gap:12px; overflow-x: auto; } .app { flex-direction: column; } .grid { grid-template-columns:1fr } }
</style>
</head>
<body>
<div class="app">
  <aside class="sidebar">
    <div class="brand">RAMS Configuration</div>
    <nav class="nav">
      <a href="/" class="active">Dashboard</a>
      <a href="/digital-inputs">Digital Inputs</a>
      <a href="/analog-inputs">Analog Inputs</a>
      <a href="/relay-outputs">Relay Outputs</a>
      <a href="/alarm-management">Alarm Management</a>
      <a href="/phone-numbers">Phone Number Management</a>
      <a href="/network-config">Network Configuration</a>
      <a href="/event-logs">Event Logs</a>
      <a href="/diagnostics">Diagnostics</a>
      <a href="/logout" class="danger">Logout</a>
    </nav>
  </aside>

  <main class="content">
    <div class="card">
      <h1 style="margin-top:0">Dashboard</h1>
      <div class="section-title">System Summary</div>
      <div id="dashboard" class="grid" style="margin-top:12px">
        <div class="item"><div class="label">Serial Number</div><div id="dash-serial" class="value">Loading...</div></div>
        <div class="item"><div class="label">AP IP</div><div id="dash-ap-ip" class="value">Loading...</div></div>
        <div class="item"><div class="label">TCP Endpoint</div><div id="dash-tcp" class="value">Loading...</div></div>
        <div class="item"><div class="label">Active Alarms</div><div id="dash-active" class="value">0</div></div>
        <div class="item"><div class="label">Acknowledged</div><div id="dash-ack" class="value">0</div></div>
        <div class="item"><div class="label">Unacknowledged</div><div id="dash-unack" class="value">0</div></div>
        <div class="item"><div class="label">Authorized Numbers</div><div id="dash-auth" class="value">0</div></div>
        <div class="item"><div class="label">Event Recipients</div><div id="dash-recip" class="value">0</div></div>
      </div>
    </div>
  </main>
</div>

<script>
function setEl(id, v){ var e = document.getElementById(id); if (e) e.textContent = v; }
fetch('/api/dashboard').then(r=>r.json()).then(d=>{
  setEl('dash-serial', d.serial_number || 'Not Set');
  setEl('dash-ap-ip', d.ap_ip || '-');
  setEl('dash-tcp', d.tcp_endpoint || '-');
  setEl('dash-active', String(d.active_alarms || 0));
  setEl('dash-ack', String(d.acknowledged_alarms || 0));
  setEl('dash-unack', String(d.unacknowledged_alarms || 0));
  setEl('dash-auth', String(d.authorized_numbers || 0));
  setEl('dash-recip', String(d.event_recipients || 0));
}).catch(e=>console.log('dashboard load failed', e));

// Highlight active nav link
(function(){
  var links = document.querySelectorAll('.nav a');
  links.forEach(function(a){ a.classList.remove('active'); if (a.getAttribute('href') === window.location.pathname) a.classList.add('active'); });
})();
</script>
</body>
</html>
)RAW";
}

static String makeSessionToken() {
  char buf[33] = {};
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  uint32_t c = esp_random();
  uint32_t d = esp_random();
  snprintf(buf, sizeof(buf), "%08lx%08lx%08lx%08lx",
           (unsigned long)a, (unsigned long)b, (unsigned long)c, (unsigned long)d);
  return String(buf);
}

static String getLoginUsername() { return String("Admin"); }
static String getLoginPassword() { return String("Admin@123"); }

static String readCookieValue(const String &cookieHeader, const String &name) {
  int start = 0;
  while (start < (int)cookieHeader.length()) {
    int sep = cookieHeader.indexOf(';', start);
    if (sep < 0) sep = cookieHeader.length();
    String pair = cookieHeader.substring(start, sep);
    pair.trim();
    int eq = pair.indexOf('=');
    if (eq > 0) {
      String key = pair.substring(0, eq);
      String val = pair.substring(eq + 1);
      key.trim(); val.trim();
      if (key == name) return val;
    }
    start = sep + 1;
  }
  return "";
}

static bool isAuthenticated(AsyncWebServerRequest *request) {
  if (gAuthSessionToken.length() == 0) return false;
  if (!request->hasHeader("Cookie")) return false;
  String cookie = request->getHeader("Cookie")->value();
  String value = readCookieValue(cookie, String(AUTH_COOKIE_NAME));
  return value.length() > 0 && value == gAuthSessionToken;
}

static void sendRedirect(AsyncWebServerRequest *request, const char *location) {
  AsyncWebServerResponse *res = request->beginResponse(302);
  res->addHeader("Location", location);
  request->send(res);
}

static void clearAuthCookie(AsyncWebServerRequest *request) {
  gAuthSessionToken = "";
  AsyncWebServerResponse *res = request->beginResponse(302);
  res->addHeader("Location", "/login");
  res->addHeader("Set-Cookie", String(AUTH_COOKIE_NAME) + "=; Path=/; Max-Age=0");
  request->send(res);
}

static String loginPage(const String &prefilledUser, bool badCredentials = false) {
  String err = badCredentials ? "<div class='err'>Invalid ID or password.</div>" : "";
  String page = R"LOG(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Login</title>
<link rel="icon" type="image/png" href="/logo.png?v=2">
<style>
  * { box-sizing: border-box; }
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #eef2f7; font-family: Arial, sans-serif; }
  .panel { width: min(360px, 92vw); background: #fff; border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); padding: 24px; }
  h1 { margin: 0 0 14px; font-size: 20px; color: #1a1a2e; }
  label { display: block; font-size: 13px; color: #444; margin: 10px 0 6px; }
  input { width: 100%; padding: 10px 12px; border: 1px solid #d9dce2; border-radius: 8px; font-size: 14px; }
  .pass-wrap { position: relative; padding-right: 36px; }
  .pass-wrap input { padding-right: 12px; }
  .eye-btn {
    position: absolute; right: 0; top: 50%; transform: translateY(-50%);
    width: 32px; height: 32px; margin-top: 0;
    display: flex; align-items: center; justify-content: center;
    border: 0; background: transparent; cursor: pointer; font-size: 18px;
    line-height: 1; padding: 0; color: #444;
  }
  .eye-btn .eye-icon { position: relative; display: inline-block; }
  .eye-btn .eye-icon svg { width: 20px; height: 20px; display: block; }
  .eye-btn .eye-icon .slash { opacity: 0; transition: opacity 0.12s ease; }
  .eye-btn.eye-off .eye-icon .slash { opacity: 1; }
  button { width: 100%; margin-top: 16px; padding: 10px 12px; border: 0; border-radius: 8px; background: #1565c0; color: #fff; font-weight: 600; cursor: pointer; }
  button:hover { opacity: 0.9; }
  .pass-wrap .eye-btn {
    width: 32px;
    margin-top: 0;
    padding: 0;
    background: transparent;
    border-radius: 0;
    color: #444;
  }
  .err { margin: 8px 0 6px; padding: 10px; border-radius: 8px; background: #ffebee; color: #c62828; border: 1px solid #ef9a9a; font-size: 13px; }
</style>
</head>
<body>
  <form class="panel" method="POST" action="/login" autocomplete="off">
    <h1>RAMS Login</h1>
    __ERROR_BLOCK__
    <label for="user">ID</label>
    <input id="user" name="user" type="text" value="__PREFILLED_USER__" readonly required>
    <label for="pass">Password</label>
    <div class="pass-wrap">
      <input id="pass" name="pass" type="password" required>
      <button class="eye-btn eye-off" type="button" id="togglePass" aria-label="Show password">
        <span class="eye-icon">
          <svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
            <path d="M12 5C7 5 2.73 8.11 1 12c1.73 3.89 6 7 11 7s9.27-3.11 11-7c-1.73-3.89-6-7-11-7z" fill="none" stroke="currentColor" stroke-width="1.5" />
            <circle cx="12" cy="12" r="3" fill="currentColor" />
            <path class="slash" d="M3 3l18 18" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" />
          </svg>
        </span>
      </button>
    </div>
    <button type="submit">Login</button>
  </form>
<script>
  (function() {
    var pass = document.getElementById('pass');
    var btn = document.getElementById('togglePass');
    if (!pass || !btn) return;
    btn.addEventListener('click', function() {
      var show = pass.type === 'password';
      pass.type = show ? 'text' : 'password';
      btn.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
      if (show) btn.classList.remove('eye-off');
      else btn.classList.add("eye-off");
    });
  })();
</script>
</body>
</html>
)LOG";
  page.replace("__ERROR_BLOCK__", err);
  page.replace("__PREFILLED_USER__", prefilledUser);
  return page;
}

static String readSerialNumber() {
  String serial = "";
  if (!Shared_lockFileSystem()) return serial;
  if (LittleFS.exists(SERIAL_FILE_PATH)) {
    File f = LittleFS.open(SERIAL_FILE_PATH, "r");
    if (f) {
      serial = f.readStringUntil('\n');
      serial.trim();
      f.close();
    }
  }
  Shared_unlockFileSystem();
  return serial;
}

static bool isSerialFormatValid(const String &serial) {
  if (serial.length() < 3 || serial.length() > 32) return false;
  for (size_t i = 0; i < serial.length(); ++i) {
    char c = serial.charAt(i);
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool writeSerialNumberOnce(const String &serial, String &error) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(2000))) {
    error = "File system busy";
    return false;
  }

  if (LittleFS.exists(SERIAL_FILE_PATH)) {
    File existing = LittleFS.open(SERIAL_FILE_PATH, "r");
    if (existing) {
      String current = existing.readStringUntil('\n');
      current.trim();
      existing.close();
      if (current.length() > 0) {
        Shared_unlockFileSystem();
        error = "Serial number already set";
        return false;
      }
    }
  }

  File out = LittleFS.open(SERIAL_FILE_PATH, "w");
  if (!out) {
    Shared_unlockFileSystem();
    error = "Failed to open serial file";
    return false;
  }
  out.println(serial);
  out.close();
  Shared_unlockFileSystem();
  return true;
}

static String serialNumberPage(const String &currentSerial, const String &message, bool okMessage) {
  String status = "";
  if (message.length() > 0) {
    status = "<div class='status " + String(okMessage ? "ok" : "err") + "'>" + message + "</div>";
  }

  String formBlock = "";
  if (currentSerial.length() > 0) {
    formBlock = "<div class='locked'>Serial Number is <strong>" + currentSerial + "</strong></div>";
  } else {
    formBlock = R"SER(
      <form method="POST" action="/serialnumber/">
        <label for="serial">Serial Number</label>
        <input id="serial" name="serial" type="text" placeholder="e.g. RAMS001" required maxlength="32" pattern="[A-Za-z0-9_-]+">
        <button type="submit">Set Serial Number</button>
      </form>
    )SER";
  }

  String page = R"SERIAL(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Device Serial Number</title>
<link rel="icon" type="image/png" href="/logo.png?v=2">
<style>
  * { box-sizing: border-box; }
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #eef2f7; font-family: Arial, sans-serif; padding: 14px; }
  .panel { width: min(460px, 96vw); background: #fff; border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); padding: 24px; }
  h1 { margin: 0 0 14px; font-size: 20px; color: #1a1a2e; }
  label { display: block; font-size: 13px; color: #444; margin: 10px 0 6px; }
  input { width: 100%; padding: 10px 12px; border: 1px solid #d9dce2; border-radius: 8px; font-size: 14px; }
  button { margin-top: 14px; padding: 10px 14px; border: 0; border-radius: 8px; background: #1565c0; color: #fff; font-weight: 600; cursor: pointer; }
  .status { margin: 10px 0 12px; padding: 10px; border-radius: 8px; font-size: 13px; }
  .status.ok { background: #e8f5e9; color: #2e7d32; border: 1px solid #a5d6a7; }
  .status.err { background: #ffebee; color: #c62828; border: 1px solid #ef9a9a; }
  .locked { margin: 12px 0; padding: 12px; border-radius: 8px; background: #f1f8e9; border: 1px solid #c5e1a5; color: #33691e; }
  .links { margin-top: 16px; }
  .links a { color: #1565c0; text-decoration: none; margin-right: 14px; font-size: 14px; }
</style>
</head>
<body>
 <div class="panel">
    <h1>Device Serial Number</h1>
    <p>This value can be written once. After setting, it becomes read-only.</p>
    __STATUS_BLOCK__
    __FORM_BLOCK__
    <div class="links">
      <a href="/">Back to Dashboard</a>
      <a href="/logout">Logout</a>
    </div>
  </div>
</body>
</html>
)SERIAL";
  page.replace("__STATUS_BLOCK__", status);
  page.replace("__FORM_BLOCK__", formBlock);
  return page;
}

static String digitalInputsPage() {
  return R"DIg(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Digital Inputs</title>
<link rel="icon" type="image/png" href="/logo.png?v=2">
<style>
body{font-family:Arial, sans-serif; margin:16px; background:#f3f5f7}
.panel{max-width:900px;margin:0 auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 6px 20px rgba(15,23,42,0.06)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;align-items:start}
.field{margin-bottom:12px;display:block}
.field label{display:block;font-size:13px;color:#384047;margin-bottom:6px}
.field input[type=text],.field select,.field input[type=number],.field textarea{width:100%;box-sizing:border-box;padding:10px;border:1px solid #d7dee8;border-radius:6px;font-size:14px}
.field textarea{min-height:64px;resize:vertical}
#formArea{overflow:auto}
.btn{display:inline-block;padding:10px 14px;border-radius:8px;border:0;background:#1565c0;color:#fff;font-weight:700;cursor:pointer}
.muted{background:#e9eef7;color:#247}
.small{font-size:13px;color:#5b6b78}
@media(max-width:700px){.grid{grid-template-columns:1fr}.panel{padding:14px}}
</style>
</head>
<body>
<div class="panel">
  <h2>Digital Inputs Configuration</h2>
  <p class="small">Select input number, configure settings and save. Contacts are loaded from Phone Number Management.</p>

  <div class="field"><label>Digital Input No.</label>
    <select id="diSelect"><option value="0">DI1</option><option value="1">DI2</option><option value="2">DI3</option><option value="3">DI4</option></select>
  </div>

  <div id="formArea">
    <div class="grid">
      <div>
        <div class="field"><label>Enable</label><input type="checkbox" id="di-enabled"></div>
        <div class="field"><label>Input Name</label><input type="text" id="di-name"></div>
        <div class="field"><label>Input Type</label><select id="di-type"><option value="NO">Normally Open</option><option value="NC">Normally Closed</option></select></div>
        <div class="field"><label>Contact (recipient)</label><select id="di-contact"><option value="">-- none --</option></select></div>
      </div>
      <div>
        <div class="field"><label>Time To Alarm (seconds)</label><input type="number" id="di-tta" min="0"></div>
        <div class="field"><label>Time To Return (seconds)</label><input type="number" id="di-ttr" min="0"></div>
        <div class="field"><label><input type="checkbox" id="di-alarmSms"> Alarm SMS Notification</label></div>
        <div class="field"><label><input type="checkbox" id="di-returnSms"> Return SMS Notification</label></div>
      </div>
    </div>

    <div class="field"><label>Alarm Message</label><textarea id="di-alarmMsg" rows="2"></textarea></div>
    <div class="field"><label>Return Message</label><textarea id="di-returnMsg" rows="2"></textarea></div>
  </div>

  <div style="margin-top:12px">
    <button id="saveBtn" class="btn">Save Input</button>
    <button id="resetBtn" class="btn muted" style="margin-left:8px">Reset Selected</button>
    <a href="/" style="margin-left:12px">Back to Dashboard</a>
  </div>

  <div id="status" style="margin-top:12px"></div>
</div>

<script>
let recipients = [];
let cfgArr = [];
let currentIndex = 0;

async function loadRecipients(){
  try { const res = await fetch('/api/recipients'); if (!res.ok) return []; return await res.json(); } catch(e){ return []; }
}

function defaultForIndex(i){
  return { enabled:false, name: 'DI'+(i+1), type:'NO', tta:5, ttr:3, alarmSms:false, returnSms:false, alarmMessage:'Alarm on DI'+(i+1), returnMessage:'Return on DI'+(i+1), contactId: '' };
}

async function loadConfig(){
  try { const r = await fetch('/api/digital-inputs'); if (r.ok) cfgArr = await r.json(); else cfgArr = []; } catch(e){ cfgArr = []; }
  // ensure length 4
  for(let i=0;i<4;i++) if (!cfgArr[i]) cfgArr[i] = defaultForIndex(i);
}

function populateRecipientsSelect(){
  const sel = document.getElementById('di-contact'); sel.innerHTML = '<option value="">-- none --</option>';
  recipients.forEach(r=>{ const opt = document.createElement('option'); opt.value = r.id || r.phone || r.name; opt.textContent = (r.name? (r.name + ' - ' + (r.phone||'')) : (r.phone||r.id)); sel.appendChild(opt); });
}

function renderForIndex(i){
  currentIndex = i;
  const cfg = cfgArr[i] || defaultForIndex(i);
  document.getElementById('di-enabled').checked = !!cfg.enabled;
  document.getElementById('di-name').value = cfg.name || '';
  document.getElementById('di-type').value = cfg.type || 'NO';
  document.getElementById('di-tta').value = cfg.tta || 0;
  document.getElementById('di-ttr').value = cfg.ttr || 0;
  document.getElementById('di-alarmSms').checked = !!cfg.alarmSms;
  document.getElementById('di-returnSms').checked = !!cfg.returnSms;
  document.getElementById('di-alarmMsg').value = cfg.alarmMessage || '';
  document.getElementById('di-returnMsg').value = cfg.returnMessage || '';
  document.getElementById('di-contact').value = cfg.contactId || '';
  document.getElementById('status').textContent = '';
}

function collectSingle(){
  return {
    enabled: !!document.getElementById('di-enabled').checked,
    name: document.getElementById('di-name').value || ('DI'+(currentIndex+1)),
    type: document.getElementById('di-type').value || 'NO',
    tta: parseInt(document.getElementById('di-tta').value) || 0,
    ttr: parseInt(document.getElementById('di-ttr').value) || 0,
    alarmSms: !!document.getElementById('di-alarmSms').checked,
    returnSms: !!document.getElementById('di-returnSms').checked,
    alarmMessage: document.getElementById('di-alarmMsg').value || '',
    returnMessage: document.getElementById('di-returnMsg').value || '',
    contactId: document.getElementById('di-contact').value || ''
  };
}

document.getElementById('diSelect').addEventListener('change', function(e){ renderForIndex(parseInt(e.target.value)); });

document.getElementById('saveBtn').addEventListener('click', async function(){
  const obj = collectSingle(); cfgArr[currentIndex] = obj;
  document.getElementById('status').textContent = 'Saving...';
  try {
    const res = await fetch('/api/digital-inputs', { method:'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(cfgArr) });
    if (res.ok) document.getElementById('status').textContent = 'Saved.';
    else document.getElementById('status').textContent = 'Save failed';
  } catch(e){ document.getElementById('status').textContent = 'Save error'; }
});

document.getElementById('resetBtn').addEventListener('click', function(){
  if (!confirm('Reset selected input to defaults?')) return; cfgArr[currentIndex] = defaultForIndex(currentIndex); renderForIndex(currentIndex); document.getElementById('status').textContent = 'Reset to defaults (not saved).';
});

async function init(){ recipients = await loadRecipients(); await loadConfig(); populateRecipientsSelect(); renderForIndex(0); }
init();
</script>
</body>
</html>
)DIg";
 }

static String phoneNumbersPage() {
  return R"PHN(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Phone Number Management</title>
<link rel="icon" type="image/png" href="/logo.png?v=2">
<style>
body{font-family:Arial, sans-serif; margin:16px; background:#f3f5f7}
.panel{max-width:900px;margin:0 auto;background:#fff;padding:18px;border-radius:10px;box-shadow:0 6px 20px rgba(15,23,42,0.06)}
.list{border:1px solid #e6eef8;padding:12px;border-radius:8px;background:#fbfeff}
.row{display:flex;gap:8px;align-items:center;margin-bottom:8px}
.row input{flex:1;padding:8px;border:1px solid #d7dee8;border-radius:6px}
.btn{padding:8px 12px;border-radius:8px;border:0;background:#1565c0;color:#fff;cursor:pointer}
.muted{background:#e9eef7;color:#247}
.small{font-size:13px;color:#5b6b78}
</style>
</head>
<body>
<div class="panel">
  <h2>Phone Number Management</h2>
  <p class="small">Add contact name and phone. These appear in Digital Input contact dropdowns.</p>
  <div id="list" class="list"></div>
  <div style="margin-top:12px">
    <button id="addBtn" class="btn">Add Contact</button>
    <button id="saveBtn" class="btn" style="margin-left:8px">Save All</button>
    <a href="/" style="margin-left:12px">Back to Dashboard</a>
  </div>
  <div id="status" style="margin-top:12px"></div>
</div>
<script>
let items = [];
function elt(tag, props, ...children){ const e=document.createElement(tag); if(props) Object.keys(props).forEach(k=>e[k]=props[k]); children.forEach(c=>{ if(typeof c==='string') e.appendChild(document.createTextNode(c)); else e.appendChild(c); }); return e; }

function renderList(){
  const list = document.getElementById('list'); list.innerHTML='';
  items.forEach((it, idx)=>{
    const row = elt('div',{className:'row'});
    const name = elt('input',{value:it.name || '', placeholder:'Name'});
    const phone = elt('input',{value:it.phone || '', placeholder:'Phone'});
    const del = elt('button',{className:'btn muted'}, 'Delete');
    del.addEventListener('click', ()=>{ if(confirm('Delete contact?')){ items.splice(idx,1); renderList(); } });
    name.addEventListener('input', ()=> items[idx].name = name.value);
    phone.addEventListener('input', ()=> items[idx].phone = phone.value);
    row.appendChild(name); row.appendChild(phone); row.appendChild(del);
    list.appendChild(row);
  });
}

document.getElementById('addBtn').addEventListener('click', ()=>{ items.push({name:'',phone:''}); renderList(); });

document.getElementById('saveBtn').addEventListener('click', async ()=>{
  document.getElementById('status').textContent = 'Saving...';
  try{
    const res = await fetch('/api/recipients', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(items) });
    if(res.ok) document.getElementById('status').textContent = 'Saved.'; else document.getElementById('status').textContent = 'Save failed';
  }catch(e){ document.getElementById('status').textContent = 'Save error'; }
});

async function load(){
  try{ const r = await fetch('/api/recipients'); if(r.ok) items = await r.json(); else items = []; }catch(e){ items = []; }
  if(!Array.isArray(items)) items = [];
  renderList();
}
load();
</script>
</body>
</html>
)PHN";
}

static void setupWebServerRoutes() {
  if (serverRoutesSetup) return;

  // Serve project logo from LittleFS. Put your logo at the project 'data' folder as `logo.png` and upload to LittleFS.
  server.on("/logo.png", HTTP_GET, [](AsyncWebServerRequest *request) {
    // prefer Gaurangalogo.png if present in LittleFS
    if (LittleFS.exists("/Gaurangalogo.png")) request->send(LittleFS, "/Gaurangalogo.png", "image/png");
    else request->send(LittleFS, "/logo.png", "image/png");
  });
  // Also serve as favicon for browsers (some browsers request /favicon.ico)
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/Gaurangalogo.png")) request->send(LittleFS, "/Gaurangalogo.png", "image/png");
    else request->send(LittleFS, "/logo.png", "image/png");
  });

  server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (isAuthenticated(request)) { sendRedirect(request, "/"); return; }
    bool bad = request->hasParam("err");
    request->send(200, "text/html", loginPage(getLoginUsername(), bad));
  });

  server.on("/login", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";
    String expectedUser = getLoginUsername();
    String expectedPass = getLoginPassword();
    if (user == expectedUser && pass == expectedPass) {
      gAuthSessionToken = makeSessionToken();
      AsyncWebServerResponse *res = request->beginResponse(302);
      res->addHeader("Location", "/");
      res->addHeader("Set-Cookie", String(AUTH_COOKIE_NAME) + "=" + gAuthSessionToken + "; Path=/; Max-Age=86400; HttpOnly; SameSite=Strict");
      request->send(res);
      return;
    }
    sendRedirect(request, "/login?err=1");
  });

  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest *request) { clearAuthCookie(request); });

  server.on("/serialnumber", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    String serial = readSerialNumber();
    request->send(200, "text/html", serialNumberPage(serial, "", true));
  });

  server.on("/serialnumber/", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { request->send(401, "text/plain", "Unauthorized"); return; }
    String serial = request->hasParam("serial", true) ? request->getParam("serial", true)->value() : "";
    serial.trim();
    if (!isSerialFormatValid(serial)) {
      String current = readSerialNumber();
      request->send(400, "text/html", serialNumberPage(current, "Invalid serial format.", false));
      return;
    }
    String error="";
    if (!writeSerialNumberOnce(serial, error)) {
      String current = readSerialNumber();
      request->send(409, "text/html", serialNumberPage(current, error, false));
      return;
    }
    String current = readSerialNumber();
    request->send(200, "text/html", serialNumberPage(current, "Serial saved.", true));
  });

  // Dashboard API
  server.on("/api/dashboard", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    String serial = readSerialNumber(); if (serial.length()==0) serial = "Not Set";
    String apIp = WiFi.softAPIP().toString(); if (apIp=="0.0.0.0") apIp = "AP Mode Off (10.10.10.10 when enabled)";
    String body = "{";
    body += "\"serial_number\":\"" + serial + "\",";
    body += "\"ap_ip\":\"" + apIp + "\",";
    body += "\"tcp_endpoint\":\"-\",";
    body += "\"active_alarms\":0,";
    body += "\"acknowledged_alarms\":0,";
    body += "\"unacknowledged_alarms\":0,";
    body += "\"authorized_numbers\":0,";
    body += "\"event_recipients\":0";
    body += "}";
    request->send(200, "application/json", body);
  });

  // Return list of saved recipients (phone numbers)
  server.on("/api/recipients", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    String out = "[]";
    if (Shared_lockFileSystem()) {
      if (LittleFS.exists("/recipients.json")) {
        File f = LittleFS.open("/recipients.json", "r");
        if (f) { out = f.readString(); f.close(); }
      }
      Shared_unlockFileSystem();
    }
    request->send(200, "application/json", out);
  });

  // Save recipients (replace entire list)
  server.on("/api/recipients", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); gRequestBodyRecipients.clear(); return; }
    if (gRequestBodyRecipients.length() == 0) { request->send(400, "application/json", "{\"error\":\"Empty body\"}"); return; }
    bool ok = false;
    if (Shared_lockFileSystem()) {
      File f = LittleFS.open("/recipients.json", "w");
      if (f) { f.print(gRequestBodyRecipients); f.close(); ok = true; }
      Shared_unlockFileSystem();
    }
    gRequestBodyRecipients.clear();
    if (ok) request->send(200, "application/json", "{\"status\":\"ok\"}");
    else request->send(500, "application/json", "{\"error\":\"Failed to save\"}");
  }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    if (index == 0) gRequestBodyRecipients = "";
    gRequestBodyRecipients.concat((const char*)data, len);
  });

  // Digital inputs config GET
  server.on("/api/digital-inputs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    String out = "";
    if (Shared_lockFileSystem()) {
      if (LittleFS.exists("/digital_inputs.json")) {
        File f = LittleFS.open("/digital_inputs.json", "r");
        if (f) { out = f.readString(); f.close(); }
      }
      Shared_unlockFileSystem();
    }
    if (out.length() == 0) {
      // Default 4 inputs
      out = R"RAW([
  {"enabled":false,"name":"DI1","type":"NO","tta":5,"ttr":3,"alarmSms":false,"returnSms":false,"alarmMessage":"Alarm on DI1","returnMessage":"Return on DI1","contactId":""},
  {"enabled":false,"name":"DI2","type":"NO","tta":5,"ttr":3,"alarmSms":false,"returnSms":false,"alarmMessage":"Alarm on DI2","returnMessage":"Return on DI2","contactId":""},
  {"enabled":false,"name":"DI3","type":"NO","tta":5,"ttr":3,"alarmSms":false,"returnSms":false,"alarmMessage":"Alarm on DI3","returnMessage":"Return on DI3","contactId":""},
  {"enabled":false,"name":"DI4","type":"NO","tta":5,"ttr":3,"alarmSms":false,"returnSms":false,"alarmMessage":"Alarm on DI4","returnMessage":"Return on DI4","contactId":""}
])RAW";
    }
    request->send(200, "application/json", out);
  });

  // Digital inputs config POST (body received in onBody)
  server.on("/api/digital-inputs", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); gRequestBody.clear(); return; }
    // Save last received body
    if (gRequestBody.length() == 0) { request->send(400, "application/json", "{\"error\":\"Empty body\"}"); return; }
    bool ok = false;
    if (Shared_lockFileSystem()) {
      File f = LittleFS.open("/digital_inputs.json", "w");
      if (f) { f.print(gRequestBody); f.close(); ok = true; }
      Shared_unlockFileSystem();
    }
    gRequestBody.clear();
    if (ok) request->send(200, "application/json", "{\"status\":\"ok\"}");
    else request->send(500, "application/json", "{\"error\":\"Failed to save\"}");
  }, nullptr, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    // append body data (single-thread assumption)
    if (index == 0) gRequestBody = "";
    gRequestBody.concat((const char*)data, len);
  });

  // Dashboard page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", htmlPage());
  });

  // Quick navigation placeholders
  auto makePlaceholder = [](const char *title, const char *desc) {
    String s = "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>";
    s += title;
    s += "</title></head><body style=\"font-family:Arial, sans-serif;padding:18px\"><h1>";
    s += title;
    s += "</h1><p>";
    s += desc;
    s += "</p><p><a href=\"/\">Back to Dashboard</a></p></body></html>";
    return s;
  };

  server.on("/digital-inputs", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", digitalInputsPage());
  });

  server.on("/analog-inputs", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", makePlaceholder("Analog Inputs","Configure and view analog inputs (scaling, set/reset, alarms)"));
  });

  server.on("/relay-outputs", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", makePlaceholder("Relay Outputs","Configure relay outputs, default states, alarm links") );
  });

  server.on("/alarm-management", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", makePlaceholder("Alarm Management","View active alarms, ack/return actions") );
  });

  server.on("/phone-numbers", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", phoneNumbersPage() );
  });

  server.on("/network-config", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", makePlaceholder("Network Configuration","Configure DHCP/static IP and AP parameters") );
  });

  server.on("/event-logs", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", makePlaceholder("Event Logs","View stored event log entries") );
  });

  server.on("/diagnostics", HTTP_GET, [=](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { sendRedirect(request, "/login"); return; }
    request->send(200, "text/html", makePlaceholder("Diagnostics","Device, network and modem diagnostics") );
  });

  serverRoutesSetup = true;
}

static void startAPMode() {
  if (Shared_isAPModeActive()) return;

  Serial.println("[AP] Starting Access Point...");
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  IPAddress apIP(10,10,10,10);
  IPAddress gateway(10,10,10,10);
  IPAddress subnet(255,255,255,0);
  WiFi.softAPConfig(apIP,gateway,subnet);
  String ssid = readSerialNumber();
  if (ssid.length()==0) ssid = "MSys"; else ssid = "MSys-" + ssid;
  WiFi.softAP(ssid.c_str(), AP_PASS_FIXED);
  delay(200);
  Serial.print("[AP] SSID: "); Serial.println(ssid);
  Serial.print("[AP] AP IP address: "); Serial.println(WiFi.softAPIP());
  Shared_setAPModeActive(true);
  digitalWrite(AP_STATUS_LED_PIN, HIGH);
  Serial.println("[AP] Access Point is now active");
}

static void stopAPMode() {
  if (!Shared_isAPModeActive()) return;
  Serial.println("[AP] Stopping Access Point...");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  delay(200);
  Shared_setAPModeActive(false);
  digitalWrite(AP_STATUS_LED_PIN, LOW);
  Serial.println("[AP] Access Point is now disabled");
}

void AP_taskLoop(void *pvParameters) {
  (void)pvParameters;
  unsigned long lastStateChange = 0;
  WiFi.mode(WIFI_STA);
  delay(50);

  // Ensure filesystem ready
  Shared_lockFileSystem();

  setupWebServerRoutes();
  if (!serverStarted) { server.begin(); serverStarted = true; Serial.println("[WEB] Config server started on port 80"); }

  for (;;) {
    bool switchState = digitalRead(BUTTON_PIN);
    unsigned long now = millis();
    if (now - lastStateChange > BUTTON_DEBOUNCE_MS) {
      if (switchState == LOW && !Shared_isAPModeActive()) { startAPMode(); lastStateChange = now; }
      else if (switchState == HIGH && Shared_isAPModeActive()) { stopAPMode(); lastStateChange = now; }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
