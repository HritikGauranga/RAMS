#include "AP.h"
#include "Shared.h"
#include <ESPAsyncWebServer.h>
#include <ETH.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>

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
        <input id="serial" name="serial" type="text" placeholder="e.g. OMS0001" required maxlength="32" pattern="[A-Za-z0-9_-]+">
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
    document.getElementById('tcpPort').value=c.tcp_port;
  }).catch(e=>status('Load failed: '+e.message,false));
}
function saveCfg(){
  var p=new URLSearchParams();
  p.append('use_dhcp',document.getElementById('useDhcp').checked?'1':'0');
  p.append('static_ip',document.getElementById('staticIp').value.trim());
  p.append('subnet_mask',document.getElementById('subnetMask').value.trim());
  p.append('gateway_ip',document.getElementById('gatewayIp').value.trim());
  p.append('tcp_port',document.getElementById('tcpPort').value);
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

  server.on("/gateway-config", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    request->send(200, "text/html", gatewaySettingsPage());
  });

  server.on("/gateway-config/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      sendRedirect(request, "/login");
      return;
    }
    request->send(200, "text/html", gatewaySettingsPage());
  });

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
    body += "\"tcp_port\":" + String(s.tcpPort) + ",";
    body += "\"baud_rate\":" + String((unsigned long)s.baudRate) + ",";
    body += "\"data_bits\":" + String(s.dataBits) + ",";
    body += "\"parity\":\"" + String(s.parity) + "\",";
    body += "\"stop_bits\":" + String(s.stopBits);
    body += "}";
    request->send(200, "application/json", body);
  });

  server.on("/api/gateway-settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    auto val = [&](const char *k) -> String {
      return request->hasParam(k, true) ? request->getParam(k, true)->value() : "";
    };
    GatewaySettings s = {};
    Shared_getGatewaySettings(s);

    s.useDhcp = (val("use_dhcp") == "1");
    String tcpPortText = val("tcp_port");
    if (tcpPortText.startsWith("-")) {
      request->send(400, "application/json", "{\"error\":\"Negative values are not allowed\"}");
      return;
    }
    long tcpPortLong = tcpPortText.toInt();
    if (tcpPortLong < 1 || tcpPortLong > 65535) {
      request->send(400, "application/json", "{\"error\":\"TCP Port must be 1-65535\"}");
      return;
    }
    s.tcpPort = (uint16_t)tcpPortLong;

    if (!parseIPFromText(val("static_ip"), s.staticIp) ||
        !parseIPFromText(val("subnet_mask"), s.subnetMask) ||
        !parseIPFromText(val("gateway_ip"), s.gatewayIp)) {
      request->send(400, "application/json", "{\"error\":\"Invalid IP format\"}");
      return;
    }

    if (!Shared_saveGatewaySettings(s)) {
      request->send(500, "application/json", "{\"error\":\"Save failed\"}");
      return;
    }
    request->send(200, "application/json", "{\"success\":true}");
  });

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
    body += "\"tcp_port\":" + String(s.tcpPort);
    body += "}";
    request->send(200, "application/json", body);
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
  body { margin:0; font-family: Arial, sans-serif; height:100vh; display:flex; }
  .sidebar { width:220px; background:#f4f6f8; border-right:1px solid #e0e0e0; padding:12px; box-sizing:border-box; }
  .brand { font-weight:700; margin-bottom:12px; }
  .nav { list-style:none; padding:0; margin:0; }
  .nav li { padding:10px 8px; cursor:pointer; border-radius:6px; color:#222; }
  .nav li.active { background:#e8f0ff; color:#0b57a4; font-weight:600; }
  .content { flex:1; padding:18px; box-sizing:border-box; overflow:auto; }
  .topbar { display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; }
  .tab { display:none; }
  .tab.active { display:block; }
  .btn { padding:8px 12px; border-radius:6px; border:0; cursor:pointer; }
  .btn.primary { background:#1565c0; color:white; }
  .small { font-size:13px; color:#666; }
</style>
</head>
<body>
  <div class="sidebar">
    <div class="brand">RAMS</div>
    <ul class="nav" id="nav">
      <li data-tab="dashboard" class="active">Dashboard</li>
      <li data-tab="digital">Digital Inputs</li>
      <li data-tab="analog">Analog Inputs</li>
      <li data-tab="relays">Relay Outputs</li>
      <li data-tab="alarms">Alarm Management</li>
      <li data-tab="phones">Phone Numbers</li>
      <li data-tab="network">Network Configuration</li>
      <li data-tab="events">Event Logs</li>
      <li data-tab="diag">Diagnostics</li>
    </ul>
  </div>
  <div class="content">
    <div class="topbar">
      <div><strong>RAMS</strong> <span class="small">Remote Alarm Monitoring System</span></div>
      <div>
        <button class="btn" onclick="location.href='/logout'">Logout</button>
        <button class="btn primary" onclick="openGatewayConfig()">Gateway</button>
      </div>
    </div>

    <div id="dashboard" class="tab active">
      <h2>Dashboard</h2>
      <div id="dashinfo">Loading...</div>
    </div>

    <div id="digital" class="tab">
      <h2>Digital Inputs</h2>
      <div class="small">Configure and view 4 digital inputs here (placeholder).</div>
    </div>

    <div id="analog" class="tab">
      <h2>Analog Inputs</h2>
      <div class="small">Configure and view 2 analog inputs here (placeholder).</div>
    </div>

    <div id="relays" class="tab">
      <h2>Relay Outputs</h2>
      <div class="small">Control 2 relay outputs (placeholder).</div>
    </div>

    <div id="alarms" class="tab">
      <h2>Alarm Management</h2>
      <div class="small">Alarm configuration and TTA/TTR (placeholder).</div>
    </div>

    <div id="phones" class="tab">
      <h2>Phone Number Management</h2>
      <div class="small">Manage authorized and recipient phone numbers (placeholder).</div>
    </div>

    <div id="network" class="tab">
      <h2>Network Configuration</h2>
      <div id="netcfg">Loading network configuration...</div>
    </div>

    <div id="events" class="tab">
      <h2>Event Logs</h2>
      <div class="small">Event log viewer (placeholder).</div>
    </div>

    <div id="diag" class="tab">
      <h2>Diagnostics</h2>
      <div class="small">System diagnostics and status (placeholder).</div>
    </div>
  </div>

<script>
function openGatewayConfig(){ location.href='/gateway-config'; }
document.getElementById('nav').addEventListener('click', function(e){
  var li = e.target.closest('li'); if(!li) return;
  var tab = li.getAttribute('data-tab');
  document.querySelectorAll('.nav li').forEach(function(n){ n.classList.remove('active'); });
  li.classList.add('active');
  document.querySelectorAll('.tab').forEach(function(t){ t.classList.remove('active'); });
  var el = document.getElementById(tab); if(el) el.classList.add('active');
});

function loadDashboard(){
  fetch('/api/dashboard').then(r=>r.json()).then(d=>{
    var html = '<ul>';
    html += '<li><strong>Serial:</strong> '+(d.serial_number||'Not set')+'</li>';
    html += '<li><strong>AP IP:</strong> '+(d.ap_ip||'-')+'</li>';
    html += '<li><strong>Ethernet IP:</strong> '+(d.eth_ip||'-')+'</li>';
    html += '<li><strong>DHCP:</strong> '+(d.use_dhcp? 'Enabled':'Disabled')+'</li>';
    html += '<li><strong>Static IP:</strong> '+(d.static_ip||'-')+'</li>';
    html += '</ul>';
    document.getElementById('dashinfo').innerHTML = html;
    document.getElementById('netcfg').textContent = '';
  }).catch(e=>{ document.getElementById('dashinfo').textContent = 'Failed to load dashboard'; });
}
loadDashboard();
</script>
</body>
</html>

)rawliteral";
}
