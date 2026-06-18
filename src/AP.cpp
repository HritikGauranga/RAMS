#include "AP.h"
#include "Shared.h"
#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>
#include <stdlib.h>

static AsyncWebServer server(80);
static bool          serverStarted     = false;
static bool          serverRoutesSetup = false;
static const char    *WEBUI_USER        = "Admin";
static const char    *WEBUI_PASS        = "Admin@123";
static const char    *AP_PASS_FIXED     = "MSys@1234";
static const char    *AUTH_COOKIE_NAME  = "MSMSG_AUTH";
static const char    *SERIAL_FILE_PATH  = "/serialnumber.txt";
static const char    *SERIAL_META_PATH  = "/serial_meta.txt";
// MB map CSV removed for RAMS; configuration will be handled via Web UI
static String         gAuthSessionToken = "";
#ifndef FW_BUILD_TAG
#define FW_BUILD_TAG __DATE__ " " __TIME__
#endif
static const char *FW_BUILD_TAG_VALUE = FW_BUILD_TAG;

static String htmlPage();
static void setupWebServerRoutes();
static void startAPMode();
static void stopAPMode();

static String makeSessionToken() {
  char buf[33] = {};
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  uint32_t c = esp_random();
  uint32_t d = esp_random();
  snprintf(buf, sizeof(buf), "%08lx%08lx%08lx%08lx",
           (unsigned long)a,
           (unsigned long)b,
           (unsigned long)c,
           (unsigned long)d);
  return String(buf);
}

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
      key.trim();
      val.trim();
      if (key == name) return val;
    }
    start = sep + 1;
  }
  return "";
}

// Local helpers used by contact parsing (mirror Shared.cpp validation)
static String trimCopy(const String &value) {
  String copy = value;
  copy.trim();
  return copy;
}

static bool isValidPhoneFormat(const String &number) {
  String trimmed = number;
  trimmed.trim();
  if (trimmed.length() == 0) return false;
  if (trimmed.length() > PHONE_NUMBER_LENGTH - 1) return false;
  if (trimmed.charAt(0) == '+') {
    size_t digitCount = trimmed.length() - 1;
    if (digitCount < 10 || digitCount > 15) return false;
    for (size_t i = 1; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  } else {
    size_t digitCount = trimmed.length();
    if (digitCount < 10 || digitCount > 15) return false;
    for (size_t i = 0; i < trimmed.length(); ++i) {
      char c = trimmed.charAt(i);
      if (c < '0' || c > '9') return false;
    }
    return true;
  }
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
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Login</title>
<link rel="icon" type="image/png" href="/Gaurangalogo.png?v=2">
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
  .eye-btn.eye-off .eye-icon::after {
    content: "";
    position: absolute;
    left: -1px;
    right: -1px;
    top: 50%;
    height: 2px;
    background: currentColor;
    transform: rotate(-35deg);
    transform-origin: center;
  }
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
    <h1>MB Map Config Login</h1>
    __ERROR_BLOCK__
    <label for="user">ID</label>
    <input id="user" name="user" type="text" value="__PREFILLED_USER__" readonly required>
    <label for="pass">Password</label>
    <div class="pass-wrap">
      <input id="pass" name="pass" type="password" required>
      <button class="eye-btn eye-off" type="button" id="togglePass" aria-label="Show password"><span class="eye-icon">&#128065;</span></button>
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
)rawliteral";
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

static String getAPSSID() {
  String serial = readSerialNumber();
  serial.trim();
  if (serial.length() == 0) return "MSys";
  return "MSys-" + serial;
}

static String readSerialMeta() {
  String meta = "";
  if (!Shared_lockFileSystem()) return meta;
  if (LittleFS.exists(SERIAL_META_PATH)) {
    File f = LittleFS.open(SERIAL_META_PATH, "r");
    if (f) {
      meta = f.readStringUntil('\n');
      meta.trim();
      f.close();
    }
  }
  Shared_unlockFileSystem();
  return meta;
}

static void clearStaleSerialForNewBuild() {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(2000))) {
    Serial.println("[SERIAL] File system busy, skipping stale serial cleanup");
    return;
  }

  String serial = "";
  String meta = "";

  if (LittleFS.exists(SERIAL_FILE_PATH)) {
    File sf = LittleFS.open(SERIAL_FILE_PATH, "r");
    if (sf) {
      serial = sf.readStringUntil('\n');
      serial.trim();
      sf.close();
    }
  }

  if (LittleFS.exists(SERIAL_META_PATH)) {
    File mf = LittleFS.open(SERIAL_META_PATH, "r");
    if (mf) {
      meta = mf.readStringUntil('\n');
      meta.trim();
      mf.close();
    }
  }

  bool hasPreviousSerial = (serial.length() > 0);
  bool isCurrentBuildMeta = (meta == String(FW_BUILD_TAG_VALUE));

  if (hasPreviousSerial && !isCurrentBuildMeta) {
    LittleFS.remove(SERIAL_FILE_PATH);
    LittleFS.remove(SERIAL_META_PATH);
    Serial.println("[SERIAL] Cleared stale serial for new firmware build");
  }

  Shared_unlockFileSystem();
}

static String getLoginUsername() {
  return String(WEBUI_USER);
}

static String getLoginPassword() {
  return String(WEBUI_PASS);
}

static bool isSerialLockedForCurrentBuild() {
  String serial = readSerialNumber();
  if (serial.length() == 0) return false;
  String meta = readSerialMeta();
  return meta == String(FW_BUILD_TAG_VALUE);
}

static bool isSerialFormatValid(const String &serial) {
  if (serial.length() < 3 || serial.length() > 32) return false;

  for (size_t i = 0; i < serial.length(); ++i) {
    char c = serial.charAt(i);
    bool ok =
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool writeSerialNumberOnce(const String &serial, String &error) {
  if (!Shared_lockFileSystem(pdMS_TO_TICKS(2000))) {
    error = "File system busy";
    return false;
  }

  bool lockedForThisBuild = false;
  if (LittleFS.exists(SERIAL_FILE_PATH)) {
    File existing = LittleFS.open(SERIAL_FILE_PATH, "r");
    if (existing) {
      String current = existing.readStringUntil('\n');
      current.trim();
      existing.close();
      if (current.length() > 0) {
        String meta = "";
        if (LittleFS.exists(SERIAL_META_PATH)) {
          File m = LittleFS.open(SERIAL_META_PATH, "r");
          if (m) {
            meta = m.readStringUntil('\n');
            meta.trim();
            m.close();
          }
        }
        lockedForThisBuild = (meta == String(FW_BUILD_TAG_VALUE));
      }
      if (current.length() > 0 && lockedForThisBuild) {
        Shared_unlockFileSystem();
        error = "Serial number already set and locked for this firmware build";
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

  File meta = LittleFS.open(SERIAL_META_PATH, "w");
  if (!meta) {
    Shared_unlockFileSystem();
    error = "Serial saved but failed to lock metadata";
    return false;
  }
  meta.println(FW_BUILD_TAG_VALUE);
  meta.close();

  Shared_unlockFileSystem();
  return true;
}

static String serialNumberPage(const String &currentSerial, const String &message, bool okMessage) {
  String status = "";
  if (message.length() > 0) {
    status = "<div class='status " + String(okMessage ? "ok" : "err") + "'>" + message + "</div>";
  }

  String formBlock = "";
  if (currentSerial.length() > 0 && isSerialLockedForCurrentBuild()) {
    formBlock = "<div class='locked'>Serial Number is <strong>" + currentSerial + "</strong></div>";
  } else if (currentSerial.length() > 0) {
    formBlock = "<div class='status ok'>Previous serial found: <strong>" + currentSerial + "</strong>. You can overwrite it once for this new firmware upload.</div>" + String(R"rawliteral(
      <form method="POST" action="/serialnumber/">
        <label for="serial">Serial Number</label>
        <input id="serial" name="serial" type="text" required maxlength="32" pattern="[A-Za-z0-9_-]+">
        <button type="submit">Save Serial Number</button>
      </form>
    )rawliteral");
  } else {  
    formBlock = R"rawliteral(
      <form method="POST" action="/serialnumber/">
        <label for="serial">Serial Number</label>
        <input id="serial" name="serial" type="text" placeholder="e.g. RAMS0001" required maxlength="32" pattern="[A-Za-z0-9_-]+">
        <button type="submit">Set Serial Number</button>
      </form>
    )rawliteral";
  }

  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Device Serial Number</title>
<link rel="icon" type="image/png" href="/Gaurangalogo.png?v=2">
<style>
  * { box-sizing: border-box; }
  body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #eef2f7; font-family: Arial, sans-serif; padding: 14px; }
  .panel { width: min(460px, 96vw); background: #fff; border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,0.08); padding: 24px; }
  h1 { margin: 0 0 14px; font-size: 20px; color: #1a1a2e; }
  p { margin: 0 0 12px; color: #555; font-size: 14px; }
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
      <a href="/">Back to Config</a>
      <a href="/logout">Logout</a>
    </div>
  </div>
</body>
</html>
)rawliteral";

  page.replace("__STATUS_BLOCK__", status);
  page.replace("__FORM_BLOCK__", formBlock);
  return page;
}

static String ipBytesToString(const uint8_t ip[4]) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

static String escapeJson(const String &s) {
  String out;
  out.reserve(s.length() * 2);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

static bool parseIPFromText(const String &src, uint8_t out[4]) {
  int parts[4] = {0, 0, 0, 0};
  int p = 0;
  String token = "";
  for (size_t i = 0; i < src.length(); ++i) {
    char c = src.charAt(i);
    if (c == '.') {
      if (p > 2 || token.length() == 0) return false;
      parts[p++] = token.toInt();
      token = "";
      continue;
    }
    if (c < '0' || c > '9') return false;
    token += c;
  }
  if (p != 3 || token.length() == 0) return false;
  parts[3] = token.toInt();
  for (int i = 0; i < 4; ++i) {
    if (parts[i] < 0 || parts[i] > 255) return false;
    out[i] = (uint8_t)parts[i];
  }
  return true;
}

static String gatewaySettingsPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Gateway Settings</title>
<link rel="icon" type="image/png" href="/Gaurangalogo.png?v=2">
<style>
  *{box-sizing:border-box}
  body{font-family:Arial,sans-serif;background:#f3f5f7;margin:0;padding:12px}
  .card{max-width:860px;margin:auto;background:#fff;border-radius:10px;padding:16px;box-shadow:0 8px 26px rgba(0,0,0,.08)}
  h1{margin:0 0 12px;font-size:20px}
  .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
  .grid>div{min-width:0}
  label{display:block;font-size:13px;color:#333;margin-bottom:6px}
  input,select{width:100%;max-width:100%;padding:8px;border:1px solid #cfd8dc;border-radius:7px}
  .full{grid-column:1/-1}
  .row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
  .btn{padding:10px 14px;border:0;border-radius:8px;background:#1565c0;color:#fff;font-weight:600;cursor:pointer}
  .muted{font-size:12px;color:#666}
  .status{margin:10px 0;padding:10px;border-radius:8px;display:none}
  .ok{display:block;background:#e8f5e9;color:#2e7d32}
  .err{display:block;background:#ffebee;color:#c62828}
  @media (max-width:640px){
    body{padding:10px}
    .card{padding:12px}
    h1{font-size:18px}
    .grid{grid-template-columns:1fr;gap:10px}
    .btn{width:100%}
  }
</style>
</head>
<body>
<div class="card">
  <h1>Gateway Configuration</h1>
  <div id="status" class="status"></div>
  <div class="grid">
    <div class="full row">
      <input id="useDhcp" type="checkbox" style="width:auto">
      <label for="useDhcp" style="margin:0">Use DHCP for Ethernet</label>
    </div>
    <div><label>Static IP</label><input id="staticIp" placeholder="192.168.8.200" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)"></div>
    <div><label>Subnet Mask</label><input id="subnetMask" placeholder="255.255.255.0" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)"></div>
    <div><label>Gateway IP</label><input id="gatewayIp" placeholder="192.168.8.1" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)"></div>
    <div><label>HTTP Port</label><input id="tcpPort" type="number" min="1" max="65535" inputmode="numeric" oninput="sanitizeNumberInput(this)"></div>
  </div>
  <div class="row" style="margin-top:14px">
    <button class="btn" onclick="saveCfg()">Save Settings</button>
    <a href="/" class="muted">Back to Dashboard</a>
  </div>
  <p class="muted">After save, reboot device to apply network settings.</p>
</div>
<script>
function status(msg, ok){var s=document.getElementById('status');s.textContent=msg;s.className='status '+(ok?'ok':'err');}
function sanitizeIpInput(el){
  if(!el) return;
  var cleaned = el.value.replace(/[^0-9.]/g, '');
  var parts = cleaned.split('.');
  if (parts.length > 4) parts = parts.slice(0, 4);
  for (var i = 0; i < parts.length; i++) {
    if (parts[i].length > 3) parts[i] = parts[i].slice(0, 3);
  }
  el.value = parts.join('.');
}
function sanitizeNumberInput(el){ if(!el) return; el.value = el.value.replace(/[^0-9]/g, ''); if(el.value==='') return; var v=parseInt(el.value,10); if(!Number.isFinite(v)) return; if(v<1) v=1; if(v>65535) v=65535; el.value=String(v); }
function loadCfg(){
  fetch('/api/gateway-settings').then(r=>r.json()).then(c=>{
    document.getElementById('useDhcp').checked=!!c.use_dhcp;
    document.getElementById('staticIp').value=c.static_ip||'';
    document.getElementById('subnetMask').value=c.subnet_mask||'';
    document.getElementById('gatewayIp').value=c.gateway_ip||'';
    document.getElementById('tcpPort').value=c.http_port;
  }).catch(e=>status('Load failed: '+e.message,false));
}
function saveCfg(){
  var p=new URLSearchParams();
  p.append('use_dhcp',document.getElementById('useDhcp').checked?'1':'0');
  p.append('static_ip',document.getElementById('staticIp').value.trim());
  p.append('subnet_mask',document.getElementById('subnetMask').value.trim());
  p.append('gateway_ip',document.getElementById('gatewayIp').value.trim());
  p.append('http_port',document.getElementById('tcpPort').value);
  fetch('/api/gateway-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()})
    .then(r=>r.json()).then(d=>{ if(d.success) status('Saved. Reboot to apply.',true); else status(d.error||'Save failed',false); })
    .catch(e=>status('Save failed: '+e.message,false));
}
loadCfg();
</script>
</body>
</html>
)rawliteral";
}



void printAPStatus() {
  Serial.println("");
  Serial.println("=== AP Mode Info ===");
  Serial.println("To enable AP Mode: Press and hold button on GPIO 33");
  Serial.println("AP status LED: ON when AP mode is active");
  Serial.println("AP SSID: MSys or MSys-<SerialNumber>");
  Serial.println("AP Password: MSys@1234");
  Serial.println("AP URL: http://10.10.10.10");
  Serial.println("Note: AP mode not active by default");
}

// ---------------------------------------------------------------------------
// Build a JSON array of all loaded message configs for the table view.
// Format: [{"no":1,"phones":["081...","","","",""],"text":"ALARM..."},...]
// ---------------------------------------------------------------------------
// Message CSV/table support removed for RAMS firmware

static void setupWebServerRoutes() {
  if (serverRoutesSetup) return;

  server.on("/Gaurangalogo.png", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/Gaurangalogo.png", "image/png");
  });

  server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (isAuthenticated(request)) {
      sendRedirect(request, "/");
      return;
    }
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
      res->addHeader("Set-Cookie",
                     String(AUTH_COOKIE_NAME) + "=" + gAuthSessionToken +
                     "; Path=/; Max-Age=86400; HttpOnly; SameSite=Strict");
      request->send(res);
      return;
    }

    sendRedirect(request, "/login?err=1");
  });

  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest *request) {
    clearAuthCookie(request);
  });

  server.on("/serialnumber", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    String serial = readSerialNumber();
    request->send(200, "text/html", serialNumberPage(serial, "", true));
  });

  server.on("/serialnumber/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    String serial = readSerialNumber();
    request->send(200, "text/html", serialNumberPage(serial, "", true));
  });

  server.on("/serialnumber/", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "text/plain", "Unauthorized");
      return;
    }

    String serial = request->hasParam("serial", true) ? request->getParam("serial", true)->value() : "";
    serial.trim();

    if (!isSerialFormatValid(serial)) {
      String current = readSerialNumber();
      request->send(400, "text/html", serialNumberPage(current, "Invalid serial format. Use only A-Z, a-z, 0-9, _ or - (3-32 chars).", false));
      return;
    }

    String error = "";
    if (!writeSerialNumberOnce(serial, error)) {
      String current = readSerialNumber();
      request->send(409, "text/html", serialNumberPage(current, error, false));
      return;
    }

    String current = readSerialNumber();
    String msg = "Serial number saved and locked successfully.";
    request->send(200, "text/html", serialNumberPage(current, msg, true));
  });

  // Gateway config page and API removed; Network configuration handled under
  // the Network tab in the Web UI which currently fetches /api/dashboard.

  server.on("/api/dashboard", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    GatewaySettings s = {};
    if (!Shared_getGatewaySettings(s)) {
      request->send(500, "application/json", "{\"error\":\"Read failed\"}");
      return;
    }

    String serial = readSerialNumber();
    if (serial.length() == 0) serial = "Not Set";
    String apIp = WiFi.softAPIP().toString();
    if (apIp == "0.0.0.0") apIp = "AP Mode Off (10.10.10.10 when enabled)";
    String ethIp = ETH.localIP().toString();
    if (ethIp == "0.0.0.0") {
      ethIp = ipBytesToString(s.staticIp);
    }

    String body = "{";
    body += "\"serial_number\":\"" + serial + "\",";
    body += "\"login_user\":\"" + getLoginUsername() + "\",";
    body += "\"ap_ip\":\"" + apIp + "\",";
    body += "\"eth_ip\":\"" + ethIp + "\",";
    body += "\"use_dhcp\":" + String(s.useDhcp ? "true" : "false") + ",";
    body += "\"static_ip\":\"" + ipBytesToString(s.staticIp) + "\",";
    body += "\"subnet_mask\":\"" + ipBytesToString(s.subnetMask) + "\",";
    body += "\"gateway_ip\":\"" + ipBytesToString(s.gatewayIp) + "\",";
    body += "\"http_port\":" + String(s.httpPort) + ",";
    body += "\"fw_build\":\"" + String(FW_BUILD_TAG_VALUE) + "\"";
    body += "}";
    request->send(200, "application/json", body);
  });

  // System Config endpoints: store site details and timezone in LittleFS
  server.on("/api/system-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    const char *cfgPath = "/system_config.json";
    String body = "";
    if (!Shared_lockFileSystem()) {
      request->send(500, "application/json", "{\"error\":\"FS busy\"}");
      return;
    }
    if (LittleFS.exists(cfgPath)) {
      File f = LittleFS.open(cfgPath, "r");
      if (f) {
        body = f.readString();
        f.close();
      }
    }
    Shared_unlockFileSystem();
    if (body.length() == 0) {
      body = "{\"site_name\":\"\",\"site_address\":\"\",\"timezone\":\"UTC0\"}";
    }
    request->send(200, "application/json", body);
  });

  server.on("/api/system-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    String site = request->hasParam("site_name", true) ? request->getParam("site_name", true)->value() : "";
    String addr = request->hasParam("site_address", true) ? request->getParam("site_address", true)->value() : "";
    String tz = request->hasParam("timezone", true) ? request->getParam("timezone", true)->value() : "";
    site.trim(); addr.trim(); tz.trim();

    String json = "{\"site_name\":\"" + escapeJson(site) + "\",\"site_address\":\"" + escapeJson(addr) + "\",\"timezone\":\"" + escapeJson(tz) + "\"}";

    if (!Shared_lockFileSystem(pdMS_TO_TICKS(2000))) {
      request->send(500, "application/json", "{\"error\":\"FS busy\"}");
      return;
    }
    File f = LittleFS.open("/system_config.json", "w");
    if (!f) {
      Shared_unlockFileSystem();
      request->send(500, "application/json", "{\"error\":\"Write failed\"}");
      return;
    }
    f.print(json);
    f.close();
    Shared_unlockFileSystem();
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/restart-ntp", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    String tz = request->hasParam("timezone", true) ? request->getParam("timezone", true)->value() : "";
    if (tz.length() > 0) {
      setenv("TZ", tz.c_str(), 1);
      tzset();
    }
    // Restart SNTP/NTP client
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    request->send(200, "application/json", "{\"success\":true}");
  });

  // Contact endpoints (authorized & recipients)
  server.on("/api/contacts/authorized", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    ContactList cl = {};
    if (!Shared_getAuthorizedContacts(cl)) {
      request->send(500, "application/json", "{\"error\":\"Read failed\"}");
      return;
    }
    String body = "{\"authorized\": [";
    for (size_t i = 0; i < cl.count; ++i) {
      if (i) body += ",";
      body += "{";
      body += "\"enabled\":" + String(cl.items[i].enabled ? "true" : "false") + ",";
      body += "\"name\":\"" + escapeJson(String(cl.items[i].name)) + "\",";
      body += "\"number\":\"" + escapeJson(String(cl.items[i].number)) + "\"";
      body += "}";
    }
    body += "]}";
    request->send(200, "application/json", body);
  });
  server.on("/api/contacts/authorized", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    // Expect body as urlencoded param 'contacts' containing a JSON array.
    String body = "";
    if (request->hasParam("contacts", true)) body = request->getParam("contacts", true)->value();
    if (body.length() == 0) { request->send(400, "application/json", "{\"error\":\"Missing contacts payload\"}"); return; }

    // Parse all objects to count them. Enforce max and validate phone format server-side.
    ContactList cl = {};
    int pos = 0;
    int totalFound = 0;
    while (true) {
      int objStart = body.indexOf('{', pos);
      if (objStart < 0) break;
      int objEnd = body.indexOf('}', objStart);
      if (objEnd < 0) break;
      totalFound++;
      if (totalFound > (int)MAX_PHONE_PER_LIST) {
        String err = String("{\"error\":\"Too many contacts (max ") + String(MAX_PHONE_PER_LIST) + " )\"}";
        request->send(400, "application/json", err);
        return;
      }

      String obj = body.substring(objStart + 1, objEnd);
      Contact c = {};
      int enIdx = obj.indexOf("\"enabled\"");
      if (enIdx >= 0) {
        int colon = obj.indexOf(':', enIdx);
        if (colon >= 0) {
          String val = trimCopy(obj.substring(colon + 1));
          if (val.startsWith("true")) c.enabled = true;
        }
      }
      int nameIdx = obj.indexOf("\"name\"");
      if (nameIdx >= 0) {
        int colon = obj.indexOf(':', nameIdx);
        int q1 = obj.indexOf('"', colon + 1);
        int q2 = obj.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 >= 0) {
          String n = obj.substring(q1 + 1, q2);
          n.trim(); n.toCharArray(c.name, sizeof(c.name));
        }
      }
      int numIdx = obj.indexOf("\"number\"");
      if (numIdx < 0) {
        String err = String("{\"error\":\"Missing phone number field at contact ") + String(totalFound) + "\"}";
        request->send(400, "application/json", err);
        return;
      }
      int colon = obj.indexOf(':', numIdx);
      int q1 = obj.indexOf('"', colon + 1);
      int q2 = obj.indexOf('"', q1 + 1);
      if (q1 < 0 || q2 < 0) {
        String err = String("{\"error\":\"Missing phone number value at contact ") + String(totalFound) + "\"}";
        request->send(400, "application/json", err);
        return;
      }
      {
        String n = obj.substring(q1 + 1, q2);
        n.trim();
        if (n.length() == 0) {
          String err = String("{\"error\":\"Empty phone number at contact ") + String(totalFound) + "\"}";
          request->send(400, "application/json", err);
          return;
        }
        if (!isValidPhoneFormat(n)) {
          String err = String("{\"error\":\"Invalid phone format at contact ") + String(totalFound) + "\"}";
          request->send(400, "application/json", err);
          return;
        }
        n.toCharArray(c.number, PHONE_NUMBER_LENGTH);
      }

      if (cl.count < MAX_PHONE_PER_LIST) cl.items[cl.count++] = c;
      pos = objEnd + 1;
    }

    if (!Shared_saveAuthorizedContacts(cl)) { request->send(500, "application/json", "{\"error\":\"Save failed\"}"); return; }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/contacts/recipients", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    ContactList cl = {};
    if (!Shared_getRecipientContacts(cl)) { request->send(500, "application/json", "{\"error\":\"Read failed\"}"); return; }
    String body = "{\"recipients\": [";
    for (size_t i = 0; i < cl.count; ++i) {
      if (i) body += ",";
      body += "{";
      body += "\"enabled\":" + String(cl.items[i].enabled ? "true" : "false") + ",";
      body += "\"name\":\"" + escapeJson(String(cl.items[i].name)) + "\",";
      body += "\"number\":\"" + escapeJson(String(cl.items[i].number)) + "\"";
      body += "}";
    }
    body += "]}";
    request->send(200, "application/json", body);
  });

  server.on("/api/contacts/recipients", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    String body = request->hasParam("contacts", true) ? request->getParam("contacts", true)->value() : "";
    if (body.length() == 0) { request->send(400, "application/json", "{\"error\":\"Missing contacts payload\"}"); return; }

    ContactList cl = {};
    int pos = 0;
    int totalFound = 0;
    while (true) {
      int objStart = body.indexOf('{', pos);
      if (objStart < 0) break;
      int objEnd = body.indexOf('}', objStart);
      if (objEnd < 0) break;
      totalFound++;
      if (totalFound > (int)MAX_PHONE_PER_LIST) {
        String err = String("{\"error\":\"Too many contacts (max ") + String(MAX_PHONE_PER_LIST) + " )\"}";
        request->send(400, "application/json", err);
        return;
      }
      String obj = body.substring(objStart + 1, objEnd);
      Contact c = {};
      int enIdx = obj.indexOf("\"enabled\"");
      if (enIdx >= 0) {
        int colon = obj.indexOf(':', enIdx);
        if (colon >= 0) {
          String val = trimCopy(obj.substring(colon + 1));
          if (val.startsWith("true")) c.enabled = true;
        }
      }
      int nameIdx = obj.indexOf("\"name\"");
      if (nameIdx >= 0) {
        int colon = obj.indexOf(':', nameIdx);
        int q1 = obj.indexOf('"', colon + 1);
        int q2 = obj.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 >= 0) { String n = obj.substring(q1 + 1, q2); n.trim(); n.toCharArray(c.name, sizeof(c.name)); }
      }
      int numIdx = obj.indexOf("\"number\"");
      if (numIdx < 0) {
        String err = String("{\"error\":\"Missing phone number field at contact ") + String(totalFound) + "\"}";
        request->send(400, "application/json", err);
        return;
      }
      int colon = obj.indexOf(':', numIdx);
      int q1 = obj.indexOf('"', colon + 1);
      int q2 = obj.indexOf('"', q1 + 1);
      if (q1 < 0 || q2 < 0) {
        String err = String("{\"error\":\"Missing phone number value at contact ") + String(totalFound) + "\"}";
        request->send(400, "application/json", err);
        return;
      }
      {
        String n = obj.substring(q1 + 1, q2);
        n.trim();
        if (n.length() == 0) { String err = String("{\"error\":\"Empty phone number at contact ") + String(totalFound) + "\"}"; request->send(400, "application/json", err); return; }
        if (!isValidPhoneFormat(n)) { String err = String("{\"error\":\"Invalid phone format at contact ") + String(totalFound) + "\"}"; request->send(400, "application/json", err); return; }
        n.toCharArray(c.number, PHONE_NUMBER_LENGTH);
      }
      if (cl.count < MAX_PHONE_PER_LIST) cl.items[cl.count++] = c;
      pos = objEnd + 1;
    }
    if (!Shared_saveRecipientContacts(cl)) { request->send(500, "application/json", "{\"error\":\"Save failed\"}"); return; }
    request->send(200, "application/json", "{\"success\":true}");
  });

  // Network settings endpoints (used by Network tab)
  server.on("/api/gateway-settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    GatewaySettings s = {};
    if (!Shared_getGatewaySettings(s)) {
      request->send(500, "application/json", "{\"error\":\"Read failed\"}");
      return;
    }
    String body = "{";
    body += "\"use_dhcp\":" + String(s.useDhcp ? "true" : "false") + ",";
    body += "\"static_ip\":\"" + ipBytesToString(s.staticIp) + "\",";
    body += "\"subnet_mask\":\"" + ipBytesToString(s.subnetMask) + "\",";
    body += "\"gateway_ip\":\"" + ipBytesToString(s.gatewayIp) + "\",";
    body += "\"http_port\":" + String(s.httpPort);
    body += "}";
    request->send(200, "application/json", body);
  });

  server.on("/api/gateway-settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    GatewaySettings s = {};
    s.useDhcp = request->hasParam("use_dhcp", true) ? (request->getParam("use_dhcp", true)->value() == "1") : false;
    String sip = request->hasParam("static_ip", true) ? request->getParam("static_ip", true)->value() : "";
    String sm = request->hasParam("subnet_mask", true) ? request->getParam("subnet_mask", true)->value() : "";
    String gw = request->hasParam("gateway_ip", true) ? request->getParam("gateway_ip", true)->value() : "";
    String port = request->hasParam("http_port", true) ? request->getParam("http_port", true)->value() : "80";

    if (sip.length() > 0) parseIPFromText(sip, s.staticIp);
    if (sm.length() > 0) parseIPFromText(sm, s.subnetMask);
    if (gw.length() > 0) parseIPFromText(gw, s.gatewayIp);
    s.httpPort = (uint16_t)port.toInt();

    if (!Shared_saveGatewaySettings(s)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    request->send(200, "text/html", htmlPage());
  });

  // Message CSV endpoints removed for RAMS; configuration is handled via Web UI

  serverRoutesSetup = true;
}

static void startAPMode() {
  if (Shared_isAPModeActive()) return;

  Serial.println("[AP] Starting Access Point...");
  WiFi.mode(WIFI_AP_STA);
  delay(100);  // Increased delay to let WiFi/Ethernet stack stabilize after mode change
  IPAddress apIP(10, 10, 10, 10);
  IPAddress gateway(10, 10, 10, 10);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  String ssid = getAPSSID();
  WiFi.softAP(ssid.c_str(), AP_PASS_FIXED);
  delay(200);

  Serial.print("[AP] SSID: ");
  Serial.println(ssid);
  Serial.print("[AP] Password: ");
  Serial.println(AP_PASS_FIXED);
  Serial.print("[AP] AP IP address: ");
  Serial.println(WiFi.softAPIP());

  Shared_setAPModeActive(true);
  digitalWrite(AP_STATUS_LED_PIN, HIGH);
  Serial.println("[AP] Access Point is now active");
}

static void stopAPMode() {
  if (!Shared_isAPModeActive()) return;

  Serial.println("[AP] Stopping Access Point...");
  WiFi.softAPdisconnect(true);
  // Keep STA mode alive so Web UI on Ethernet remains available.
  WiFi.mode(WIFI_STA);
  delay(200);  // Increased delay to let WiFi/Ethernet stack stabilize after mode change

  Shared_setAPModeActive(false);
  digitalWrite(AP_STATUS_LED_PIN, LOW);
  Serial.println("[AP] Access Point is now disabled");
}

void AP_taskLoop(void *pvParameters) {
  (void)pvParameters;
  static unsigned long lastStateChange = 0;

  // Bring up base Wi-Fi stack without connecting, required by Async web stack.
  WiFi.mode(WIFI_STA);
  delay(50);

  // Reset old serial state when firmware build changes.
  clearStaleSerialForNewBuild();

  // Keep Web UI always active (Ethernet IP + AP IP when AP mode is enabled).
  setupWebServerRoutes();
  if (!serverStarted) {
    server.begin();
    serverStarted = true;
    Serial.println("[WEB] Config server started on port 80 (always active)");
  }

  for (;;) {
    bool switchState  = digitalRead(BUTTON_PIN);
    unsigned long now = millis();

    if (now - lastStateChange > BUTTON_DEBOUNCE_MS) {
      if (switchState == LOW && !Shared_isAPModeActive()) {
        startAPMode();
        lastStateChange = now;
      } else if (switchState == HIGH && Shared_isAPModeActive()) {
        stopAPMode();
        lastStateChange = now;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static String htmlPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>RAMS Dashboard</title>
<style>
  :root{--bg:#f3f6f9;--muted:#6b7280;--primary:#1565c0;--card:#eef7ff}
  *{box-sizing:border-box}
  body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:#0f172a}
  .layout{display:flex;min-height:100vh}
  .sidebar{width:240px;background:#fff;border-right:1px solid #e6eef7;padding:20px}
  .brand{font-weight:700;color:var(--primary);font-size:18px;margin-bottom:12px}
  .nav{list-style:none;padding:0;margin:0}
  .nav li{padding:10px 12px;border-radius:8px;color:#475569;cursor:pointer;margin-bottom:6px}
  .nav li.active{background:#e8f6ff;color:var(--primary);font-weight:600}
  .content{flex:1;padding:20px}
  .topbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px}
  .actions{display:flex;gap:8px}
  .btn{padding:8px 14px;border-radius:8px;border:0;cursor:pointer;font-weight:700}
  .btn.primary{background:var(--primary);color:#fff}
  .btn.ghost{background:transparent;color:var(--primary);border:1px solid rgba(21,101,192,0.08)}
  .btn.danger{background:#d32f2f;color:#fff}
  .panel{background:#fff;border-radius:12px;padding:16px;box-shadow:0 8px 24px rgba(2,6,23,0.06)}
  h2{margin:0;font-size:18px}
  .subtitle{color:var(--muted);font-size:13px}
  .grid{display:grid;grid-template-columns:repeat(2,1fr);gap:12px;margin-top:14px}
  .stat{background:var(--card);border-radius:10px;padding:14px}
  .stat .label{color:var(--muted);font-size:13px}
  .stat .value{font-weight:700;margin-top:6px;font-size:15px}
  @media(max-width:900px){.grid{grid-template-columns:1fr}.sidebar{display:none}}

  /* System Config form layout */
  .form-section{margin-top:14px;padding-top:8px;border-top:0}
  .form-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;align-items:start}
  .field{display:flex;flex-direction:column}
  .field.full{grid-column:1/-1}
  .field label{font-size:13px;color:#444;margin-bottom:6px}
  .input{width:100%;padding:10px 12px;border:1px solid #dfe6ee;border-radius:8px;font-size:14px}
  .form-actions{margin-top:10px}
  @media(max-width:640px){.form-grid{grid-template-columns:1fr}}
</style>
</head>
<body>
  <div class="layout">
    <div class="sidebar">
      <div class="brand">RAMS</div>
      <ul class="nav" id="nav">
        <li data-tab="dashboard" class="active">Dashboard</li>
        <li data-tab="digital">Digital Inputs</li>
        <li data-tab="analog">Analog Inputs</li>
        <li data-tab="relays">Relay Outputs</li>
        <!-- Alarm Management removed -->
        <li data-tab="phones">Contact Config</li>
        <li data-tab="network">Network Configuration</li>
        <li data-tab="sysconfig">System Config</li>
        <li data-tab="diag">Diagnostics</li>
      </ul>
    </div>
    <div class="content">
      <div class="topbar">
        <div>
          <h2>Dashboard</h2>
          <div class="subtitle">Current Configuration Status</div>
        </div>
        <div class="actions">
          <button class="btn danger" onclick="location.href='/logout'">Logout</button>
        </div>
      </div>

      <div id="dashboard" class="tab active">
        <div class="panel">
          <div class="grid" id="dashGrid">
            <div class="stat"><div class="label">Serial Number</div><div class="value" id="s_serial">Loading...</div></div>
            <div class="stat"><div class="label">Login User Name</div><div class="value" id="s_login">Loading...</div></div>
            <div class="stat"><div class="label">Wi‑Fi AP IP</div><div class="value" id="s_apip">Loading...</div></div>
            <div class="stat"><div class="label">Ethernet IP</div><div class="value" id="s_ethip">Loading...</div></div>
            <div class="stat"><div class="label">DHCP Mode</div><div class="value" id="s_dhcp">Loading...</div></div>
            <div class="stat"><div class="label">Static IP</div><div class="value" id="s_static">Loading...</div></div>
            <div class="stat"><div class="label">Subnet Mask</div><div class="value" id="s_subnet">Loading...</div></div>
            <div class="stat"><div class="label">Gateway IP</div><div class="value" id="s_gateway">Loading...</div></div>
            <div class="stat"><div class="label">HTTP Port</div><div class="value" id="s_httpport">-</div></div>
            <div class="stat"><div class="label">Firmware</div><div class="value" id="s_fw">-</div></div>
          </div>
        </div>
      </div>

      <div id="digital" class="tab" style="display:none"><div class="panel"><h2>Digital Inputs</h2><div class="subtitle">Configure and view 4 digital inputs here (placeholder).</div></div></div>
      <div id="analog" class="tab" style="display:none"><div class="panel"><h2>Analog Inputs</h2><div class="subtitle">Configure and view 2 analog inputs here (placeholder).</div></div></div>
      <div id="relays" class="tab" style="display:none"><div class="panel"><h2>Relay Outputs</h2><div class="subtitle">Control 2 relay outputs (placeholder).</div></div></div>
      <!-- Alarm Management tab removed -->
      <div id="phones" class="tab" style="display:none">
        <div class="panel">
          <h2>Contact Configuration</h2>
          <div id="phones_status" style="display:none;margin-bottom:12px"></div>
          <div class="subtitle">Manage Authorized contacts and Event Recipients. Phone numbers accept digits and optional leading '+', up to 15 digits.</div>

          <h3>Authorized Contacts</h3>
          <div id="auth_list"></div>
          <button class="btn" onclick="addAuthContact()">Add Authorized</button>

          <h3 style="margin-top:16px">Event Recipients</h3>
          <div id="rec_list"></div>
          <button class="btn" onclick="addRecContact()">Add Recipient</button>

          <div style="margin-top:10px">
            <button class="btn primary" onclick="saveContacts()">Save Contacts</button>
          </div>
        </div>
      </div>

      <div id="network" class="tab" style="display:none">
        <div class="panel">
          <h2>Network Configuration</h2>
          <div id="net_status" style="display:none;margin-bottom:12px"></div>

          <div class="form-section">
            <h3>Connection Mode</h3>
            <div class="field full">
              <div class="row">
                <input id="net_useDhcp" type="checkbox" style="width:auto" onchange="toggleNetworkStaticFields()">
                <label for="net_useDhcp" style="margin:0 0 0 8px">Use DHCP for Ethernet</label>
              </div>
            </div>
          </div>

          <div class="form-section">
            <h3>Static Network Settings</h3>
            <div class="form-grid">
              <div class="field">
                <label>Static IP</label>
                <input id="net_staticIp" class="input" placeholder="192.168.8.200" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)">
              </div>
              <div class="field">
                <label>Subnet Mask</label>
                <input id="net_subnetMask" class="input" placeholder="255.255.255.0" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)">
              </div>
              <div class="field">
                <label>Gateway IP</label>
                <input id="net_gatewayIp" class="input" placeholder="192.168.8.1" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)">
              </div>
              <div class="field">
                <label>HTTP Port</label>
                <input id="net_tcpPort" class="input" type="number" min="1" max="65535" inputmode="numeric" oninput="sanitizeNumberInput(this)">
              </div>
            </div>
            <div class="form-actions">
              <button class="btn primary" onclick="saveNetworkCfg()">Save Network Settings</button>
              <a href="/" class="muted" style="margin-left:12px">Back to Dashboard</a>
            </div>
            <p class="muted" style="margin-top:8px">Reboot device after saving to apply network changes.</p>
          </div>
        </div>
      </div>
      <div id="sysconfig" class="tab" style="display:none">
        <div class="panel">
          <h2>System Config</h2>
          <div id="sys_status" style="display:none;margin-bottom:12px"></div>

          <div class="form-section">
            <h3>Site Details</h3>
            <div class="form-grid">
              <div class="field">
                <label>Site Name</label>
                <input id="site_name" type="text" class="input">
              </div>
              <div class="field full">
                <label>Site Address</label>
                <textarea id="site_address" rows="3" class="input"></textarea>
              </div>
            </div>
            <div class="form-actions">
              <button class="btn primary" onclick="saveSystemConfig()">Save Site Details</button>
            </div>
          </div>

          <div class="form-section">
            <h3>Real Time Clock (RTC)</h3>
            <div class="field">
              <label>Time Zone</label>
              <select id="timezone" class="input">
                <option value="UTC0">UTC</option>
                <option value="GMT0BST,M3.5.0/01:00:00,M10.5.0/02:00:00">Europe/London</option>
                <option value="CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:00">Europe/Berlin</option>
                <option value="IST-5:30">Asia/Kolkata</option>
                <option value="EST5EDT,M3.2.0/02:00:00,M11.1.0/02:00:00">America/New_York</option>
                <option value="AEST-10AEDT,M10.1.0/02:00:00,M4.1.0/03:00:00">Australia/Sydney</option>
              </select>
            </div>
            <div class="form-actions">
              <button class="btn" onclick="restartNtp()">Restart NTP Service</button>
            </div>
          </div>
        </div>
      </div>
      <div id="diag" class="tab" style="display:none"><div class="panel"><h2>Diagnostics</h2><div class="subtitle">System diagnostics and status (placeholder).</div></div></div>
    </div>
  </div>

<script>
document.getElementById('nav').addEventListener('click', function(e){
  var li = e.target.closest('li'); if(!li) return;
  var tab = li.getAttribute('data-tab');
  document.querySelectorAll('.nav li').forEach(function(n){ n.classList.remove('active'); });
  li.classList.add('active');
  document.querySelectorAll('.tab').forEach(function(t){ t.style.display='none'; });
  var el = document.getElementById(tab); if(el) el.style.display='block';
  if (tab === 'sysconfig') loadSystemConfig();
  if (tab === 'phones') loadPhones();
  if (tab === 'network') loadNetworkCfg();
  
});

function setStat(id, value){ var el=document.getElementById(id); if(el) el.textContent = value; }

function loadDashboard(){
  fetch('/api/dashboard').then(r=>{
    if(r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(d=>{
    setStat('s_serial', d.serial_number || 'Not Set');
    setStat('s_login', d.login_user || 'Admin');
    setStat('s_apip', d.ap_ip || '-');
    setStat('s_ethip', d.eth_ip || '-');
    setStat('s_dhcp', d.use_dhcp ? 'Enabled' : 'Disabled');
    setStat('s_static', d.static_ip || '-');
    setStat('s_subnet', d.subnet_mask || '-');
    setStat('s_gateway', d.gateway_ip || '-');
    setStat('s_httpport', d.http_port ? String(d.http_port) : '-');
    setStat('s_fw', d.fw_build || '-');
    // Clear network loading placeholder
    var nc = document.getElementById('netcfg'); if(nc) nc.textContent = '';
  }).catch(e=>{ if(e !== 'auth') console.log('dashboard load failed', e); });
}
function showStatus(msg, ok) {
  var el = document.getElementById('sys_status'); if (!el) return;
  el.textContent = msg; el.style.display = 'block'; el.style.color = ok ? 'green' : 'red';
  setTimeout(function() { el.style.display = 'none'; }, 4000);
}

function loadSystemConfig(){
  fetch('/api/system-config').then(r=>{
    if (r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(cfg=>{
    var sn = document.getElementById('site_name'); if (sn) sn.value = cfg.site_name || '';
    var sa = document.getElementById('site_address'); if (sa) sa.value = cfg.site_address || '';
    var tz = cfg.timezone || 'UTC0';
    var tzSel = document.getElementById('timezone');
    if (tzSel) {
      var found = false;
      for (var i=0;i<tzSel.options.length;i++) { if (tzSel.options[i].value === tz) { tzSel.selectedIndex = i; found = true; break; } }
      if (!found) { var o = document.createElement('option'); o.value = tz; o.text = tz; tzSel.add(o); tzSel.value = tz; }
    }
    
  }).catch(e=>{ if (e !== 'auth') console.log('load sysconfig failed', e); });
}

function saveSystemConfig(){
  var p = new URLSearchParams();
  p.append('site_name', document.getElementById('site_name').value.trim());
  p.append('site_address', document.getElementById('site_address').value.trim());
  p.append('timezone', document.getElementById('timezone').value);
  fetch('/api/system-config', { method:'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{ if (d.success) showStatus('Saved', true); else showStatus(d.error||'Save failed', false); })
    .catch(e=>showStatus('Save failed: '+e.message, false));
}

function restartNtp(){
  var p = new URLSearchParams();
  p.append('timezone', document.getElementById('timezone').value);
  fetch('/api/restart-ntp', { method:'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{ if (d.success) showStatus('NTP restarted', true); else showStatus(d.error||'Restart failed', false); })
    .catch(e=>showStatus('Restart failed: '+e.message, false));
}

loadDashboard();
</script>
<script>
function showSmallStatus(elId, msg, ok) { var el=document.getElementById(elId); if(!el) return; el.textContent=msg; el.style.display='block'; el.style.color = ok ? 'green' : 'red'; setTimeout(function(){ el.style.display='none'; }, 3500); }
const MAX_CONTACTS = 5;

function loadPhones(){
  // Load both authorized and recipient contacts
  fetch('/api/contacts/authorized').then(r=>{ if(r.status===401){window.location='/login';return Promise.reject('auth')} return r.json(); }).then(d=>{
    if (d.authorized && Array.isArray(d.authorized)) renderContactList('auth_list', d.authorized, true);
  }).catch(e=>{ if(e!=='auth') console.log('auth load failed', e); });
  fetch('/api/contacts/recipients').then(r=>{ if(r.status===401){window.location='/login';return Promise.reject('auth')} return r.json(); }).then(d=>{
    if (d.recipients && Array.isArray(d.recipients)) renderContactList('rec_list', d.recipients, false);
  }).catch(e=>{ if(e!=='auth') console.log('rec load failed', e); });
}

function saveContacts(){
  // collect authorized
  var authEls = document.querySelectorAll('#auth_list .contact-row');
  var authArr = [];
  authEls.forEach(function(el){
    var name = (el.querySelector('.c_name')||{}).value || '';
    var num = (el.querySelector('.c_number')||{}).value || '';
    var en = !!(el.querySelector('.c_enabled')||{}).checked;
    authArr.push({enabled:en, name:name, number:num});
  });
  var recEls = document.querySelectorAll('#rec_list .contact-row');
  var recArr = [];
  recEls.forEach(function(el){
    var name = (el.querySelector('.c_name')||{}).value || '';
    var num = (el.querySelector('.c_number')||{}).value || '';
    var en = !!(el.querySelector('.c_enabled')||{}).checked;
    recArr.push({enabled:en, name:name, number:num});
  });

  // simple validation for phone numbers: digits and optional leading '+' and 10-15 digits
  var phoneValid = function(n){ if(!n) return true; var m = n.match(/^\+?[0-9]{10,15}$/); return !!m; };
  for (var i=0;i<authArr.length;i++) if(!phoneValid(authArr[i].number)){ showSmallStatus('phones_status','Invalid phone in authorized list',false); return; }
  for (var i=0;i<recArr.length;i++) if(!phoneValid(recArr[i].number)){ showSmallStatus('phones_status','Invalid phone in recipients list',false); return; }

  // Enforce max contacts per list
  if (authArr.length > MAX_CONTACTS) { showSmallStatus('phones_status','Authorized list limited to ' + MAX_CONTACTS + ' entries', false); return; }
  if (recArr.length > MAX_CONTACTS) { showSmallStatus('phones_status','Recipients list limited to ' + MAX_CONTACTS + ' entries', false); return; }

  var p1 = new URLSearchParams(); p1.append('contacts', JSON.stringify(authArr));
  fetch('/api/contacts/authorized', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: p1.toString() })
    .then(r=>r.json()).then(d=>{ if(d.success) showSmallStatus('phones_status','Authorized saved',true); else showSmallStatus('phones_status',d.error||'Save failed',false); })
    .catch(e=>showSmallStatus('phones_status','Save failed',false));

  var p2 = new URLSearchParams(); p2.append('contacts', JSON.stringify(recArr));
  fetch('/api/contacts/recipients', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: p2.toString() })
    .then(r=>r.json()).then(d=>{ if(d.success) showSmallStatus('phones_status','Recipients saved',true); else showSmallStatus('phones_status',d.error||'Save failed',false); })
    .catch(e=>showSmallStatus('phones_status','Save failed',false));
}

function renderContactList(containerId, arr, isAuth){
  var el = document.getElementById(containerId); if(!el) return;
  el.innerHTML = '';
  for (var idx = 0; idx < arr.length && idx < MAX_CONTACTS; idx++) {
    var item = arr[idx];
    var row = document.createElement('div'); row.className = 'contact-row';
    row.style.display='flex'; row.style.gap='8px'; row.style.margin='6px 0';
    var chk = document.createElement('input'); chk.type='checkbox'; chk.className='c_enabled'; chk.checked=!!item.enabled;
    var name = document.createElement('input'); name.className='c_name input'; name.placeholder='Name'; name.value=item.name||'';
    var num = document.createElement('input'); num.className='c_number input'; num.placeholder="+1234567890"; num.value=item.number||'';
    num.oninput = function(){ this.value = this.value.replace(/[^0-9+]/g,''); if(this.value.indexOf('+')>0) this.value = this.value.replace(/\+/g,''); if(this.value.length>16) this.value = this.value.slice(0,16); };
    var del = document.createElement('button'); del.className='btn'; del.textContent='Remove'; del.onclick = function(){ row.remove(); };
    row.appendChild(chk); row.appendChild(name); row.appendChild(num); row.appendChild(del);
    el.appendChild(row);
  }
}

function addAuthContact(){ var el=document.getElementById('auth_list'); if(!el) return; var cnt = el.querySelectorAll('.contact-row').length; if(cnt >= MAX_CONTACTS){ showSmallStatus('phones_status','Max ' + MAX_CONTACTS + ' contacts allowed', false); return; } renderContactListAppend(el); }
function addRecContact(){ var el=document.getElementById('rec_list'); if(!el) return; var cnt = el.querySelectorAll('.contact-row').length; if(cnt >= MAX_CONTACTS){ showSmallStatus('phones_status','Max ' + MAX_CONTACTS + ' contacts allowed', false); return; } renderContactListAppend(el); }
function renderContactListAppend(el){ var item={enabled:true,name:'',number:''}; var row = document.createElement('div'); row.className = 'contact-row'; row.style.display='flex'; row.style.gap='8px'; row.style.margin='6px 0'; var chk=document.createElement('input'); chk.type='checkbox'; chk.className='c_enabled'; chk.checked=true; var name=document.createElement('input'); name.className='c_name input'; name.placeholder='Name'; var num=document.createElement('input'); num.className='c_number input'; num.placeholder='+1234567890'; num.oninput=function(){ this.value=this.value.replace(/[^0-9+]/g,''); if(this.value.indexOf('+')>0) this.value=this.value.replace(/\+/g,''); if(this.value.length>16) this.value=this.value.slice(0,16); }; var del=document.createElement('button'); del.className='btn'; del.textContent='Remove'; del.onclick=function(){ row.remove(); }; row.appendChild(chk); row.appendChild(name); row.appendChild(num); row.appendChild(del); el.appendChild(row); }

function loadNetworkCfg(){
  fetch('/api/gateway-settings').then(r=>{ if(r.status===401){window.location='/login';return Promise.reject('auth')} return r.json(); }).then(d=>{
    var dh = !!d.use_dhcp;
    var useEl = document.getElementById('net_useDhcp'); if (useEl) useEl.checked = dh;
    var si = document.getElementById('net_staticIp'); if (si) si.value = d.static_ip || '';
    var sm = document.getElementById('net_subnetMask'); if (sm) sm.value = d.subnet_mask || '';
    var gw = document.getElementById('net_gatewayIp'); if (gw) gw.value = d.gateway_ip || '';
    var tp = document.getElementById('net_tcpPort'); if (tp) tp.value = d.http_port || '';
    // ensure static fields are enabled/disabled according to DHCP
    try { toggleNetworkStaticFields(); } catch(e){}
  }).catch(e=>{ if(e!=='auth') console.log('network load failed', e); });
}

function saveNetworkCfg(){
  var useDhcp = document.getElementById('net_useDhcp').checked;
  var staticIp = (document.getElementById('net_staticIp')||{}).value || '';
  var subnet = (document.getElementById('net_subnetMask')||{}).value || '';
  var gateway = (document.getElementById('net_gatewayIp')||{}).value || '';
  var port = (document.getElementById('net_tcpPort')||{}).value || '';

  if (!useDhcp) {
    if (!isValidIPv4(staticIp) || !isValidIPv4(subnet) || !isValidIPv4(gateway)) {
      showSmallStatus('net_status','Invalid IP address format', false);
      return;
    }
  }
  if (port && (isNaN(parseInt(port,10)) || parseInt(port,10) < 1)) { showSmallStatus('net_status','Invalid port', false); return; }

  var p = new URLSearchParams();
  p.append('use_dhcp', useDhcp ? '1' : '0');
  p.append('static_ip', staticIp.trim());
  p.append('subnet_mask', subnet.trim());
  p.append('gateway_ip', gateway.trim());
  p.append('http_port', port.trim());

  fetch('/api/gateway-settings', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{ if(d.success) showSmallStatus('net_status','Saved. Reboot to apply',true); else showSmallStatus('net_status',d.error||'Save failed',false); })
    .catch(e=>showSmallStatus('net_status','Save failed',false));
}

function isValidIPv4(ip) {
  if (!ip) return false;
  var parts = ip.split('.'); if (parts.length !== 4) return false;
  for (var i=0;i<4;i++){ var n = parseInt(parts[i],10); if (isNaN(n) || n<0 || n>255) return false; }
  return true;
}

function toggleNetworkStaticFields(){
  var dhcp = document.getElementById('net_useDhcp') && document.getElementById('net_useDhcp').checked;
  ['net_staticIp','net_subnetMask','net_gatewayIp'].forEach(function(id){ var el = document.getElementById(id); if(el) el.disabled = dhcp; });
}

function sanitizeIpInput(el){ if(!el) return; var cleaned = el.value.replace(/[^0-9.]/g,''); var parts = cleaned.split('.'); if(parts.length>4) parts = parts.slice(0,4); for(var i=0;i<parts.length;i++){ if(parts[i].length>3) parts[i] = parts[i].slice(0,3); } el.value = parts.join('.'); }
function sanitizeNumberInput(el){ if(!el) return; el.value = el.value.replace(/[^0-9]/g, ''); if(el.value==='') return; var v=parseInt(el.value,10); if(!Number.isFinite(v)) return; if(v<1) v=1; if(v>65535) v=65535; el.value=String(v); }
</script>
</body>
</html>

)rawliteral";
}
