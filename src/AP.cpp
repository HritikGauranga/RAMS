#include "AP.h"
#include "Shared.h"
#include "Modem.h"
#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <new>

static AsyncWebServer *server            = nullptr;
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

static const char *htmlPage();
static void sendHtmlPage(AsyncWebServerRequest *request);
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
    <h1>RAMS Config Login</h1>
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
  // Fresh successful login redirects to /?tab=dashboard; refresh tab state is kept in localStorage.
  sessionStorage.clear();
  
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

  // Open/create serial file - LittleFS files are in root
  File out = LittleFS.open(SERIAL_FILE_PATH, "w");
  if (!out) {
    Shared_unlockFileSystem();
    error = "Failed to open serial file for writing";
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
function loadCfg(){
  fetch('/api/gateway-settings').then(function(r){ return r.json(); }).then(function(c){
    document.getElementById('useDhcp').checked=!!c.use_dhcp;
    document.getElementById('staticIp').value=c.static_ip||'';
    document.getElementById('subnetMask').value=c.subnet_mask||'';
    document.getElementById('gatewayIp').value=c.gateway_ip||'';
  }).catch(function(e){ status('Load failed: '+e.message,false); });
}
function saveCfg(){
  var p=new URLSearchParams();
  p.append('use_dhcp',document.getElementById('useDhcp').checked?'1':'0');
  p.append('static_ip',document.getElementById('staticIp').value.trim());
  p.append('subnet_mask',document.getElementById('subnetMask').value.trim());
  p.append('gateway_ip',document.getElementById('gatewayIp').value.trim());
  fetch('/api/gateway-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p.toString()})
    .then(function(r){ return r.json(); }).then(function(d){ if(d.success) status('Saved. Reboot to apply.',true); else status(d.error||'Save failed',false); })
    .catch(function(e){ status('Save failed: '+e.message,false); });
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
  if (server == nullptr) return;
  if (serverRoutesSetup) return;

  server->on("/Gaurangalogo.png", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/Gaurangalogo.png", "image/png");
  });

  server->on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (isAuthenticated(request)) {
      sendRedirect(request, "/");
      return;
    }
    bool bad = request->hasParam("err");
    request->send(200, "text/html", loginPage(getLoginUsername(), bad));
  });

  server->on("/login", HTTP_POST, [](AsyncWebServerRequest *request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";
    String expectedUser = getLoginUsername();
    String expectedPass = getLoginPassword();

    if (user == expectedUser && pass == expectedPass) {
      gAuthSessionToken = makeSessionToken();
      AsyncWebServerResponse *res = request->beginResponse(302);
      res->addHeader("Location", "/?tab=dashboard");
      res->addHeader("Set-Cookie",
                     String(AUTH_COOKIE_NAME) + "=" + gAuthSessionToken +
                     "; Path=/; Max-Age=86400; HttpOnly; SameSite=Strict");
      request->send(res);
      return;
    }

    sendRedirect(request, "/login?err=1");
  });

  server->on("/logout", HTTP_GET, [](AsyncWebServerRequest *request) {
    clearAuthCookie(request);
  });

  server->on("/serialnumber", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    String serial = readSerialNumber();
    request->send(200, "text/html", serialNumberPage(serial, "", true));
  });

  server->on("/serialnumber/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    String serial = readSerialNumber();
    request->send(200, "text/html", serialNumberPage(serial, "", true));
  });

  server->on("/serialnumber/", HTTP_POST, [](AsyncWebServerRequest *request) {
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

  server->on("/api/dashboard", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    GatewaySettings s = {};
    if (!Shared_getGatewaySettings(s)) {
      request->send(500, "application/json", "{\"error\":\"Read failed\"}");
      return;
    }

    SIMConfig simCfg = {};
    Shared_getSIMConfig(simCfg);

    int8_t rssi = Modem_getSignalStrength();
    String strength = "Unknown";
    if (rssi == -2) {
      strength = "SIM Not Inserted";
    } else if (rssi >= 0) {
      if (rssi >= 25) strength = "Excellent";
      else if (rssi >= 20) strength = "Very Good";
      else if (rssi >= 15) strength = "Good";
      else if (rssi >= 10) strength = "Fair";
      else if (rssi >= 5) strength = "Weak";
      else strength = "Very Weak";
    }

    String serial = readSerialNumber();
    if (serial.length() == 0) serial = "Not Set";
    String apIp = WiFi.softAPIP().toString();
    if (apIp == "0.0.0.0") apIp = "AP Mode Off (10.10.10.10 when enabled)";
    String ethIp = ETH.localIP().toString();
    if (ethIp == "0.0.0.0") {
      ethIp = ipBytesToString(s.staticIp);
    }

    // Merge SIM info for display: "Provider - Phone Number"
    String simDisplay = String(simCfg.service_provider);
    if (String(simCfg.phone_number).length() > 0) {
      if (simDisplay.length() > 0) simDisplay += " - ";
      simDisplay += String(simCfg.phone_number);
    }
    if (simDisplay.length() == 0) simDisplay = "Not Set";

    String body = "{";
    body += "\"serial_number\":\"" + serial + "\",";
    body += "\"login_user\":\"" + getLoginUsername() + "\",";
    body += "\"ap_ip\":\"" + apIp + "\",";
    body += "\"eth_ip\":\"" + ethIp + "\",";
    body += "\"use_dhcp\":" + String(s.useDhcp ? "true" : "false") + ",";
    body += "\"static_ip\":\"" + ipBytesToString(s.staticIp) + "\",";
    body += "\"subnet_mask\":\"" + ipBytesToString(s.subnetMask) + "\",";
    body += "\"gateway_ip\":\"" + ipBytesToString(s.gatewayIp) + "\",";
    body += "\"fw_build\":\"" + String(FW_BUILD_TAG_VALUE) + "\",";
    body += "\"uptime_ms\":" + String(millis()) + ",";
    body += "\"signal_strength\":\"" + strength + "\",";
    body += "\"rssi\":" + String((int)rssi) + ",";
    body += "\"sim_info\":\"" + simDisplay + "\"";
    body += "}";
    request->send(200, "application/json", body);
  });

  // System Config endpoints: store site details in LittleFS
  server->on("/api/system-config", HTTP_GET, [](AsyncWebServerRequest *request) {
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
      body = "{\"site_name\":\"\",\"site_address\":\"\",\"timezone\":\"\"}";
    }
    request->send(200, "application/json", body);
  });

  server->on("/api/system-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    String site = request->hasParam("site_name", true) ? request->getParam("site_name", true)->value() : "";
    String addr = request->hasParam("site_address", true) ? request->getParam("site_address", true)->value() : "";
    String tz   = request->hasParam("timezone", true) ? request->getParam("timezone", true)->value() : "";
    site.trim(); addr.trim(); tz.trim();

    String json = "{\"site_name\":\"" + escapeJson(site) + "\",\"site_address\":\"" + escapeJson(addr) + "\",\"timezone\":\"" + escapeJson(tz) + "\"}";

    // Apply timezone immediately
    if (tz.length() > 0) {
      setenv("TZ", tz.c_str(), 1);
      tzset();
    }

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

  server->on("/api/restart-ntp", HTTP_POST, [](AsyncWebServerRequest *request) {
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
  server->on("/api/contacts/recipients", HTTP_GET, [](AsyncWebServerRequest *request) {
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

  server->on("/api/contacts/recipients", HTTP_POST, [](AsyncWebServerRequest *request) {
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
  server->on("/api/gateway-settings", HTTP_GET, [](AsyncWebServerRequest *request) {
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
    body += "\"gateway_ip\":\"" + ipBytesToString(s.gatewayIp) + "\"";
    body += "}";
    request->send(200, "application/json", body);
  });

  server->on("/api/gateway-settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    GatewaySettings s = {};
    Shared_getGatewaySettings(s);
    s.useDhcp = request->hasParam("use_dhcp", true) ? (request->getParam("use_dhcp", true)->value() == "1") : false;
    String sip = request->hasParam("static_ip", true) ? request->getParam("static_ip", true)->value() : "";
    String sm = request->hasParam("subnet_mask", true) ? request->getParam("subnet_mask", true)->value() : "";
    String gw = request->hasParam("gateway_ip", true) ? request->getParam("gateway_ip", true)->value() : "";
    if (!s.useDhcp) {
      if (!parseIPFromText(sip, s.staticIp) ||
          !parseIPFromText(sm, s.subnetMask) ||
          !parseIPFromText(gw, s.gatewayIp)) {
        request->send(400, "application/json", "{\"error\":\"Invalid static network settings\"}");
        return;
      }
    }

    s.httpPort = 80;

    if (!Shared_saveGatewaySettings(s)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

  // Dashboard IO Status endpoint: returns digital inputs, analog inputs, and relay outputs with current values and configs
  server->on("/api/io-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    SystemSnapshot snapshot = Shared_getSnapshot();
    
    String body = "{";
    body += "\"digital_inputs\":[";
    
    // Digital Inputs
    for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
      if (i) body += ",";
      DigitalInputConfig di = {};
      Shared_getDigitalInputConfig(i, di);
      body += "{";
      body += "\"index\":" + String(i) + ",";
      body += "\"name\":\"" + escapeJson(String(di.name)) + "\",";
      body += "\"normally_closed\":" + String(di.normallyClosed ? "true" : "false") + ",";
      body += "\"in_alarm\":" + String(snapshot.digitalInputs[i] ? "true" : "false") + ",";
      body += "\"enabled\":" + String(di.enabled ? "true" : "false");
      body += "}";
    }
    body += "],";
    
    body += "\"analog_inputs\":[";
    // Analog Inputs
    for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
      if (i) body += ",";
      AnalogInputConfig ai = {};
      Shared_getAnalogInputConfig(i, ai);
      bool aiAlarm = false;
      Shared_getAIAlarmState(i, aiAlarm);
      body += "{";
      body += "\"index\":" + String(i) + ",";
      body += "\"name\":\"" + escapeJson(String(ai.name)) + "\",";
      body += "\"value\":" + String(snapshot.analogInputs[i], 2) + ",";
      body += "\"enabled\":" + String(ai.enabled ? "true" : "false") + ",";
      body += "\"in_alarm\":" + String(aiAlarm ? "true" : "false");
      body += "}";
    }
    body += "],";
    
    // Timestamp from last I/O event
    char timeBuf[12] = "--:--:--";
    time_t lastEvent = Shared_getLastEventTime();
    if (lastEvent > 0) {
      struct tm *ti = localtime(&lastEvent);
      if (ti) strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", ti);
    }
    body += "\"timestamp\":\"" + String(timeBuf) + "\",";

    body += "\"relay_outputs\":[";
    // Relay Outputs
    for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
      if (i) body += ",";
      RelayConfig relay = {};
      Shared_getRelayConfig(i, relay);
      body += "{";
      body += "\"index\":" + String(i) + ",";
      body += "\"name\":\"" + escapeJson(String(relay.name)) + "\",";
      body += "\"state\":" + String(snapshot.relayState[i] ? "true" : "false") + ",";
      body += "\"enabled\":" + String(relay.enabled ? "true" : "false") + ",";
      body += "\"alarm_control_enabled\":" + String(relay.alarm_control_enabled ? "true" : "false") + ",";
      body += "\"alarm_source\":" + String((int)relay.alarm_source);
      body += "}";
    }
    body += "]";
    body += "}";
    
    request->send(200, "application/json", body);
  });

  // Digital Input Configuration endpoints
  server->on("/api/digital-input-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    String body = "[";
    for (size_t i = 0; i < DIGITAL_INPUT_COUNT; ++i) {
      if (i) body += ",";
      DigitalInputConfig cfg = {};
      Shared_getDigitalInputConfig(i, cfg);
      body += "{";
      body += "\"index\":" + String(i) + ",";
      body += "\"enabled\":" + String(cfg.enabled ? "true" : "false") + ",";
      body += "\"name\":\"" + escapeJson(String(cfg.name)) + "\",";
      body += "\"normally_closed\":" + String(cfg.normallyClosed ? "true" : "false") + ",";
      body += "\"tta_ms\":" + String(cfg.tta_ms / 1000) + ",";
      body += "\"ttr_ms\":" + String(cfg.ttr_ms / 1000) + ",";
      body += "\"alarm_sms_enabled\":" + String(cfg.alarm_sms_enabled ? "true" : "false") + ",";
      body += "\"return_sms_enabled\":" + String(cfg.return_sms_enabled ? "true" : "false") + ",";
      body += "\"alarm_message\":\"" + escapeJson(String(cfg.alarm_message)) + "\",";
      body += "\"return_message\":\"" + escapeJson(String(cfg.return_message)) + "\",";
      body += "\"selected_contacts\":" + String(cfg.selected_contacts);
      body += "}";
    }
    body += "]";
    request->send(200, "application/json", body);
  });

  server->on("/api/digital-input-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    // Expect index as parameter
    if (!request->hasParam("index", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing index parameter\"}");
      return;
    }

    size_t idx = request->getParam("index", true)->value().toInt();
    if (idx >= DIGITAL_INPUT_COUNT) {
      request->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      return;
    }

    DigitalInputConfig cfg = {};
    Shared_getDigitalInputConfig(idx, cfg);

    // Update fields from POST parameters
    if (request->hasParam("enabled", true)) cfg.enabled = (request->getParam("enabled", true)->value() == "1");
    if (request->hasParam("normally_closed", true)) cfg.normallyClosed = (request->getParam("normally_closed", true)->value() == "1");
    if (request->hasParam("name", true)) {
      String name = request->getParam("name", true)->value();
      strncpy(cfg.name, name.c_str(), sizeof(cfg.name) - 1);
      cfg.name[sizeof(cfg.name) - 1] = '\0';
    }
    if (request->hasParam("tta_ms", true)) {
      int val = request->getParam("tta_ms", true)->value().toInt() * 1000;
      cfg.tta_ms = (val >= 1000) ? (uint32_t)val : 1000;
    }
    if (request->hasParam("ttr_ms", true)) {
      int val = request->getParam("ttr_ms", true)->value().toInt() * 1000;
      cfg.ttr_ms = (val >= 1000) ? (uint32_t)val : 1000;
    }
    if (request->hasParam("alarm_sms_enabled", true)) cfg.alarm_sms_enabled = (request->getParam("alarm_sms_enabled", true)->value() == "1");
    if (request->hasParam("return_sms_enabled", true)) cfg.return_sms_enabled = (request->getParam("return_sms_enabled", true)->value() == "1");
    if (request->hasParam("alarm_message", true)) {
      String msg = request->getParam("alarm_message", true)->value();
      strncpy(cfg.alarm_message, msg.c_str(), sizeof(cfg.alarm_message) - 1);
      cfg.alarm_message[sizeof(cfg.alarm_message) - 1] = '\0';
    }
    if (request->hasParam("return_message", true)) {
      String msg = request->getParam("return_message", true)->value();
      strncpy(cfg.return_message, msg.c_str(), sizeof(cfg.return_message) - 1);
      cfg.return_message[sizeof(cfg.return_message) - 1] = '\0';
    }
    if (request->hasParam("selected_contacts", true)) {
      int val = request->getParam("selected_contacts", true)->value().toInt();
      cfg.selected_contacts = (uint8_t)(val & 0xFF);
    }

    if (!Shared_saveDigitalInputConfig(idx, cfg)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }

    request->send(200, "application/json", "{\"success\":true}");
  });

  server->on("/api/analog-input-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    String body = "[";
    for (size_t i = 0; i < ANALOG_INPUT_COUNT; ++i) {
      if (i) body += ",";
      AnalogInputConfig cfg = {};
      Shared_getAnalogInputConfig(i, cfg);
      body += "{";
      body += "\"index\":" + String(i) + ",";
      body += "\"enabled\":" + String(cfg.enabled ? "true" : "false") + ",";
      body += "\"name\":\"" + escapeJson(String(cfg.name)) + "\",";
      body += "\"engineering_unit\":\"" + escapeJson(String(cfg.engineering_unit)) + "\",";
      body += "\"scale_low\":" + String(cfg.scale_low, 4) + ",";
      body += "\"scale_high\":" + String(cfg.scale_high, 4) + ",";
      body += "\"alarm_type\":" + String(cfg.alarm_type) + ",";
      body += "\"set_point\":" + String(cfg.set_point, 4) + ",";
      body += "\"reset_point\":" + String(cfg.reset_point, 4) + ",";
      body += "\"tta_ms\":" + String(cfg.tta_ms / 1000) + ",";
      body += "\"ttr_ms\":" + String(cfg.ttr_ms / 1000) + ",";
      body += "\"alarm_sms_enabled\":" + String(cfg.alarm_sms_enabled ? "true" : "false") + ",";
      body += "\"return_sms_enabled\":" + String(cfg.return_sms_enabled ? "true" : "false") + ",";
      body += "\"alarm_message\":\"" + escapeJson(String(cfg.alarm_message)) + "\",";
      body += "\"return_message\":\"" + escapeJson(String(cfg.return_message)) + "\",";
      body += "\"selected_contacts\":" + String(cfg.selected_contacts);
      body += "}";
    }
    body += "]";
    request->send(200, "application/json", body);
  });

  server->on("/api/analog-input-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    if (!request->hasParam("index", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing index parameter\"}");
      return;
    }

    size_t idx = request->getParam("index", true)->value().toInt();
    if (idx >= ANALOG_INPUT_COUNT) {
      request->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      return;
    }

    AnalogInputConfig cfg = {};
    Shared_getAnalogInputConfig(idx, cfg);

    // Update fields from POST parameters
    if (request->hasParam("enabled", true)) cfg.enabled = (request->getParam("enabled", true)->value() == "1");
    if (request->hasParam("name", true)) {
      String name = request->getParam("name", true)->value();
      strncpy(cfg.name, name.c_str(), sizeof(cfg.name) - 1);
      cfg.name[sizeof(cfg.name) - 1] = '\0';
    }
    if (request->hasParam("engineering_unit", true)) {
      String unit = request->getParam("engineering_unit", true)->value();
      strncpy(cfg.engineering_unit, unit.c_str(), sizeof(cfg.engineering_unit) - 1);
      cfg.engineering_unit[sizeof(cfg.engineering_unit) - 1] = '\0';
    }
    if (request->hasParam("scale_low", true)) cfg.scale_low = request->getParam("scale_low", true)->value().toFloat();
    if (request->hasParam("scale_high", true)) cfg.scale_high = request->getParam("scale_high", true)->value().toFloat();
    if (request->hasParam("alarm_type", true)) {
      int val = request->getParam("alarm_type", true)->value().toInt();
      cfg.alarm_type = (val >= 0 && val <= 3) ? (uint8_t)val : 0;
    }
    if (request->hasParam("set_point", true)) cfg.set_point = request->getParam("set_point", true)->value().toFloat();
    if (request->hasParam("reset_point", true)) cfg.reset_point = request->getParam("reset_point", true)->value().toFloat();
    if (request->hasParam("tta_ms", true)) {
      int val = request->getParam("tta_ms", true)->value().toInt() * 1000;
      cfg.tta_ms = (val >= 1000) ? (uint32_t)val : 1000;
    }
    if (request->hasParam("ttr_ms", true)) {
      int val = request->getParam("ttr_ms", true)->value().toInt() * 1000;
      cfg.ttr_ms = (val >= 1000) ? (uint32_t)val : 1000;
    }
    if (request->hasParam("alarm_sms_enabled", true)) cfg.alarm_sms_enabled = (request->getParam("alarm_sms_enabled", true)->value() == "1");
    if (request->hasParam("return_sms_enabled", true)) cfg.return_sms_enabled = (request->getParam("return_sms_enabled", true)->value() == "1");
    if (request->hasParam("alarm_message", true)) {
      String msg = request->getParam("alarm_message", true)->value();
      strncpy(cfg.alarm_message, msg.c_str(), sizeof(cfg.alarm_message) - 1);
      cfg.alarm_message[sizeof(cfg.alarm_message) - 1] = '\0';
    }
    if (request->hasParam("return_message", true)) {
      String msg = request->getParam("return_message", true)->value();
      strncpy(cfg.return_message, msg.c_str(), sizeof(cfg.return_message) - 1);
      cfg.return_message[sizeof(cfg.return_message) - 1] = '\0';
    }
    if (request->hasParam("selected_contacts", true)) {
      int val = request->getParam("selected_contacts", true)->value().toInt();
      cfg.selected_contacts = (uint8_t)(val & 0xFF);
    }

    if (!Shared_saveAnalogInputConfig(idx, cfg)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }

    request->send(200, "application/json", "{\"success\":true}");
  });

  // Relay (DO) Config endpoints
  server->on("/api/relay-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Not authenticated\"}");
      return;
    }
    String body = "[";
    for (size_t i = 0; i < RELAY_OUTPUT_COUNT; ++i) {
      RelayConfig cfg = {};
      Shared_getRelayConfig(i, cfg);
      if (i > 0) body += ",";
      body += "{\"index\":" + String((int)i) + ",";
      body += "\"enabled\":" + String(cfg.enabled ? "true" : "false") + ",";
      body += "\"name\":\"" + escapeJson(String(cfg.name)) + "\",";
      body += "\"default_power_up_state\":" + String(cfg.default_power_up_state ? "true" : "false") + ",";
      body += "\"sms_control_enabled\":" + String(cfg.sms_control_enabled ? "true" : "false") + ",";
      body += "\"alarm_control_enabled\":" + String(cfg.alarm_control_enabled ? "true" : "false") + ",";
      body += "\"alarm_source\":" + String((int)cfg.alarm_source) + ",";
      body += "\"selected_contacts\":" + String((int)cfg.selected_contacts) + "}";
    }
    body += "]";
    request->send(200, "application/json", body);
  });

  server->on("/api/relay-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Not authenticated\"}");
      return;
    }
    if (!request->hasParam("index", true)) {
      request->send(400, "application/json", "{\"error\":\"Missing index parameter\"}");
      return;
    }
    int idx = request->getParam("index", true)->value().toInt();
    if (idx < 0 || idx >= (int)RELAY_OUTPUT_COUNT) {
      request->send(400, "application/json", "{\"error\":\"Invalid index\"}");
      return;
    }

    RelayConfig cfg = {};
    Shared_getRelayConfig(idx, cfg);

    if (request->hasParam("enabled", true)) {
      cfg.enabled = (request->getParam("enabled", true)->value() == "1");
    }
    if (request->hasParam("name", true)) {
      String name = request->getParam("name", true)->value();
      name.toCharArray(cfg.name, sizeof(cfg.name));
      cfg.name[sizeof(cfg.name) - 1] = '\0';
    }
    if (request->hasParam("default_power_up_state", true)) {
      cfg.default_power_up_state = (request->getParam("default_power_up_state", true)->value() == "1");
    }
    if (request->hasParam("sms_control_enabled", true)) {
      cfg.sms_control_enabled = (request->getParam("sms_control_enabled", true)->value() == "1");
    }
    if (request->hasParam("alarm_control_enabled", true)) {
      cfg.alarm_control_enabled = (request->getParam("alarm_control_enabled", true)->value() == "1");
    }
    if (request->hasParam("alarm_source", true)) {
      int val = request->getParam("alarm_source", true)->value().toInt();
      cfg.alarm_source = (uint8_t)(val & 0xFF);
    }
    if (request->hasParam("selected_contacts", true)) {
      int val = request->getParam("selected_contacts", true)->value().toInt();
      cfg.selected_contacts = (uint8_t)(val & 0xFF);
    }

    if (!Shared_saveRelayConfig(idx, cfg)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }

    request->send(200, "application/json", "{\"success\":true}");
  });

  // SIM Configuration endpoints
  server->on("/api/sim-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    SIMConfig cfg = {};
    Shared_getSIMConfig(cfg);

    String body = "{";
    body += "\"service_provider\":\"" + String(cfg.service_provider) + "\",";
    body += "\"phone_number\":\"" + String(cfg.phone_number) + "\",";
    body += "\"relay_pin\":\"" + String(cfg.relay_pin) + "\"";
    body += "}";
    request->send(200, "application/json", body);
  });

  server->on("/api/sim-config", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    SIMConfig cfg = {};
    if (request->hasParam("service_provider", true)) {
      String provider = request->getParam("service_provider", true)->value();
      provider.toCharArray(cfg.service_provider, sizeof(cfg.service_provider));
      cfg.service_provider[sizeof(cfg.service_provider) - 1] = '\0';
    }
    if (request->hasParam("phone_number", true)) {
      String phone = request->getParam("phone_number", true)->value();
      phone.toCharArray(cfg.phone_number, sizeof(cfg.phone_number));
      cfg.phone_number[sizeof(cfg.phone_number) - 1] = '\0';
    }
    if (request->hasParam("relay_pin", true)) {
      String pin = request->getParam("relay_pin", true)->value();
      pin.toCharArray(cfg.relay_pin, sizeof(cfg.relay_pin));
      cfg.relay_pin[sizeof(cfg.relay_pin) - 1] = '\0';
    }

    if (!Shared_saveSIMConfig(cfg)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }

    request->send(200, "application/json", "{\"success\":true}");
  });

  // Signal strength endpoint
  server->on("/api/signal-strength", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    int8_t rssi = Modem_getSignalStrength();
    String strength = "Unknown";
    if (rssi == -2) {
      strength = "SIM Not Inserted";
    } else if (rssi >= 0) {
      if (rssi >= 25) strength = "Excellent (" + String(rssi) + "/31)";
      else if (rssi >= 20) strength = "Very Good (" + String(rssi) + "/31)";
      else if (rssi >= 15) strength = "Good (" + String(rssi) + "/31)";
      else if (rssi >= 10) strength = "Fair (" + String(rssi) + "/31)";
      else if (rssi >= 5) strength = "Weak (" + String(rssi) + "/31)";
      else strength = "Very Weak (" + String(rssi) + "/31)";
    }

    String body = "{\"rssi\":" + String((int)rssi) + ",\"strength\":\"" + strength + "\"}";
    request->send(200, "application/json", body);
  });

  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    sendHtmlPage(request);
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
  // DISABLED: Serial number should persist across firmware updates
  // clearStaleSerialForNewBuild();

  // Keep Web UI always active (Ethernet IP + AP IP when AP mode is enabled).
  if (server == nullptr) {
    server = new (std::nothrow) AsyncWebServer(80);
    if (server == nullptr) {
      Serial.println("[WEB] ERROR: Failed to allocate config server");
      vTaskDelete(nullptr);
      return;
    }
  }
  setupWebServerRoutes();
  if (!serverStarted) {
    server->begin();
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

static const char *htmlPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>RAMS Dashboard</title>
<link rel="icon" type="image/png" href="/Gaurangalogo.png?v=2">
<style>
  :root{--bg:#f3f6f9;--muted:#6b7280;--primary:#1565c0;--card:#eef7ff}
  *{box-sizing:border-box}
  body{margin:0;font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:#0f172a}
  .layout{display:flex;min-height:100vh}
  .sidebar{width:240px;background:#fff;border-right:1px solid #e6eef7;padding:20px}
  .brand{font-weight:700;color:var(--primary);font-size:18px;margin-bottom:12px;display:flex;align-items:center;gap:8px}
  .brand img{width:32px;height:32px;object-fit:contain;flex-shrink:0}
  .nav{list-style:none;padding:0;margin:0}
  .nav li{padding:10px 12px;border-radius:8px;color:#475569;cursor:pointer;margin-bottom:6px}
  .nav li.active{background:#e8f6ff;color:var(--primary);font-weight:600}
  .content{flex:1;padding:20px}
  .topbar{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px}
  .actions{display:flex;gap:8px;align-items:center}
  .uptime{padding:8px 12px;border-radius:8px;background:#fff;border:1px solid #dfe6ee;color:#334155;font-size:13px;font-weight:700;white-space:nowrap}
  .uptime span{color:var(--muted);font-weight:600;margin-right:4px}
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
      <div class="brand"><img src="/Gaurangalogo.png?v=2" alt="Logo">RAMS</div>
      <ul class="nav" id="nav">
        <li data-tab="dashboard" class="active">Dashboard</li>
        <li data-tab="digital">DI Config</li>
        <li data-tab="analog">AI Config</li>
        <li data-tab="relays">DO Config</li>
        <li data-tab="phones">Contact Config</li>
        <li data-tab="network">Network Config</li>
        <li data-tab="sysconfig">System Config</li>

      </ul>
    </div>
    <div class="content">
      <div class="topbar">
        <div>
          <h2 id="topbar_title">Dashboard</h2>
          <div class="subtitle" id="topbar_subtitle">Current Configuration Status</div>
        </div>
        <div class="actions">
          <div class="uptime"><span>UP Time</span><strong id="uptime_counter">--:--:--</strong></div>
          <button class="btn danger" onclick="location.href='/logout'">Logout</button>
        </div>
      </div>

      <div id="dashboard" class="tab active">
        <div class="panel">
          <h2 style="margin-bottom:15px">System Information</h2>
          <div class="grid" id="dashGrid">
            <div class="stat"><div class="label">Serial Number</div><div class="value" id="s_serial">Loading...</div></div>
            <div class="stat"><div class="label">Site Name & Location</div><div class="value" id="s_site">Loading...</div></div>
            <div class="stat"><div class="label">Wi-Fi AP IP</div><div class="value" id="s_apip">Loading...</div></div>
            <div class="stat"><div class="label">Ethernet IP</div><div class="value" id="s_ethip">Loading...</div></div>
            <div class="stat"><div class="label">DHCP Mode</div><div class="value" id="s_dhcp">Loading...</div></div>
            <div class="stat"><div class="label">Static IP</div><div class="value" id="s_static">Loading...</div></div>
            <div class="stat"><div class="label">4G Signal Strength</div><div class="value" id="s_signal">Loading...</div></div>
            <div class="stat"><div class="label">SIM Information</div><div class="value" id="s_siminfo">Loading...</div></div>
          </div>
        </div>
        
        <div class="panel" style="margin-top:20px">
          <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:15px">
            <h2 style="margin:0">Input &amp; Output Status</h2>
            <span style="font-size:13px;color:#6b7280;font-weight:600">Last Updated: <strong id="io_timestamp">--:--:--</strong></span>
          </div>
          
          <div style="margin-bottom:20px">
            <h3 style="font-size:14px;margin-bottom:10px;color:#555">-Digital Inputs</h3>
            <table style="width:100%;border-collapse:collapse;table-layout:fixed">
              <thead style="background:#f9fafb">
                <tr>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Input</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Name</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Type</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Status</th>
                </tr>
              </thead>
              <tbody id="di-table">
                <tr><td colspan="4" style="padding:10px;text-align:center;color:#999">Loading...</td></tr>
              </tbody>
            </table>
          </div>

          <div style="margin-bottom:20px">
            <h3 style="font-size:14px;margin-bottom:10px;color:#555">-Analog Inputs</h3>
            <table style="width:100%;border-collapse:collapse;table-layout:fixed">
              <thead style="background:#f9fafb">
                <tr>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Input</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Name</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Value</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Status</th>
                </tr>
              </thead>
              <tbody id="ai-table">
                <tr><td colspan="4" style="padding:10px;text-align:center;color:#999">Loading...</td></tr>
              </tbody>
            </table>
          </div>

          <div>
            <h3 style="font-size:14px;margin-bottom:10px;color:#555">-Digital Outputs</h3>
            <table style="width:100%;border-collapse:collapse;table-layout:fixed">
              <thead style="background:#f9fafb">
                <tr>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Output</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Name</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">State</th>
                  <th style="padding:10px;text-align:left;font-weight:600;border-bottom:2px solid #e5e7eb">Alarm Link</th>
                </tr>
              </thead>
              <tbody id="relay-table">
                <tr><td colspan="4" style="padding:10px;text-align:center;color:#999">Loading...</td></tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <div id="digital" class="tab" style="display:none">
        <div class="panel">
          
          <!-- Selector Section -->
          <div style="margin-bottom:28px;padding-bottom:20px;border-bottom:2px solid #eee;display:flex;align-items:center;gap:30px">
            <div style="display:flex;align-items:center;gap:12px">
              <label style="font-weight:600;font-size:14px;white-space:nowrap">Select Digital Input</label>
              <select id="di_selector" onchange="switchDI(this.value)" style="padding:10px 12px;font-size:14px;width:120px;border:1px solid #ccc;border-radius:4px;background-color:#fff;cursor:pointer">
                <option value="0">DI1</option>
                <option value="1">DI2</option>
                <option value="2">DI3</option>
                <option value="3">DI4</option>
              </select>
            </div>
            <div style="display:flex;align-items:center;gap:8px">
              <input type="checkbox" id="di_enabled" onchange="updateDIFieldsState()" style="width:18px;height:18px;cursor:pointer">
              <label style="font-weight:500;font-size:13px;cursor:pointer;white-space:nowrap">Enable This Input</label>
            </div>
          </div>
          
          <div id="di_form_container" style="display:none">
            <form id="di_form">
              <!-- Basic Settings Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #5c00d4">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Basic Settings</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Input Name</label>
                    <input type="text" id="di_name" placeholder="e.g. Main Door" maxlength="31" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Input Type</label>
                    <select id="di_type" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;background-color:#fff;cursor:pointer;box-sizing:border-box">
                      <option value="0">Alarm on Close (Normally Open)</option>
                      <option value="1">Alarm on Open (Normally Closed)</option>
                    </select>
                  </div>
                </div>
              </div>
              
              <!-- Timing Settings Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #2196F3">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Timing Configuration</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Time to Alarm (seconds)</label>
                    <input type="number"
                    id="di_tta"
                    min="1"
                    max="65535"
                    oninput="if(this.value>65535)this.value=65535;if(this.value<1&&this.value!='')this.value=1;"
                    style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">                    
                    <div style="font-size:11px;color:#999;margin-top:4px">Duration before triggering alarm (1-65535s)</div>
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Time to Return (seconds)</label>
                    <input type="number"
                    id="di_ttr"
                    min="1"
                    max="65535"
                    oninput="if(this.value>65535)this.value=65535;if(this.value<1&&this.value!='')this.value=1;"
                    style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">                    
                    <div style="font-size:11px;color:#999;margin-top:4px">Duration after alarm to return (1-65535s)</div>
                  </div>
                </div>
              </div>
              
              <!-- SMS Notification Settings Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #FF9800">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">SMS Notifications</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:14px">
                  <div style="display:flex;align-items:center;gap:8px">
                    <input type="checkbox" id="di_alarm_sms" style="width:18px;height:18px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">Send Alarm SMS</label>
                  </div>
                  <div style="display:flex;align-items:center;gap:8px">
                    <input type="checkbox" id="di_return_sms" style="width:18px;height:18px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">Send Return SMS</label>
                  </div>
                </div>
                <div style="display:grid;grid-template-columns:1fr;gap:14px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Alarm Message</label>
                    <input type="text" id="di_alarm_msg" placeholder="Message when alarm occurs" maxlength="63" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div style="font-size:11px;color:#999;margin-top:4px">Max 63 characters</div>
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Return Message</label>
                    <input type="text" id="di_return_msg" placeholder="Message when alarm clears" maxlength="63" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div style="font-size:11px;color:#999;margin-top:4px">Max 63 characters</div>
                  </div>
                </div>
              </div>
              
              <!-- Event Recipients Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #4CAF50">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Select Contact</h3>
                <div>
                  <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Notify Recipients on Alarm (checkbox)</label>
                  <div id="di_recipients_select"
                  style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;background-color:#fff;min-height:80px">
                  <div style="color:#999">(no recipients configured)</div>
                  </div>
                </div>
              </div>
              
              <!-- Action Button -->
              <div id="di_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
              <div style="display:flex;gap:10px;margin-top:28px">
                <button type="button" class="btn primary" onclick="saveDIConfig()" style="flex:1;padding:12px 24px;font-size:14px;font-weight:600">Save Configuration</button>
              </div>
            </form>
          </div>
          <div style="text-align:center;padding:20px;color:#999" id="di_loading">Loading Digital Input configurations...</div>
        </div>
      </div>
      <div id="analog" class="tab" style="display:none">
        <div class="panel">
          
          <!-- Selector Section -->
          <div style="margin-bottom:28px;padding-bottom:20px;border-bottom:2px solid #eee;display:flex;align-items:center;gap:30px">
            <div style="display:flex;align-items:center;gap:12px">
              <label style="font-weight:600;font-size:14px;white-space:nowrap">Select Analog Input</label>
              <select id="ai_selector" onchange="switchAI(this.value)" style="padding:10px 12px;font-size:14px;width:120px;border:1px solid #ccc;border-radius:4px;background-color:#fff;cursor:pointer">
                <option value="0">AI1</option>
                <option value="1">AI2</option>
              </select>
            </div>
            <div style="display:flex;align-items:center;gap:8px">
              <input type="checkbox" id="ai_enabled" onchange="updateAIFieldsState()" style="width:18px;height:18px;cursor:pointer">
              <label style="font-weight:500;font-size:13px;cursor:pointer;white-space:nowrap">Enable This Input</label>
            </div>
          </div>
          
          <div id="ai_form_container" style="display:none">
            <form id="ai_form">
              <!-- Basic Settings Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #c200d4">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Basic Settings</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Input Name</label>
                    <input type="text" id="ai_name" placeholder="e.g. Tank Level" maxlength="31" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Engineering Unit</label>
                    <select id="ai_unit_select" onchange="toggleAIUnitCustom(true)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;background-color:#fff">
                      <option value="Liters">Liters</option>
                      <option value="Percentage (%)">Percentage (%)</option>
                      <option value="Bar">Bar</option>
                      <option value="PSI">PSI</option>
                      <option value="&deg;C">&deg;C</option>
                      <option value="&deg;F">&deg;F</option>
                      <option value="mA">mA</option>
                      <option value="Volts">Volts</option>
                      <option value="__custom__">Custom Text</option>
                    </select>
                    <input type="text" id="ai_unit_custom" placeholder="Custom unit" maxlength="15" style="display:none;width:100%;margin-top:8px;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                  </div>
                </div>
              </div>
              
              <!-- Scaling Configuration Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #4CAF50">

              <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Scaling (4-20mA)</h3>
              <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
              <div>
              <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Scale Low (4mA)</label>
              <input type="number"
              id="ai_scale_low"
              min="-999999"
              max="999999"
              step="0.01"
              oninput="if(this.value>999999)this.value=999999;if(this.value<-999999)this.value=-999999"
              style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              <div style="font-size:11px;color:#999;margin-top:4px">Engineering value at 4mA (-999999 to 999999)</div>
              </div>
              <div>
              <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Scale High (20mA)</label>
              <input type="number"
              id="ai_scale_high"
              min="-999999"
              max="999999"
              step="0.01"
              oninput="if(this.value>999999)this.value=999999;if(this.value<-999999)this.value=-999999"
              style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              <div style="font-size:11px;color:#999;margin-top:4px">Engineering value at 20mA (-999999 to 999999)</div>
            </div>

          </div>
        </div>
              
              <!-- Alarm Type Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #2196F3">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Alarm Type</h3>
                <div style="display:flex;gap:16px;flex-wrap:wrap">
                  <div style="display:flex;align-items:center;gap:6px">
                    <input type="radio" id="ai_type_high" name="ai_alarm_type" value="0" onchange="updateAIThresholdHints()" style="width:16px;height:16px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">High Alarm</label>
                  </div>
                  <div style="display:flex;align-items:center;gap:6px">
                    <input type="radio" id="ai_type_low" name="ai_alarm_type" value="1" onchange="updateAIThresholdHints()" style="width:16px;height:16px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">Low Alarm</label>
                  </div>
                  <div style="display:flex;align-items:center;gap:6px">
                    <input type="radio" id="ai_type_inband" name="ai_alarm_type" value="2" onchange="updateAIThresholdHints()" style="width:16px;height:16px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">In-Band Alarm</label>
                  </div>
                  <div style="display:flex;align-items:center;gap:6px">
                    <input type="radio" id="ai_type_outband" name="ai_alarm_type" value="3" onchange="updateAIThresholdHints()" style="width:16px;height:16px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">Out-of-Band Alarm</label>
                  </div>
                </div>
              </div>
              
              <!-- Alarm Thresholds Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #FF9800">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Alarm Thresholds</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Set Point</label>
                    <input type="number"
                    id="ai_set_point"
                    step="0.01"
                    oninput="if(this.value.length>12)this.value=this.value.slice(0,12);updateAIThresholdHints()"
                    style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div id="ai_set_point_hint" style="font-size:11px;color:#999;margin-top:4px">Alarm triggers at this value</div>
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Reset Point</label>
                    <input type="number"
                    id="ai_reset_point"
                    step="0.01"
                    oninput="if(this.value.length>12)this.value=this.value.slice(0,12);updateAIThresholdHints()"
                    style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div id="ai_reset_point_hint" style="font-size:11px;color:#999;margin-top:4px">Alarm clears at this value</div>
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Time To Alarm (seconds)</label>
                    <input type="number" id="ai_tta" min="1" max="65535" value="1"
                    oninput="if(this.value>65535)this.value=65535;if(this.value<1&&this.value!='')this.value=1;"
                    style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div style="font-size:11px;color:#999;margin-top:4px">Delay before alarm triggers  (1-65535s)</div>
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Time To Return (seconds)</label>
                    <input type="number" id="ai_ttr" min="1" max="65535" value="1"
                    oninput="if(this.value>65535)this.value=65535;if(this.value<1&&this.value!='')this.value=1;"
                    style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div style="font-size:11px;color:#999;margin-top:4px">Delay before alarm clears (1-65535s)</div>
                  </div>
                </div>
              </div>
              
              <!-- SMS Notification Settings Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #9C27B0">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">SMS Notifications</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px;margin-bottom:14px">
                  <div style="display:flex;align-items:center;gap:8px">
                    <input type="checkbox" id="ai_alarm_sms" style="width:18px;height:18px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">Send Alarm SMS</label>
                  </div>
                  <div style="display:flex;align-items:center;gap:8px">
                    <input type="checkbox" id="ai_return_sms" style="width:18px;height:18px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer">Send Return SMS</label>
                  </div>
                </div>
                <div style="display:grid;grid-template-columns:1fr;gap:14px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Alarm Message</label>
                    <input type="text" id="ai_alarm_msg" placeholder="e.g. HIGH TANK LEVEL" maxlength="63" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div style="font-size:11px;color:#999;margin-top:4px">Max 63 characters</div>
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Return Message</label>
                    <input type="text" id="ai_return_msg" placeholder="e.g. TANK LEVEL RETURNED TO NORMAL" maxlength="63" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                    <div style="font-size:11px;color:#999;margin-top:4px">Max 63 characters</div>
                  </div>
                </div>
              </div>
              
              <!-- Event Recipients Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #4CAF50">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Select Contact</h3>
                <div>
                  <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Notify Recipients on Alarm (checkbox)</label>
                  <div id="ai_recipients_select" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;background-color:#fff;min-height:80px">
                  <div style="color:#999">(no recipients configured)</div>
                  </div>
                </div>
              </div>
              
              <!-- Action Button -->
              <div id="ai_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
              <div style="display:flex;gap:10px;margin-top:28px">
                <button type="button" class="btn primary" onclick="saveAIConfig()" style="flex:1;padding:12px 24px;font-size:14px;font-weight:600">Save Configuration</button>
              </div>
            </form>
          </div>
          <div style="text-align:center;padding:20px;color:#999" id="ai_loading">Loading Analog Input configurations...</div>
        </div>
      </div>
      <div id="relays" class="tab" style="display:none">
        <div class="panel">
          
          <!-- Selector Section -->
          <div style="margin-bottom:28px;padding-bottom:20px;border-bottom:2px solid #eee;display:flex;align-items:center;gap:30px">
            <div style="display:flex;align-items:center;gap:12px">
              <label style="font-weight:600;font-size:14px;white-space:nowrap">Select Output</label>
              <select id="do_selector" onchange="switchDO(parseInt(this.value))" style="padding:10px 12px;font-size:14px;width:120px;border:1px solid #ccc;border-radius:4px;background-color:#fff;cursor:pointer">
                <option value="0">DO1</option>
                <option value="1">DO2</option>
              </select>
            </div>
            <div style="display:flex;align-items:center;gap:8px">
              <input type="checkbox" id="do_enabled" onchange="updateDOFieldsState()" style="width:18px;height:18px;cursor:pointer">
              <label style="font-weight:500;font-size:13px;cursor:pointer;white-space:nowrap">Enable This Output</label>
            </div>
          </div>
          
          <div id="do_form_container">
            <form id="do_form">
              <!-- Basic Settings Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #d40000">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Basic Settings</h3>
                <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Output Name</label>
                    <input type="text" id="do_name" placeholder="e.g. Siren, Beacon" maxlength="31" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
                  </div>
                  <div>
                    <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Default Power-Up State</label>
                    <select id="do_powerup" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;background-color:#fff;cursor:pointer;box-sizing:border-box">
                      <option value="0">OFF</option>
                      <option value="1">ON</option>
                    </select>
                  </div>
                </div>
              </div>
              
              <!-- Control Configuration Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #FF9800">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Control Configuration</h3>
                <div style="display:flex;flex-direction:column;gap:14px">
                  <div style="display:flex;align-items:center;gap:8px">
                    <input type="checkbox" id="do_sms_control" style="width:18px;height:18px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer;margin:0">Enable SMS Control</label>
                  </div>
                  <div style="font-size:11px;color:#999;margin-left:26px;margin-top:-10px">Allows remote control: DO1 ON, DO1 OFF, DO1 PULSE 30</div>
                  <div style="display:flex;align-items:center;gap:8px">
                    <input type="checkbox" id="do_alarm_control" onchange="updateDOFieldsState()" style="width:18px;height:18px;cursor:pointer">
                    <label style="font-weight:500;font-size:13px;cursor:pointer;margin:0">Enable Alarm Control</label>
                  </div>
                  <div style="font-size:11px;color:#999;margin-left:26px;margin-top:-10px">Output activates automatically when linked alarm triggers</div>
                </div>
              </div>
              
              <!-- Alarm Linking Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #2196F3">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Alarm Linking</h3>
                <div>
                  <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Link to Alarm Source</label>
                  <select id="do_alarm_source" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;background-color:#fff;cursor:pointer">
                    <option value="0">None (Manual / SMS only)</option>
                    <option value="1">AI1 - Analog Alarm</option>
                    <option value="2">AI2 - Analog Alarm</option>
                    <option value="3">DI1 - Digital Alarm</option>
                    <option value="4">DI2 - Digital Alarm</option>
                    <option value="5">DI3 - Digital Alarm</option>
                    <option value="6">DI4 - Digital Alarm</option>
                  </select>
                  <div style="font-size:11px;color:#999;margin-top:4px">Select alarm to link (if Alarm Control enabled)</div>
                </div>
              </div>
              
              <!-- Event Recipients Section -->
              <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #4CAF50">
                <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Select Contact</h3>
                <div>
                  <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Notify Recipients on Alarm (checkbox)</label>
                  <div id="relay_recipients_select" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;background-color:#fff;min-height:80px">
                  <div style="color:#999">(no recipients configured)</div>
                  </div>
                </div>
              </div>
              
              <!-- Save Button -->
              <div id="do_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
              <div style="margin-top:24px;display:flex;gap:12px">
                <button type="button" onclick="saveDOConfig()" style="flex:1;padding:12px;background:#0066cc;color:#fff;border:none;border-radius:4px;font-weight:600;cursor:pointer;font-size:14px">Save Configuration</button>
              </div>
            </form>
          </div>
        </div>
      </div>
      <!-- Alarm Management tab removed -->
      <div id="phones" class="tab" style="display:none">
        <div class="panel">
          <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #4CAF50">
            <h3 style="font-size:14px;font-weight:600;margin:0 0 6px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Event Recipients</h3>
            <div style="font-size:12px;color:#999;margin-bottom:14px">Phone numbers accept digits and optional leading '+', 10-15 digits.</div>
            <div id="rec_list"></div>
            <button class="btn" onclick="addRecContact()" style="margin-top:10px">Add Recipient</button>
          </div>
          <div id="phones_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
          <div style="display:flex;gap:10px;margin-top:8px">
            <button class="btn primary" onclick="saveContacts()" style="flex:1;padding:12px 24px;font-size:14px;font-weight:600">Save Contacts</button>
          </div>
        </div>
      </div>

      <div id="network" class="tab" style="display:none">
        <div class="panel">
          <!-- Connection Mode -->
          <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #2196F3">
            <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Connection Mode</h3>
            <div style="display:flex;align-items:center;gap:8px">
              <input id="net_useDhcp" type="checkbox" style="width:18px;height:18px;cursor:pointer" onchange="toggleNetworkStaticFields()">
              <label for="net_useDhcp" style="font-weight:500;font-size:13px;cursor:pointer;margin:0">Use DHCP for Ethernet</label>
            </div>
          </div>
          <!-- Static Network Settings -->
          <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #FF9800">
            <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Static Network Settings</h3>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Static IP</label>
                <input id="net_staticIp" type="text" placeholder="192.168.8.200" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Subnet Mask</label>
                <input id="net_subnetMask" type="text" placeholder="255.255.255.0" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Gateway IP</label>
                <input id="net_gatewayIp" type="text" placeholder="192.168.8.1" inputmode="numeric" pattern="[0-9.]+" maxlength="15" oninput="sanitizeIpInput(this)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
            </div>
            <div style="font-size:11px;color:#999;margin-top:10px">Reboot device after saving to apply network changes.</div>
          </div>
          <div id="net_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
          <div style="display:flex;gap:10px;margin-bottom:28px">
            <button class="btn primary" onclick="saveNetworkCfg()" style="flex:1;padding:12px 24px;font-size:14px;font-weight:600">Save Network Settings</button>
          </div>
          <!-- SIM Configuration -->
          <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #9C27B0">
            <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">SIM Configuration</h3>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Service Provider</label>
                <input id="sim_provider" type="text" placeholder="e.g., Vodafone, AT&T" maxlength="63" oninput="this.value=this.value.substring(0,63)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">SIM Phone Number</label>
                <input id="sim_phone" type="text" placeholder="e.g., +1234567890" maxlength="19" oninput="let v=this.value.replace(/[^0-9+]/g,'');if(v.startsWith('+'))v='+'+v.substring(1).replace(/\+/g,'');else v=v.replace(/\+/g,'');this.value=v;" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">SMS Command PIN</label>
                <input id="sim_relay_pin" type="text" placeholder="e.g. 0000" maxlength="15" oninput="this.value=this.value.replace(/[^0-9]/g,'').slice(0,15)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
            </div>
          </div>
          <div id="sim_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
          <div style="display:flex;gap:10px">
            <button class="btn primary" onclick="saveSIMConfig()" style="flex:1;padding:12px 24px;font-size:14px;font-weight:600">Save SIM Configuration</button>
          </div>
        </div>
      </div>
      <div id="sysconfig" class="tab" style="display:none">
        <div class="panel">
          <div style="margin-bottom:24px;padding:16px;background-color:#f9f9f9;border-radius:6px;border-left:4px solid #f38721">
            <h3 style="font-size:14px;font-weight:600;margin:0 0 14px 0;color:#333;text-transform:uppercase;letter-spacing:0.5px">Site Details</h3>
            <div style="display:grid;grid-template-columns:1fr 1fr;gap:20px">
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Site Name</label>
                <input id="site_name" type="text" maxlength="63" oninput="if(this.value.length>63)this.value=this.value.slice(0,63)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box">
              </div>
              <div style="grid-column:1/-1">
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Site Address</label>
                <textarea id="site_address" rows="3" maxlength="127" oninput="if(this.value.length>127)this.value=this.value.slice(0,127)" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;resize:vertical"></textarea>
              </div>
              <div>
                <label style="font-weight:500;display:block;margin-bottom:6px;font-size:13px">Time Zone</label>
                <select id="site_timezone" style="width:100%;padding:8px 10px;border:1px solid #ccc;border-radius:4px;font-size:13px;box-sizing:border-box;background-color:#fff">
                  <option value="UTC0">UTC</option>
                  <option value="IST-5:30">India (IST, UTC+5:30)</option>
                  <option value="GST-4">Gulf (GST, UTC+4)</option>
                  <option value="MSK-3">Moscow (MSK, UTC+3)</option>
                  <option value="EET-2">Eastern Europe (EET, UTC+2)</option>
                  <option value="CET-1">Central Europe (CET, UTC+1)</option>
                  <option value="GMT0BST,M3.5.0/1,M10.5.0">UK (GMT/BST)</option>
                  <option value="WET0WEST,M3.5.0/1,M10.5.0">Portugal (WET)</option>
                  <option value="EST5EDT,M3.2.0,M11.1.0">US Eastern (EST/EDT)</option>
                  <option value="CST6CDT,M3.2.0,M11.1.0">US Central (CST/CDT)</option>
                  <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain (MST/MDT)</option>
                  <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific (PST/PDT)</option>
                  <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia Eastern (AEST)</option>
                  <option value="SGT-8">Singapore (SGT, UTC+8)</option>
                  <option value="CST-8">China (CST, UTC+8)</option>
                  <option value="JST-9">Japan (JST, UTC+9)</option>
                  <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand (NZST)</option>
                </select>
              </div>
            </div>
          </div>
          <div id="sys_status" style="display:none;margin-bottom:12px;padding:12px;border-radius:4px"></div>
          <div style="display:flex;gap:10px;margin-top:8px">
            <button class="btn primary" onclick="saveSystemConfig()" style="flex:1;padding:12px 24px;font-size:14px;font-weight:600">Save Site Details</button>
          </div>
        </div>
      </div>
      </div>
    </div>
  </div>

<script>
var tabLabels = {
  'dashboard': { title: 'Dashboard', subtitle: 'System Overview & Status' },
  'digital': { title: 'Digital Input Configuration'},
  'analog': { title: 'Analog Input Configuration'},
  'relays': { title: 'Digital Output Configuration', subtitle: 'Relay Output' },
  'phones': { title: 'Contact Configuration'},
  'network': { title: 'Network Configuration'},
  'sysconfig': { title: 'System Configuration'},
};

function eachNode(nodes, fn) {
  for (var i = 0; nodes && i < nodes.length; i++) fn(nodes[i], i);
}

function findClosestTabItem(el) {
  while (el && el !== document) {
    if (el.getAttribute && el.getAttribute('data-tab')) return el;
    el = el.parentNode;
  }
  return null;
}

function escapeHtml(value) {
  return String(value == null ? '' : value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function switchToTab(tabName) {
  var li = document.querySelector('[data-tab="' + tabName + '"]');
  if (!li) return;
  eachNode(document.querySelectorAll('.nav li'), function(n){ n.classList.remove('active'); });
  li.classList.add('active');
  eachNode(document.querySelectorAll('.tab'), function(t){ t.style.display='none'; });
  var el = document.getElementById(tabName); if(el) el.style.display='block';
  setStoredTab(tabName);
  try {
    if (window.history && window.history.replaceState) {
      window.history.replaceState(null, '', buildTabUrl(tabName));
    } else {
      window.location.hash = 'tab=' + encodeURIComponent(tabName);
    }
  } catch(e) {}
  var labels = tabLabels[tabName] || { title: tabName, subtitle: '' };
  document.getElementById('topbar_title').textContent = labels.title;
  document.getElementById('topbar_subtitle').textContent = labels.subtitle;
  if (tabName === 'digital') loadDIConfig();
  if (tabName === 'analog') loadAIConfig();
  if (tabName === 'relays') loadDOConfig();
  if (tabName === 'sysconfig') loadSystemConfig();
  if (tabName === 'phones' && typeof loadPhones === 'function') loadPhones();
  if (tabName === 'network' && typeof loadNetworkCfg === 'function') { loadNetworkCfg(); loadSIMConfig(); }
}

function getStoredTab() {
  try {
    return localStorage.getItem('selectedTab') || '';
  } catch(e) {
    return '';
  }
}

function setStoredTab(tabName) {
  try {
    localStorage.setItem('selectedTab', tabName);
  } catch(e) {}
}

function getQueryValue(name) {
  var query = window.location.search || '';
  if (query.charAt(0) === '?') query = query.substring(1);
  if (!query) return '';
  var parts = query.split('&');
  for (var i = 0; i < parts.length; i++) {
    var pair = parts[i].split('=');
    if (decodeURIComponent(pair[0] || '') === name) {
      return decodeURIComponent((pair[1] || '').replace(/\+/g, ' '));
    }
  }
  return '';
}

function removeQueryValue(search, name) {
  if (!search) return '';
  if (search.charAt(0) === '?') search = search.substring(1);
  if (!search) return '';
  var kept = [];
  var parts = search.split('&');
  for (var i = 0; i < parts.length; i++) {
    var pair = parts[i].split('=');
    if (decodeURIComponent(pair[0] || '') !== name) kept.push(parts[i]);
  }
  return kept.length ? '?' + kept.join('&') : '';
}

function buildTabUrl(tabName) {
  var path = window.location.pathname || '/';
  var search = removeQueryValue(window.location.search || '', 'tab');
  return path + search + '#tab=' + encodeURIComponent(tabName);
}

function getInitialTab() {
  var tab = 'dashboard';
  try {
    var queryTab = getQueryValue('tab');
    var hashTab = decodeURIComponent(window.location.hash.replace(/^#/, '').replace(/^tab=/, ''));
    var savedTab = getStoredTab();
    [hashTab, queryTab, savedTab].some(function(candidate) {
      if (candidate && document.getElementById(candidate)) {
        tab = candidate;
        return true;
      }
      return false;
    });
  } catch(e) {
    var saved = getStoredTab();
    if (saved && document.getElementById(saved)) tab = saved;
  }
  return tab;
}

document.getElementById('nav').addEventListener('click', function(e){
  var li = findClosestTabItem(e.target); if(!li) return;
  var tab = li.getAttribute('data-tab');
  switchToTab(tab);
});

function setStat(id, value){ var el=document.getElementById(id); if(el) el.textContent = value; }

function formatUptime(ms){
  var total = Math.floor((Number(ms) || 0) / 1000);
  var days = Math.floor(total / 86400);
  var hours = Math.floor((total % 86400) / 3600);
  var mins = Math.floor((total % 3600) / 60);
  var secs = total % 60;
  function pad(n){ return n < 10 ? '0' + n : '' + n; }
  return (days > 0 ? days + 'd ' : '') + pad(hours) + ':' + pad(mins) + ':' + pad(secs);
}

function configBadge(enabled){
  var style = enabled
    ? 'background:#d4edda;color:#155724'
    : 'background:#e5e7eb;color:#374151';
  return '<span style="' + style + ';padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600">' + (enabled ? 'Enabled' : 'Disabled') + '</span>';
}

function loadDashboard(){
  fetch('/api/dashboard').then(r=>{
    if(r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(d=>{
    setStat('uptime_counter', formatUptime(d.uptime_ms));
    setStat('s_serial', d.serial_number || 'Not Set');
    setStat('s_apip', d.ap_ip || '-');
    setStat('s_ethip', d.eth_ip || '-');
    setStat('s_dhcp', d.use_dhcp ? 'Enabled' : 'Disabled');
    setStat('s_static', d.static_ip || '-');
    setStat('s_signal', d.signal_strength || 'Unknown');
    setStat('s_siminfo', d.sim_info || 'Not Set');
    
    // Load system config for site/location
    fetch('/api/system-config').then(r=>r.json()).then(cfg=>{
      var site = cfg.site_name || '-';
      var addr = cfg.site_address ? ' (' + cfg.site_address + ')' : '';
      setStat('s_site', site + addr);
    }).catch(e=>console.log('site load failed', e));
    
    // Load IO status
    fetch('/api/io-status').then(r=>r.json()).then(io=>{
      // Digital Inputs
      var diHtml = '';
      if(io.digital_inputs && io.digital_inputs.length > 0) {
        io.digital_inputs.forEach(function(di, idx) {
          var status = di.in_alarm ? 'Alarm' : 'Normal';
          var badge = di.in_alarm ? 'style="background:#f8d7da;color:#721c24;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600"' : 'style="background:#d4edda;color:#155724;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600"';
          var type = di.normally_closed ? 'NC' : 'NO';
          var rowStyle = di.enabled ? '' : 'opacity:0.4;background:#f3f4f6;';
          diHtml += '<tr style="' + rowStyle + '"><td style="padding:10px;border-bottom:1px solid #e5e7eb"><strong>DI' + (idx+1) + '</strong></td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + escapeHtml(di.name || '-') + '</td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + type + '</td><td style="padding:10px;border-bottom:1px solid #e5e7eb"><span ' + badge + '>' + status + '</span></td></tr>';
        });
      }
      document.getElementById('di-table').innerHTML = diHtml || '<tr><td colspan="4" style="padding:10px;text-align:center;color:#999">No inputs configured</td></tr>';
      
      // Analog Inputs
      var aiHtml = '';
      if(io.analog_inputs && io.analog_inputs.length > 0) {
        io.analog_inputs.forEach(function(ai, idx) {
          var aiValue = Number(ai.value);
          var inAlarm = !!ai.in_alarm;
          var aiStatusBadge = inAlarm ? '<span style="background:#f8d7da;color:#721c24;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600">Alarm</span>' : '<span style="background:#d4edda;color:#155724;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600">Normal</span>';
          var aiRowStyle = ai.enabled ? '' : 'opacity:0.4;background:#f3f4f6;';
          aiHtml += '<tr style="' + aiRowStyle + '"><td style="padding:10px;border-bottom:1px solid #e5e7eb"><strong>AI' + (idx+1) + '</strong></td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + escapeHtml(ai.name || '-') + '</td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + (isNaN(aiValue) ? '-' : aiValue.toFixed(2)) + '</td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + aiStatusBadge + '</td></tr>';
        });
      }
      document.getElementById('ai-table').innerHTML = aiHtml || '<tr><td colspan="4" style="padding:10px;text-align:center;color:#999">No inputs configured</td></tr>';
      
      // Relay Outputs
      var relayHtml = '';
      if(io.relay_outputs && io.relay_outputs.length > 0) {
        io.relay_outputs.forEach(function(relay, idx) {
          var state = relay.state ? 'ON' : 'OFF';
          var badge = relay.state ? 'style="background:#d4edda;color:#155724;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600"' : 'style="background:#f8d7da;color:#721c24;padding:4px 8px;border-radius:4px;font-size:12px;font-weight:600"';
          var alarmSrcLabels = ['None','AI1','AI2','DI1','DI2','DI3','DI4'];
          var src = relay.alarm_source || 0;
          var linkText = (relay.alarm_control_enabled && src > 0) ? (alarmSrcLabels[src] || 'None') : 'None';
          var doRowStyle = relay.enabled ? '' : 'opacity:0.4;background:#f3f4f6;';
          relayHtml += '<tr style="' + doRowStyle + '"><td style="padding:10px;border-bottom:1px solid #e5e7eb"><strong>DO' + (idx+1) + '</strong></td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + escapeHtml(relay.name || '-') + '</td><td style="padding:10px;border-bottom:1px solid #e5e7eb"><span ' + badge + '>' + state + '</span></td><td style="padding:10px;border-bottom:1px solid #e5e7eb">' + escapeHtml(linkText) + '</td></tr>';
        });
      }
      document.getElementById('relay-table').innerHTML = relayHtml || '<tr><td colspan="4" style="padding:10px;text-align:center;color:#999">No outputs configured</td></tr>';

      // Update timestamp
      var tsEl = document.getElementById('io_timestamp');
      if (tsEl && io.timestamp) tsEl.textContent = io.timestamp;
    }).catch(e=>console.log('IO status load failed', e));
    
    // Clear network loading placeholder
    var nc = document.getElementById('netcfg'); if(nc) nc.textContent = '';
  }).catch(e=>{ if(e !== 'auth') console.log('dashboard load failed', e); });
}

var di_configs = [];

function loadDIConfig(){
  fetch('/api/digital-input-config').then(r=>{
    if(r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(configs=>{
    di_configs = configs;
    document.getElementById('di_loading').style.display = 'none';
    document.getElementById('di_form_container').style.display = 'block';
    var savedDIIndex = localStorage.getItem('selectedDIIndex') || '0';
    var selectedIndex = parseInt(savedDIIndex, 10);
    if (isNaN(selectedIndex) || selectedIndex < 0 || selectedIndex >= configs.length) selectedIndex = 0;
    document.getElementById('di_selector').value = selectedIndex;
    switchDI(selectedIndex);
  }).catch(e=>{ if(e !== 'auth') console.log('di config load failed', e); });
}

function switchDI(index){
  if(!di_configs || di_configs.length === 0) return;
  index = parseInt(index, 10);
  if (isNaN(index) || index < 0 || index >= di_configs.length) index = 0;
  var cfg = di_configs[index];
  if (!cfg) return;
  document.getElementById('di_enabled').checked = cfg.enabled;
  document.getElementById('di_name').value = cfg.name || '';
  document.getElementById('di_type').value = cfg.normally_closed ? '1' : '0';
  document.getElementById('di_tta').value = cfg.tta_ms;
  document.getElementById('di_ttr').value = cfg.ttr_ms;
  document.getElementById('di_alarm_sms').checked = cfg.alarm_sms_enabled;
  document.getElementById('di_return_sms').checked = cfg.return_sms_enabled;
  document.getElementById('di_alarm_msg').value = cfg.alarm_message || '';
  document.getElementById('di_return_msg').value = cfg.return_message || '';
  
  // Set selected recipients based on bitmask
  var di_sel = document.getElementById('di_recipients_select');
  if (di_sel) {
  var selectedIndices =
  decodeBitmask(cfg.selected_contacts || 0);
  eachNode(di_sel.querySelectorAll('.recipient-checkbox'), function(cb) {
      cb.checked =
        selectedIndices.indexOf(
          parseInt(cb.value, 10)
        ) >= 0;
    });
}
  
  window.current_di_index = index;
  localStorage.setItem('selectedDIIndex', index);
  updateDIFieldsState();
}

function updateDIFieldsState(){
  var enabled = document.getElementById('di_enabled').checked;
  var fields = ['di_name','di_type','di_tta','di_ttr','di_alarm_sms','di_return_sms','di_alarm_msg','di_return_msg'];
  fields.forEach(function(id){ var el=document.getElementById(id); if(el) el.disabled=!enabled; });
  var di_sel = document.getElementById('di_recipients_select');
  if (di_sel) eachNode(di_sel.querySelectorAll('input'), function(cb){ cb.disabled=!enabled; });
  var saveBtn = document.querySelector('#digital .btn.primary');
  if (saveBtn) saveBtn.disabled = false; // Save is always allowed (to save enabled=false)
}

function saveDIConfig(){
  var index = window.current_di_index || 0;
  var form_data = new FormData();
  form_data.append('index', index);
  form_data.append('enabled', document.getElementById('di_enabled').checked ? '1' : '0');
  form_data.append('name', document.getElementById('di_name').value.trim());
  form_data.append('normally_closed', document.getElementById('di_type').value === '1' ? '1' : '0');
  form_data.append('tta_ms', document.getElementById('di_tta').value);
  form_data.append('ttr_ms', document.getElementById('di_ttr').value);
  form_data.append('alarm_sms_enabled', document.getElementById('di_alarm_sms').checked ? '1' : '0');
  form_data.append('return_sms_enabled', document.getElementById('di_return_sms').checked ? '1' : '0');
  form_data.append('alarm_message', document.getElementById('di_alarm_msg').value.trim());
  form_data.append('return_message', document.getElementById('di_return_msg').value.trim());
  
  // Encode selected recipients into bitmask
  var di_sel = document.getElementById('di_recipients_select');
var selectedIndices = [];
if (di_sel) {
  eachNode(di_sel.querySelectorAll('.recipient-checkbox:checked'), function(cb) {
    selectedIndices.push(cb.value);
  });
}
var bitmask = encodeBitmask(selectedIndices);
form_data.append('selected_contacts', bitmask);
  
  var status_el = document.getElementById('di_status');
  fetch('/api/digital-input-config', { method:'POST', body:form_data })
    .then(r=>r.json())
    .then(d=>{ 
      if(d.success) {
        di_configs[index] = {enabled:form_data.get('enabled')==='1',name:form_data.get('name'),normally_closed:form_data.get('normally_closed')==='1',tta_ms:parseInt(form_data.get('tta_ms')),ttr_ms:parseInt(form_data.get('ttr_ms')),alarm_sms_enabled:form_data.get('alarm_sms_enabled')==='1',return_sms_enabled:form_data.get('return_sms_enabled')==='1',alarm_message:form_data.get('alarm_message'),return_message:form_data.get('return_message'),selected_contacts:bitmask};
        status_el.textContent = 'DI' + (index+1) + ' configuration saved successfully!';
        status_el.style.color = 'green';
      } else {
        status_el.textContent = 'Error: ' + (d.error || 'Save failed');
        status_el.style.color = 'red';
      }
      status_el.style.display = 'block';
      setTimeout(function(){ status_el.style.display = 'none'; }, 4000);
    })
    .catch(e=>{
      status_el.textContent = 'Error: ' + e.message;
      status_el.style.color = 'red';
      status_el.style.display = 'block';
    });
}

var ai_configs = [];
var do_configs = [];
var event_recipients = [];

// Helper: Convert bitmask to selected indices array
function decodeBitmask(bitmask) {
  var selected = [];
  for (var i = 0; i < 5; i++) {
    if ((bitmask & (1 << i)) !== 0) selected.push(i);
  }
  return selected;
}

// Helper: Convert selected indices array to bitmask
function encodeBitmask(selectedIndices) {
  var bitmask = 0;
  for (var i = 0; i < selectedIndices.length; i++) {
    var idx = parseInt(selectedIndices[i]);
    if (!isNaN(idx) && idx >= 0 && idx < 5) {
      bitmask |= (1 << idx);
    }
  }
  return bitmask;
}

// Load recipients and populate all three dropdowns
function loadRecipients(){
  return fetch('/api/contacts/recipients').then(r=>{
    if(r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(d=>{
    if (d.recipients && Array.isArray(d.recipients)) {
      event_recipients = d.recipients;
      var di_sel = document.getElementById('di_recipients_select');
      var ai_sel = document.getElementById('ai_recipients_select');
      var relay_sel = document.getElementById('relay_recipients_select');
      
      // Clear existing options and rebuild
      [di_sel, ai_sel, relay_sel].forEach(function(sel) {
        if (!sel) return;
        sel.innerHTML = '';
        if (d.recipients.length === 0) {
        sel.innerHTML =
        '<div style="color:#999">(no recipients configured)</div>';
        } else {
          d.recipients.forEach(function(r, idx) {
          if (!r.enabled) return;
          var label = document.createElement('label');
          label.style.display = 'block';
          label.style.marginBottom = '6px';
          label.style.cursor = 'pointer';

          var cb = document.createElement('input');
          cb.type = 'checkbox';
          cb.className = 'recipient-checkbox';
          cb.value = idx;
          cb.style.marginRight = '8px';
          label.appendChild(cb);
          label.appendChild(document.createTextNode((r.name || '-') + ' (' + (r.number || '-') + ')'));
          sel.appendChild(label);
          });
        }
      });
      refreshRecipientSelections();
    }
  }).catch(e=>{ if(e !== 'auth') console.log('recipients load failed', e); });
}

function refreshRecipientSelections(){
  if (di_configs && di_configs.length > 0 && typeof switchDI === 'function') {
    var diIndex = parseInt(window.current_di_index !== undefined ? window.current_di_index : ((document.getElementById('di_selector') || {}).value || 0));
    if (diIndex >= 0 && diIndex < di_configs.length) switchDI(diIndex);
  }
  if (ai_configs && ai_configs.length > 0 && typeof switchAI === 'function') {
    var aiIndex = parseInt(window.current_ai_index !== undefined ? window.current_ai_index : ((document.getElementById('ai_selector') || {}).value || 0));
    if (aiIndex >= 0 && aiIndex < ai_configs.length) switchAI(aiIndex);
  }
  // DO recipients are refreshed by loadDOConfig chaining; only refresh here if do_configs already loaded
  if (do_configs && do_configs.length > 0 && typeof switchDO === 'function') {
    var doIndex = parseInt(window.current_do_index !== undefined ? window.current_do_index : ((document.getElementById('do_selector') || {}).value || 0));
    if (doIndex >= 0 && doIndex < do_configs.length) switchDO(doIndex);
  }
}

function getAIUnitOptions(){
  var sel = document.getElementById('ai_unit_select');
  var vals = [];
  if (!sel) return vals;
  for (var i = 0; i < sel.options.length; i++) {
    if (sel.options[i].value !== '__custom__') vals.push(sel.options[i].value);
  }
  return vals;
}

function toggleAIUnitCustom(focusCustom){
  var sel = document.getElementById('ai_unit_select');
  var custom = document.getElementById('ai_unit_custom');
  if (!sel || !custom) return;
  custom.style.display = sel.value === '__custom__' ? 'block' : 'none';
  if (focusCustom && sel.value === '__custom__') custom.focus();
}

function setAIEngineeringUnit(unit){
  var sel = document.getElementById('ai_unit_select');
  var custom = document.getElementById('ai_unit_custom');
  if (!sel || !custom) return;
  var value = unit || 'Liters';
  var options = getAIUnitOptions();
  if (options.indexOf(value) >= 0) {
    sel.value = value;
    custom.value = '';
  } else {
    sel.value = '__custom__';
    custom.value = value;
  }
  toggleAIUnitCustom(false);
}

function getAIEngineeringUnit(){
  var sel = document.getElementById('ai_unit_select');
  var custom = document.getElementById('ai_unit_custom');
  if (!sel) return '';
  if (sel.value === '__custom__') return custom ? custom.value.trim() : '';
  return sel.value;
}

function loadAIConfig(){
  fetch('/api/analog-input-config').then(r=>{
    if(r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(configs=>{
    ai_configs = configs;
    document.getElementById('ai_loading').style.display = 'none';
    document.getElementById('ai_form_container').style.display = 'block';
    var savedAIIndex = localStorage.getItem('selectedAIIndex') || '0';
    var selectedIndex = parseInt(savedAIIndex, 10);
    if (isNaN(selectedIndex) || selectedIndex < 0 || selectedIndex >= configs.length) selectedIndex = 0;
    document.getElementById('ai_selector').value = selectedIndex;
    switchAI(selectedIndex);
  }).catch(e=>{ if(e !== 'auth') console.log('ai config load failed', e); });
}

function switchAI(index){
  if(!ai_configs || ai_configs.length === 0) return;
  index = parseInt(index, 10);
  if (isNaN(index) || index < 0 || index >= ai_configs.length) index = 0;
  var cfg = ai_configs[index];
  if (!cfg) return;
  document.getElementById('ai_enabled').checked = cfg.enabled;
  document.getElementById('ai_name').value = cfg.name || '';
  setAIEngineeringUnit(cfg.engineering_unit || 'Liters');
  document.getElementById('ai_scale_low').value = cfg.scale_low || 0;
  document.getElementById('ai_scale_high').value = cfg.scale_high || 100;
  
  // Set alarm type radio button
  eachNode(document.querySelectorAll('input[name="ai_alarm_type"]'), function(r) { r.checked = false; });
  var typeRadio = document.getElementById('ai_type_' + ['high', 'low', 'inband', 'outband'][cfg.alarm_type || 0]);
  if (typeRadio) typeRadio.checked = true;
  updateAIThresholdHints();
  
  document.getElementById('ai_set_point').value = cfg.set_point || 0;
  document.getElementById('ai_reset_point').value = cfg.reset_point || 0;
  document.getElementById('ai_tta').value = cfg.tta_ms || 1;
  document.getElementById('ai_ttr').value = cfg.ttr_ms || 1;
  document.getElementById('ai_alarm_sms').checked = cfg.alarm_sms_enabled;
  document.getElementById('ai_return_sms').checked = cfg.return_sms_enabled;
  document.getElementById('ai_alarm_msg').value = cfg.alarm_message || '';
  document.getElementById('ai_return_msg').value = cfg.return_message || '';
  
  // Set selected recipients based on bitmask
  var ai_sel = document.getElementById('ai_recipients_select');

if (ai_sel) {
  var selectedIndices =
    decodeBitmask(cfg.selected_contacts || 0);

  eachNode(ai_sel.querySelectorAll('.recipient-checkbox'), function(cb) {
      cb.checked =
        selectedIndices.indexOf(
          parseInt(cb.value, 10)
        ) >= 0;
    });
}
  
  window.current_ai_index = index;
  localStorage.setItem('selectedAIIndex', index);
  updateAIFieldsState();
}

function getSelectedAlarmType(){
  var t = 0;
  eachNode(document.querySelectorAll('input[name="ai_alarm_type"]'), function(r){ if(r.checked) t = parseInt(r.value,10); });
  return t;
}

function updateAIThresholdHints(){
  var t = getSelectedAlarmType();
  var sh = document.getElementById('ai_set_point_hint');
  var rh = document.getElementById('ai_reset_point_hint');
  if (!sh || !rh) return;
  if (t === 0) { // High
    sh.textContent = 'Alarm triggers when value rises ABOVE this (must be > Reset Point)';
    rh.textContent = 'Alarm clears when value falls BELOW this (must be < Set Point)';
  } else if (t === 1) { // Low
    sh.textContent = 'Alarm triggers when value falls BELOW this (must be < Reset Point)';
    rh.textContent = 'Alarm clears when value rises ABOVE this (must be > Set Point)';
  } else if (t === 2) { // In-Band
    sh.textContent = 'Lower band boundary';
    rh.textContent = 'Upper band boundary (must be > Set Point)';
  } else { // Out-of-Band
    sh.textContent = 'Lower band boundary';
    rh.textContent = 'Upper band boundary (must be > Set Point)';
  }
}

function updateAIFieldsState(){
  var enabled = document.getElementById('ai_enabled').checked;
  var fields = ['ai_name','ai_unit_select','ai_unit_custom','ai_scale_low','ai_scale_high','ai_set_point','ai_reset_point','ai_tta','ai_ttr','ai_alarm_sms','ai_return_sms','ai_alarm_msg','ai_return_msg'];
  fields.forEach(function(id){ var el=document.getElementById(id); if(el) el.disabled=!enabled; });
  eachNode(document.querySelectorAll('input[name="ai_alarm_type"]'), function(r){ r.disabled=!enabled; });
  var ai_sel = document.getElementById('ai_recipients_select');
  if (ai_sel) eachNode(ai_sel.querySelectorAll('input'), function(cb){ cb.disabled=!enabled; });
}

function saveAIConfig(){
  var index = window.current_ai_index || 0;
  var form_data = new FormData();
  form_data.append('index', index);
  form_data.append('enabled', document.getElementById('ai_enabled').checked ? '1' : '0');
  form_data.append('name', document.getElementById('ai_name').value.trim());
  form_data.append('engineering_unit', getAIEngineeringUnit());
  form_data.append('scale_low', document.getElementById('ai_scale_low').value);
  form_data.append('scale_high', document.getElementById('ai_scale_high').value);
  
  // Get alarm type from radio buttons
  var alarmType = 0;
  eachNode(document.querySelectorAll('input[name="ai_alarm_type"]'), function(r) {
    if (r.checked) alarmType = parseInt(r.value, 10);
  });
  form_data.append('alarm_type', alarmType);
  
  var setPoint = parseFloat(document.getElementById('ai_set_point').value);
  var resetPoint = parseFloat(document.getElementById('ai_reset_point').value);
  var status_el = document.getElementById('ai_status');
  if (alarmType === 0 && setPoint <= resetPoint) {
    status_el.textContent = 'Error: For High Alarm, Set Point must be greater than Reset Point.';
    status_el.style.color = 'red';
    status_el.style.display = 'block';
    return;
  }
  if (alarmType === 1 && setPoint >= resetPoint) {
    status_el.textContent = 'Error: For Low Alarm, Set Point must be less than Reset Point.';
    status_el.style.color = 'red';
    status_el.style.display = 'block';
    return;
  }
  if ((alarmType === 2 || alarmType === 3) && setPoint >= resetPoint) {
    status_el.textContent = 'Error: Reset Point (upper band) must be greater than Set Point (lower band).';
    status_el.style.color = 'red';
    status_el.style.display = 'block';
    return;
  }
  form_data.append('set_point', setPoint);
  form_data.append('reset_point', resetPoint);
  form_data.append('tta_ms', document.getElementById('ai_tta').value);
  form_data.append('ttr_ms', document.getElementById('ai_ttr').value);
  form_data.append('alarm_sms_enabled', document.getElementById('ai_alarm_sms').checked ? '1' : '0');
  form_data.append('return_sms_enabled', document.getElementById('ai_return_sms').checked ? '1' : '0');
  form_data.append('alarm_message', document.getElementById('ai_alarm_msg').value.trim());
  form_data.append('return_message', document.getElementById('ai_return_msg').value.trim());
  
  // Encode selected recipients into bitmask
  var ai_sel = document.getElementById('ai_recipients_select');
var selectedIndices = [];
if (ai_sel) {
  eachNode(ai_sel.querySelectorAll('.recipient-checkbox:checked'), function(cb) {
    selectedIndices.push(cb.value);
  });
}
var bitmask = encodeBitmask(selectedIndices);
form_data.append('selected_contacts', bitmask);
  
  var status_el = document.getElementById('ai_status');
  fetch('/api/analog-input-config', { method:'POST', body:form_data })
    .then(r=>r.json())
    .then(d=>{ 
      if(d.success) {
        ai_configs[index] = {enabled:form_data.get('enabled')==='1',name:form_data.get('name'),engineering_unit:form_data.get('engineering_unit'),scale_low:parseFloat(form_data.get('scale_low')),scale_high:parseFloat(form_data.get('scale_high')),alarm_type:alarmType,set_point:parseFloat(form_data.get('set_point')),reset_point:parseFloat(form_data.get('reset_point')),tta_ms:parseInt(form_data.get('tta_ms')),ttr_ms:parseInt(form_data.get('ttr_ms')),alarm_sms_enabled:form_data.get('alarm_sms_enabled')==='1',return_sms_enabled:form_data.get('return_sms_enabled')==='1',alarm_message:form_data.get('alarm_message'),return_message:form_data.get('return_message'),selected_contacts:bitmask};
        status_el.textContent = 'AI' + (index+1) + ' configuration saved successfully!';
        status_el.style.color = 'green';
      } else {
        status_el.textContent = 'Error: ' + (d.error || 'Save failed');
        status_el.style.color = 'red';
      }
      status_el.style.display = 'block';
      setTimeout(function(){ status_el.style.display = 'none'; }, 4000);
    })
    .catch(e=>{
      status_el.textContent = 'Error: ' + e.message;
      status_el.style.color = 'red';
      status_el.style.display = 'block';
    });
}

function loadDOConfig(){
  fetch('/api/relay-config').then(r=>{
    if(r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(d=>{
    do_configs = d;
    return loadRecipients();
  }).then(function(){
    var saved_idx = parseInt(localStorage.getItem('selectedDOIndex'), 10);
    if (isNaN(saved_idx) || saved_idx < 0 || saved_idx >= do_configs.length) saved_idx = 0;
    document.getElementById('do_selector').value = saved_idx;
    switchDO(saved_idx);
  }).catch(function(e){
    if(e === 'auth') return;
    console.error('Error loading DO config:', e);
  });
}

function switchDO(index){
  if(!do_configs || do_configs.length === 0) return;
  index = parseInt(index, 10);
  if (isNaN(index) || index < 0 || index >= do_configs.length) index = 0;
  var cfg = do_configs[index];
  if (!cfg) return;
  document.getElementById('do_enabled').checked = cfg.enabled;
  document.getElementById('do_name').value = cfg.name || '';
  document.getElementById('do_powerup').value = cfg.default_power_up_state ? '1' : '0';
  document.getElementById('do_sms_control').checked = cfg.sms_control_enabled;
  document.getElementById('do_alarm_control').checked = cfg.alarm_control_enabled;
  document.getElementById('do_alarm_source').value = cfg.alarm_source || 0;
  
  // Set selected recipients based on bitmask
  var relay_sel = document.getElementById('relay_recipients_select');

if (relay_sel) {

  var selectedIndices =
    decodeBitmask(cfg.selected_contacts || 0);

  eachNode(relay_sel.querySelectorAll('.recipient-checkbox'), function(cb) {

      cb.checked =
        selectedIndices.indexOf(
          parseInt(cb.value, 10)
        ) >= 0;

    });
}
  
  window.current_do_index = index;
  localStorage.setItem('selectedDOIndex', index);
  updateDOFieldsState();
}

function updateDOFieldsState(){
  var enabled = document.getElementById('do_enabled').checked;
  var alarmCtrl = enabled && document.getElementById('do_alarm_control').checked;
  var fields = ['do_name','do_powerup','do_sms_control','do_alarm_control'];
  fields.forEach(function(id){ var el=document.getElementById(id); if(el) el.disabled=!enabled; });
  var alarmSrcEl = document.getElementById('do_alarm_source');
  if (alarmSrcEl) alarmSrcEl.disabled = !alarmCtrl;
  var relay_sel = document.getElementById('relay_recipients_select');
  if (relay_sel) eachNode(relay_sel.querySelectorAll('input'), function(cb){ cb.disabled=!enabled; });
}

function saveDOConfig(){
  var index = window.current_do_index || 0;
  var form_data = new FormData();
  form_data.append('index', index);
  form_data.append('enabled', document.getElementById('do_enabled').checked ? '1' : '0');
  form_data.append('name', document.getElementById('do_name').value.trim());
  form_data.append('default_power_up_state', document.getElementById('do_powerup').value);
  form_data.append('sms_control_enabled', document.getElementById('do_sms_control').checked ? '1' : '0');
  form_data.append('alarm_control_enabled', document.getElementById('do_alarm_control').checked ? '1' : '0');
  form_data.append('alarm_source', document.getElementById('do_alarm_source').value);
  var relay_sel = document.getElementById('relay_recipients_select');
  var selectedIndices = [];
  if (relay_sel) {
    eachNode(relay_sel.querySelectorAll('.recipient-checkbox:checked'), function(cb) {
      selectedIndices.push(cb.value);
    });
  }
  var bitmask = encodeBitmask(selectedIndices);
  form_data.append('selected_contacts', bitmask);
  var status_el = document.getElementById('do_status');
  fetch('/api/relay-config', { method:'POST', body:form_data })
    .then(function(r){ return r.json(); })
    .then(function(d){
      if(d.success) {
        do_configs[index] = {enabled:form_data.get('enabled')==='1',name:form_data.get('name'),default_power_up_state:form_data.get('default_power_up_state')==='1',sms_control_enabled:form_data.get('sms_control_enabled')==='1',alarm_control_enabled:form_data.get('alarm_control_enabled')==='1',alarm_source:parseInt(form_data.get('alarm_source')),selected_contacts:bitmask};
        status_el.textContent = 'DO' + (index+1) + ' configuration saved successfully!';
        status_el.style.color = 'green';
      } else {
        status_el.textContent = 'Error: ' + (d.error || 'Save failed');
        status_el.style.color = 'red';
      }
      status_el.style.display = 'block';
      setTimeout(function(){ status_el.style.display = 'none'; }, 4000);
    })
    .catch(function(e){
      status_el.textContent = 'Error: ' + e.message;
      status_el.style.color = 'red';
      status_el.style.display = 'block';
    });
}


function showStatus(msg, ok) {
  showSmallStatus('sys_status', msg, ok);
}

function loadSystemConfig(){
  fetch('/api/system-config').then(r=>{
    if (r.status === 401) { window.location = '/login'; return Promise.reject('auth'); }
    return r.json();
  }).then(cfg=>{
    var sn = document.getElementById('site_name'); if (sn) sn.value = cfg.site_name || '';
    var sa = document.getElementById('site_address'); if (sa) sa.value = cfg.site_address || '';
    var tz = document.getElementById('site_timezone');
    if (tz && cfg.timezone) tz.value = cfg.timezone;
  }).catch(e=>{ if (e !== 'auth') console.log('load sysconfig failed', e); });
}

function saveSystemConfig(){
  var p = new URLSearchParams();
  p.append('site_name', document.getElementById('site_name').value.trim());
  p.append('site_address', document.getElementById('site_address').value.trim());
  var tzEl = document.getElementById('site_timezone');
  var tzVal = tzEl ? tzEl.value : '';
  p.append('timezone', tzVal);
  fetch('/api/system-config', { method:'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{ if (d.success) showStatus('Saved', true); else showStatus(d.error||'Save failed', false); })
    .catch(e=>showStatus('Save failed: '+e.message, false));
}

// Restore the last active tab after reloads.
try {
  switchToTab(getInitialTab());
} catch(e) {
  console.log('tab restore failed', e);
}

loadDashboard();
setInterval(loadDashboard, 5000);
loadRecipients();
loadNetworkCfg();
loadSIMConfig();
</script>
<script>
var MAX_CONTACTS = 5;

function loadPhones(){
  fetch('/api/contacts/recipients').then(r=>{ if(r.status===401){window.location='/login';return Promise.reject('auth')} return r.json(); }).then(d=>{
    if (d.recipients && Array.isArray(d.recipients)) renderContactList('rec_list', d.recipients);
  }).catch(e=>{ if(e!=='auth') console.log('recipients load failed', e); });
}

function saveContacts(){
  var recEls = document.querySelectorAll('#rec_list .contact-row');
  var recArr = [];
  eachNode(recEls, function(el){
    var name = (el.querySelector('.c_name')||{}).value || '';
    var num = (el.querySelector('.c_number')||{}).value || '';
    var en = !!(el.querySelector('.c_enabled')||{}).checked;
    recArr.push({enabled:en, name:name, number:num});
  });

  var phoneValid = function(n){ if(!n) return true; var m = n.match(/^\+?[0-9]{10,15}$/); return !!m; };
  for (var i=0;i<recArr.length;i++) if(!phoneValid(recArr[i].number)){ showSmallStatus('phones_status','Invalid phone number',false); return; }

  if (recArr.length > MAX_CONTACTS) { showSmallStatus('phones_status','Max ' + MAX_CONTACTS + ' contacts allowed', false); return; }

  var p = new URLSearchParams(); p.append('contacts', JSON.stringify(recArr));
  fetch('/api/contacts/recipients', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{ if(d.success) { showSmallStatus('phones_status','Contacts saved',true); loadPhones(); loadRecipients(); } else showSmallStatus('phones_status',d.error||'Save failed',false); })
    .catch(e=>showSmallStatus('phones_status','Save failed',false));
}

function renderContactList(containerId, arr){
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

function addRecContact(){ var el=document.getElementById('rec_list'); if(!el) return; var cnt = el.querySelectorAll('.contact-row').length; if(cnt >= MAX_CONTACTS){ showSmallStatus('phones_status','Max ' + MAX_CONTACTS + ' contacts allowed', false); return; } renderContactListAppend(el); }
function renderContactListAppend(el){ var item={enabled:true,name:'',number:''}; var row = document.createElement('div'); row.className = 'contact-row'; row.style.display='flex'; row.style.gap='8px'; row.style.margin='6px 0'; var chk=document.createElement('input'); chk.type='checkbox'; chk.className='c_enabled'; chk.checked=true; var name=document.createElement('input'); name.className='c_name input'; name.placeholder='Name'; var num=document.createElement('input'); num.className='c_number input'; num.placeholder='+1234567890'; num.oninput=function(){ this.value=this.value.replace(/[^0-9+]/g,''); if(this.value.indexOf('+')>0) this.value=this.value.replace(/\+/g,''); if(this.value.length>16) this.value=this.value.slice(0,16); }; var del=document.createElement('button'); del.className='btn'; del.textContent='Remove'; del.onclick=function(){ row.remove(); }; row.appendChild(chk); row.appendChild(name); row.appendChild(num); row.appendChild(del); el.appendChild(row); }

if (document.getElementById('phones') && document.getElementById('phones').style.display !== 'none') {
  loadPhones();
}

function loadNetworkCfg(){
  fetch('/api/gateway-settings').then(r=>{ if(r.status===401){window.location='/login';return Promise.reject('auth')} return r.json(); }).then(d=>{
    var dh = !!d.use_dhcp;
    var useEl = document.getElementById('net_useDhcp'); if (useEl) useEl.checked = dh;
    var si = document.getElementById('net_staticIp'); if (si) si.value = d.static_ip || '';
    var sm = document.getElementById('net_subnetMask'); if (sm) sm.value = d.subnet_mask || '';
    var gw = document.getElementById('net_gatewayIp'); if (gw) gw.value = d.gateway_ip || '';
    // ensure static fields are enabled/disabled according to DHCP
    try { toggleNetworkStaticFields(); } catch(e){}
  }).catch(e=>{ if(e!=='auth') console.log('network load failed', e); });
}

function saveNetworkCfg(){
  var useDhcp = document.getElementById('net_useDhcp').checked;
  var staticIp = (document.getElementById('net_staticIp')||{}).value || '';
  var subnet = (document.getElementById('net_subnetMask')||{}).value || '';
  var gateway = (document.getElementById('net_gatewayIp')||{}).value || '';

  if (!useDhcp) {
    if (!isValidIPv4(staticIp) || !isValidIPv4(subnet) || !isValidIPv4(gateway)) {
      showSmallStatus('net_status','Invalid IP address format', false);
      return;
    }
  }
  var p = new URLSearchParams();
  p.append('use_dhcp', useDhcp ? '1' : '0');
  p.append('static_ip', staticIp.trim());
  p.append('subnet_mask', subnet.trim());
  p.append('gateway_ip', gateway.trim());

  fetch('/api/gateway-settings', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{
      if(d.success) {
        showSmallStatus('net_status','Saved. Reboot to apply',true);
        loadNetworkCfg();
      } else {
        showSmallStatus('net_status',d.error||'Save failed',false);
      }
    })
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

// Load SIM Configuration
function loadSIMConfig() {
  fetch('/api/sim-config').then(r=>r.json()).then(cfg=>{
    if (cfg.service_provider) document.getElementById('sim_provider').value = cfg.service_provider;
    if (cfg.phone_number) document.getElementById('sim_phone').value = cfg.phone_number;
    if (cfg.relay_pin) document.getElementById('sim_relay_pin').value = cfg.relay_pin;
  }).catch(e=>console.log('SIM config load failed', e));
}

// Save SIM Configuration
function saveSIMConfig(){
  var provider = (document.getElementById('sim_provider')||{}).value || '';
  var phone = (document.getElementById('sim_phone')||{}).value || '';
  var relayPin = (document.getElementById('sim_relay_pin')||{}).value || '';

  var p = new URLSearchParams();
  p.append('service_provider', provider.trim());
  p.append('phone_number', phone.trim());
  p.append('relay_pin', relayPin.trim());

  fetch('/api/sim-config', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() })
    .then(r=>r.json()).then(d=>{
      if(d.success) {
        showSmallStatus('sim_status','SIM configuration saved',true);
        loadSIMConfig();
      } else {
        showSmallStatus('sim_status',d.error||'Save failed',false);
      }
    })
    .catch(e=>showSmallStatus('sim_status','Save failed',false));
}

// Helper to show status messages
function showSmallStatus(elementId, message, isSuccess){
  var el = document.getElementById(elementId);
  if(!el) return;
  el.style.display = 'block';
  el.style.background = isSuccess ? '#d4edda' : '#f8d7da';
  el.style.color = isSuccess ? '#155724' : '#721c24';
  el.style.padding = '12px';
  el.style.borderRadius = '4px';
  el.style.borderLeft = '4px solid ' + (isSuccess ? '#28a745' : '#dc3545');
  el.textContent = message;
  setTimeout(function(){ el.style.display = 'none'; }, 4000);
}
</script>
</body>
</html>

)rawliteral";
}

static void sendHtmlPage(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "text/html",
    [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      const char *page = htmlPage();
      const size_t len = strlen(page);
      if (index >= len) return 0;
      size_t chunkLen = len - index;
      if (chunkLen > maxLen) chunkLen = maxLen;
      memcpy(buffer, page + index, chunkLen);
      return chunkLen;
    });
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}
