#include "TCP.h"
#include "Shared.h"
#include <SPI.h>
#include <LittleFS.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <new>

// Modbus TCP server removed for RAMS; keep only Ethernet link + DHCP management

// W5500 SPI pin map (matches existing wiring in this project)
static const int ETH_SPI_SCK  = 18;
static const int ETH_SPI_MISO = 19;
static const int ETH_SPI_MOSI = 23;
static const int ETH_PHY_CS   = 5;
static const int ETH_PHY_IRQ  = -1; // Not connected on current hardware
static const int ETH_PHY_RST  = 14;
static const int ETH_W5500_ADDR = 1;

static bool ethInitialized = false;
static bool dhcpConfigured = false;
static bool runningOnStaticFallback = false;
static bool networkReady = false;
static unsigned long lastDhcpReacquireMs = 0;
static unsigned long lastLinkCheckMs = 0;
static constexpr unsigned long DHCP_REACQUIRE_INTERVAL_MS = 30000;
static constexpr uint8_t DHCP_REACQUIRE_FAILS_BEFORE_REINIT = 3;
static constexpr unsigned long ETH_LINK_CHECK_INTERVAL_MS = 5000; // Check link every 5 seconds
static constexpr unsigned long DHCP_INITIAL_WAIT_MS = 20000;
static constexpr unsigned long DHCP_STARTUP_RETRY_WAIT_MS = 12000;
static constexpr unsigned long DHCP_REACQUIRE_WAIT_MS = 10000;
static constexpr unsigned long DHCP_LINK_RECOVERY_WAIT_MS = 10000;
static constexpr unsigned long DHCP_INVALID_IP_RETRY_INTERVAL_MS = 15000;
static constexpr uint8_t DHCP_INVALID_IP_FAILS_BEFORE_REINIT = 3;
static constexpr unsigned long DHCP_HOLDOVER_GRACE_MS = 20000;
static bool lastKnownLinkState = false;
static uint8_t dhcpReacquireFailCount = 0;
static bool hadDhcpLeaseSinceBoot = false;
static unsigned long networkDegradedSinceMs = 0;
static unsigned long lastInvalidIpDhcpRetryMs = 0;
static uint8_t invalidIpDhcpFailCount = 0;
static unsigned long dhcpHoldoverGraceUntilMs = 0;
static unsigned long lastTcpWorkMs = 0;
static uint16_t configuredHttpPort = 80;
static bool invalidIpStateLogged = false;
static bool linkDownStateLogged = false;
static unsigned long lastDhcpPromotionDeferredLogMs = 0;
static constexpr unsigned long TCP_IDLE_LOOP_MS = 50;
static constexpr unsigned long TCP_ACTIVE_LOOP_MS = 10;
static constexpr unsigned long MODBUS_TCP_STATUS_LOG_MS = 60000;
static constexpr unsigned long MODBUS_TCP_ACTIVE_WINDOW_MS = 60000;
static constexpr unsigned long DHCP_PROMOTION_DEFER_LOG_INTERVAL_MS = 30000;
static constexpr bool AUTO_PROMOTE_STATIC_FALLBACK_TO_DHCP = false;
static bool waitForEthIP(unsigned long timeoutMs);
static const char *currentEthModeLabel();
static bool acquireDhcpLease(unsigned long timeoutMs);
static bool recoverW5500Link(const GatewaySettings &settings, const char *reasonTag, bool preferStaticFallback);

static void suppressW5500DisconnectNoise() {
  // The W5500 driver can emit repeated "received frame was truncated" errors
  // while the Ethernet cable is unplugged or bouncing. Link monitoring below
  // handles the real state transition, so keep this driver tag quiet.
  esp_log_level_set("w5500.mac", ESP_LOG_NONE);
}

static void resetTcpServerState(const char *reasonTag) {
  // Previously reset Modbus TCP server and clients. For RAMS we rely on
  // AsyncWebServer (started by AP task) to provide the HTTP UI. Keep a log
  // entry for diagnostics and mark network service as not ready.
  Serial.print("[ETH] TCP service reset: ");
  Serial.println(reasonTag);
  networkReady = false;
}

static void logRecoveryOutcome(const char *pathTag) {
  if (!Shared_lockSPI(pdMS_TO_TICKS(5))) return;
  Serial.print("[ETH] Recovery(");
  Serial.print(pathTag);
  Serial.print("): mode=");
  Serial.print(currentEthModeLabel());
  Serial.print(", ip=");
  Serial.println(ETH.localIP());
  Shared_unlockSPI();
}

static bool ensureNetworkServiceStarted() {
  // For RAMS we don't run Modbus TCP server. Return true when Ethernet is
  // initialized and has a usable IP so the AsyncWebServer can serve over it.
  return ethInitialized && networkReady && lastKnownLinkState;
}
static bool isDhcpClientStarted() {
  esp_netif_t *ethNetif = esp_netif_get_handle_from_ifkey("ETH_DEF");
  if (ethNetif == nullptr) return false;

  esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
  if (esp_netif_dhcpc_get_status(ethNetif, &status) != ESP_OK) return false;
  return status == ESP_NETIF_DHCP_STARTED;
}

static const char *currentEthModeLabel() {
  if (!dhcpConfigured) return "STATIC";
  if (runningOnStaticFallback) return "STATIC_FALLBACK";
  return isDhcpClientStarted() ? "DHCP_ACTIVE" : "DHCP_REQUESTED_NO_CLIENT";
}

static bool isValidIP(const IPAddress &ip) {
  return !(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
}

static bool isUsableDhcpLease() {
  if (!Shared_lockSPI(pdMS_TO_TICKS(5))) return false;
  bool result = ETH.linkUp()
      && isValidIP(ETH.localIP())
      && isValidIP(ETH.subnetMask())
      && isValidIP(ETH.gatewayIP());
  Shared_unlockSPI();
  return result;
}

static bool enableDhcpMode() {
  IPAddress zero(0, 0, 0, 0);
  return ETH.config(zero, zero, zero, zero, zero);
}

static bool acquireDhcpLease(unsigned long timeoutMs) {
  if (!enableDhcpMode()) return false;
  return waitForEthIP(timeoutMs) && isUsableDhcpLease();
}

static bool reinitializeEthStackForDhcp() {
  // NOTE:
  // Hard ETH teardown/restart (ETH.end + ETH.begin) can panic on some builds
  // while the lwIP/web stack is active on the other core. Keep recovery
  // non-destructive: re-request DHCP without bringing the interface down.
  Serial.println("[ETH] DHCP recovery: safe DHCP re-acquire (no ETH.end)");
  resetTcpServerState("eth-reinit");

  if (!enableDhcpMode()) {
    Serial.println("[ETH] DHCP recovery: unable to enable DHCP");
    return false;
  }

  if (!waitForEthIP(7000) || !isUsableDhcpLease()) {
    Serial.println("[ETH] DHCP recovery: DHCP still unavailable after safe retry");
    return false;
  }

  Serial.println("[ETH] DHCP recovery: success after safe retry");
  return true;
}

static bool waitForEthIP(unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (!Shared_lockSPI(pdMS_TO_TICKS(2))) {
      delay(50);
      continue;
    }
    bool hasLink = ETH.linkUp() && isValidIP(ETH.localIP());
    Shared_unlockSPI();
    if (hasLink) return true;
    delay(100);
  }
  
  if (!Shared_lockSPI(pdMS_TO_TICKS(2))) return false;
  bool result = ETH.linkUp() && isValidIP(ETH.localIP());
  Shared_unlockSPI();
  return result;
}

static bool applyStaticEthConfig(const GatewaySettings &settings, const char *reasonTag) {
  IPAddress ip(settings.staticIp[0], settings.staticIp[1], settings.staticIp[2], settings.staticIp[3]);
  IPAddress gw(settings.gatewayIp[0], settings.gatewayIp[1], settings.gatewayIp[2], settings.gatewayIp[3]);
  IPAddress sn(settings.subnetMask[0], settings.subnetMask[1], settings.subnetMask[2], settings.subnetMask[3]);

  if (!isValidIP(ip)) {
    Serial.print("[ETH] ERROR: invalid static IP for ");
    Serial.println(reasonTag);
    return false;
  }

  if (!ETH.config(ip, gw, sn, gw, gw)) {
    Serial.print("[ETH] ERROR: ETH.config failed for ");
    Serial.println(reasonTag);
    return false;
  }

  if (!waitForEthIP(10000)) {
    Serial.print("[ETH] ERROR: no valid IP after static config for ");
    Serial.println(reasonTag);
    return false;
  }

  return true;
}

static bool beginW5500Hardware(const char *reasonTag) {
  pinMode(ETH_PHY_CS, OUTPUT);
  digitalWrite(ETH_PHY_CS, HIGH);
  pinMode(ETH_PHY_RST, OUTPUT);
  digitalWrite(ETH_PHY_RST, LOW);
  delay(50);
  digitalWrite(ETH_PHY_RST, HIGH);
  delay(250);

  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI, ETH_PHY_CS);
  SPI.setFrequency(4000000); //set to 4 MHz for better stability with W5500; some modules may not handle higher speeds reliably, make it 4000000 if you experience issues or even 2000000 for very problematic ones

  Serial.print("[ETH] Starting Ethernet: ");
  Serial.println(reasonTag);

  if (!ETH.begin(ETH_PHY_W5500, ETH_W5500_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, SPI)) {
    Serial.print("[ETH] ERROR: ETH.begin failed for ");
    Serial.println(reasonTag);
    return false;
  }

  return true;
}

static bool recoverW5500Link(const GatewaySettings &settings, const char *reasonTag, bool preferStaticFallback) {
  Serial.print("[ETH] W5500 link recovery: ");
  Serial.println(reasonTag);

  resetTcpServerState(reasonTag);
  networkReady = false;
  lastKnownLinkState = false;

  // Do not call ETH.end(), SPI.end(), ETH.begin(), or toggle W5500 reset here.
  // This Arduino-ESP32/W5500 stack can panic if the active driver/lwIP stack is
  // disturbed from the TCP task. Keep the driver alive and only rebind IP state.
  delay(250);

  if (preferStaticFallback || !settings.useDhcp) {
    networkReady = applyStaticEthConfig(settings, reasonTag);
    runningOnStaticFallback = settings.useDhcp ? networkReady : false;
  } else {
    networkReady = acquireDhcpLease(DHCP_LINK_RECOVERY_WAIT_MS);
    if (networkReady && isUsableDhcpLease()) {
      runningOnStaticFallback = false;
      hadDhcpLeaseSinceBoot = true;
    } else {
      Serial.println("[ETH] DHCP not available after link recovery, using static fallback");
      networkReady = applyStaticEthConfig(settings, reasonTag);
      runningOnStaticFallback = networkReady;
    }
  }

  if (Shared_lockSPI(pdMS_TO_TICKS(5))) {
    lastKnownLinkState = ETH.linkUp();
    networkReady = networkReady && lastKnownLinkState && isValidIP(ETH.localIP());
    Shared_unlockSPI();
  } else {
    lastKnownLinkState = false;
    networkReady = false;
  }

  if (networkReady) {
    linkDownStateLogged = false;
    invalidIpStateLogged = false;
    dhcpHoldoverGraceUntilMs = 0;
    networkDegradedSinceMs = 0;
    lastDhcpReacquireMs = millis();
    dhcpReacquireFailCount = 0;
    invalidIpDhcpFailCount = 0;
    logRecoveryOutcome(reasonTag);
  }

  return networkReady;
}

void TCP_init() {
  GatewaySettings settings = {};
  Shared_getGatewaySettings(settings);
  suppressW5500DisconnectNoise();
  configuredHttpPort = settings.httpPort;
  dhcpConfigured = settings.useDhcp;
  runningOnStaticFallback = false;
  networkReady = false;
  lastKnownLinkState = false;

  // Use ESP32 lwIP Ethernet driver for W5500 so Web UI and TCP share one stack.
  if (beginW5500Hardware("startup")) {
    ethInitialized = true;
  }

  networkReady = false;

  if (ethInitialized && settings.useDhcp) {
    Serial.println("[ETH] DHCP mode");
    // Request DHCP explicitly and wait for a clean lease.
    networkReady = acquireDhcpLease(DHCP_INITIAL_WAIT_MS);

    // Some routers/switches respond slowly right after boot/link-up.
    if (!networkReady) {
      Serial.println("[ETH] DHCP startup retry...");
      networkReady = acquireDhcpLease(DHCP_STARTUP_RETRY_WAIT_MS);
    }

    if (!networkReady) {
      if (Shared_lockSPI(pdMS_TO_TICKS(5))) {
        Serial.print("[ETH] DHCP debug: linkUp=");
        Serial.print(ETH.linkUp() ? "1" : "0");
        Serial.print(", dhcpClient=");
        Serial.print(isDhcpClientStarted() ? "STARTED" : "NOT_STARTED");
        Serial.print(", ip=");
        Serial.print(ETH.localIP());
        Serial.print(", gw=");
        Serial.print(ETH.gatewayIP());
        Serial.print(", sn=");
        Serial.println(ETH.subnetMask());
        Shared_unlockSPI();
      }
      Serial.println("[ETH] DHCP timeout, switching to static IP fallback");
      networkReady = applyStaticEthConfig(settings, "DHCP fallback");
      runningOnStaticFallback = networkReady;
    } else {
      hadDhcpLeaseSinceBoot = true;
    }
  } else if (ethInitialized) {
    Serial.println("[ETH] Using static IP");
    networkReady = applyStaticEthConfig(settings, "static mode");
  }
  lastDhcpReacquireMs = millis();

  if (!Shared_lockSPI(pdMS_TO_TICKS(10))) return;
  Serial.print("[ETH] Mode: ");    Serial.println(currentEthModeLabel());
  Serial.print("[ETH] IP: ");      Serial.println(ETH.localIP());
  Serial.print("[ETH] Subnet: ");  Serial.println(ETH.subnetMask());
  Serial.print("[ETH] Gateway: "); Serial.println(ETH.gatewayIP());
  Shared_unlockSPI();
  Serial.print("[ETH] HTTP Port: ");
  Serial.println(settings.httpPort);

  if (!ethInitialized || !networkReady) {
    Serial.println("[ETH] ERROR: Ethernet not ready, network service not started");
    return;
  }

  if (Shared_lockSPI(pdMS_TO_TICKS(5))) {
    lastKnownLinkState = ETH.linkUp();
    networkReady = networkReady && lastKnownLinkState && isValidIP(ETH.localIP());
    Shared_unlockSPI();
  } else {
    lastKnownLinkState = false;
    networkReady = false;
  }

  ensureNetworkServiceStarted();

  // Start NTP after network is ready
  if (networkReady) {
    // Load saved timezone from system_config.json
    String tz = "UTC0";
    if (LittleFS.exists("/system_config.json")) {
      File f = LittleFS.open("/system_config.json", "r");
      if (f) {
        String json = f.readString();
        f.close();
        int tzIdx = json.indexOf("\"timezone\"");
        if (tzIdx >= 0) {
          int q1 = json.indexOf('"', tzIdx + 11);
          int q2 = json.indexOf('"', q1 + 1);
          if (q1 >= 0 && q2 > q1) {
            String saved = json.substring(q1 + 1, q2);
            saved.trim();
            if (saved.length() > 0) tz = saved;
          }
        }
      }
    }
    setenv("TZ", tz.c_str(), 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // Re-apply TZ after configTime since it resets SNTP internals
    setenv("TZ", tz.c_str(), 1);
    tzset();
    Serial.println("[NTP] NTP sync started, TZ=" + tz);
  }
}

static void TCP_maintainDHCP() {
  if (!ethInitialized || !dhcpConfigured) return;
  if (!lastKnownLinkState || !networkReady) return;
  if (!runningOnStaticFallback) {
    return;
  }
  unsigned long now = millis();
  if (now - lastDhcpReacquireMs < DHCP_REACQUIRE_INTERVAL_MS) return;
  lastDhcpReacquireMs = now;

  // Keep static fallback always-on unless explicitly enabled.
  // DHCP promotion probes can temporarily drop IP to 0.0.0.0 and disrupt
  // HTTP/Modbus availability even when fallback networking is healthy.
  if (!AUTO_PROMOTE_STATIC_FALLBACK_TO_DHCP) {
    return;
  }

  // Keep static fallback service uninterrupted. Rebinding to DHCP can
  // temporarily drop the active IP (0.0.0.0 window), so defer promotion while
  // the gateway is actively serving traffic or AP mode is in use.
  if (Shared_isAPModeActive()) {
    if (now - lastDhcpPromotionDeferredLogMs >= DHCP_PROMOTION_DEFER_LOG_INTERVAL_MS) {
      Serial.println("[ETH] DHCP promotion deferred to keep static fallback service stable");
      lastDhcpPromotionDeferredLogMs = now;
    }
    return;
  }

  // We are currently on static fallback; explicitly request DHCP first.
  if (!enableDhcpMode()) {
    Serial.println("[ETH] DHCP re-acquire failed: unable to enable DHCP mode");
    return;
  }

  if (waitForEthIP(DHCP_REACQUIRE_WAIT_MS) && isUsableDhcpLease()) {
    runningOnStaticFallback = false;
    networkReady = true;
    hadDhcpLeaseSinceBoot = true;
    networkDegradedSinceMs = 0;
    dhcpReacquireFailCount = 0;
    Serial.println("[ETH] DHCP re-acquire success: switched back to DHCP");
    
    if (!Shared_lockSPI(pdMS_TO_TICKS(5))) return;
    Serial.print("[ETH] Mode: ");    Serial.println(currentEthModeLabel());
    Serial.print("[ETH] IP: ");      Serial.println(ETH.localIP());
    Serial.print("[ETH] Subnet: ");  Serial.println(ETH.subnetMask());
    Serial.print("[ETH] Gateway: "); Serial.println(ETH.gatewayIP());
    Shared_unlockSPI();
    return;
  }

  GatewaySettings settings = {};
  dhcpReacquireFailCount++;
  Serial.print("[ETH] DHCP re-acquire attempt failed, count=");
  Serial.println((unsigned int)dhcpReacquireFailCount);

  if (dhcpReacquireFailCount >= DHCP_REACQUIRE_FAILS_BEFORE_REINIT) {
    if (reinitializeEthStackForDhcp()) {
      runningOnStaticFallback = false;
      networkReady = true;
      hadDhcpLeaseSinceBoot = true;
      networkDegradedSinceMs = 0;
      lastKnownLinkState = true;
      dhcpReacquireFailCount = 0;

      if (Shared_lockSPI(pdMS_TO_TICKS(5))) {
        Serial.print("[ETH] Mode: ");    Serial.println(currentEthModeLabel());
        Serial.print("[ETH] IP: ");      Serial.println(ETH.localIP());
        Serial.print("[ETH] Subnet: ");  Serial.println(ETH.subnetMask());
        Serial.print("[ETH] Gateway: "); Serial.println(ETH.gatewayIP());
        Shared_unlockSPI();
      }
      return;
    }
    dhcpReacquireFailCount = 0;
  }

  if (Shared_getGatewaySettings(settings)) {
    // Keep the existing fallback IP if it's still valid. Re-configuring static
    // on every failed DHCP re-acquire can cause avoidable TCP disruptions.
    if (Shared_lockSPI(pdMS_TO_TICKS(5))) {
      bool hasFallbackIp = ETH.linkUp() && isValidIP(ETH.localIP());
      Shared_unlockSPI();
      if (hasFallbackIp) {
        networkReady = true;
        return;
      }
    }
    networkReady = applyStaticEthConfig(settings, "reassert static fallback");
  }
}

// ---------------------------------------------------------------------------
// Link health monitoring — detect when W5500 loses connection and recover
// ---------------------------------------------------------------------------
static void TCP_monitorEthernetLink() {
  if (!ethInitialized) return;

  unsigned long now = millis();
  if (now - lastLinkCheckMs < ETH_LINK_CHECK_INTERVAL_MS) return;
  lastLinkCheckMs = now;

  // Protect W5500 SPI access from LittleFS operations
  if (!Shared_lockSPI(pdMS_TO_TICKS(10))) return;
  
  bool linkUp = ETH.linkUp();
  bool hasValidIP = isValidIP(ETH.localIP());
  
  Shared_unlockSPI();

  // Link state changed or IP became invalid
  if (linkUp != lastKnownLinkState || (linkUp && !hasValidIP)) {
    if (!linkUp) {
      if (!linkDownStateLogged) {
        Serial.println("[ETH] WARNING: Ethernet link lost, network service paused until cable reconnect");
        linkDownStateLogged = true;
      }
      invalidIpStateLogged = false;
      lastKnownLinkState = false;
      networkReady = false;
      if (networkDegradedSinceMs == 0) networkDegradedSinceMs = now;
      
      // Reset TCP server/client state and wait for physical link restore.
      // Do not run DHCP/static recovery while the cable is unplugged; it can
      // keep the W5500 driver busy in a noisy link-down state.
      resetTcpServerState("link-down");
    } else if (linkUp && !hasValidIP) {
      if (runningOnStaticFallback) {
        GatewaySettings settings = {};
        if (Shared_getGatewaySettings(settings)) {
          recoverW5500Link(settings, "link-restored-static-fallback", true);
        }
        return;
      }

      if (networkDegradedSinceMs == 0) networkDegradedSinceMs = now;
      // Hold Modbus service for a short grace window while DHCP re-acquires.
      // This avoids immediate session drops on brief lease blips.
      if (dhcpConfigured && hadDhcpLeaseSinceBoot) {
        if (dhcpHoldoverGraceUntilMs == 0) {
          dhcpHoldoverGraceUntilMs = now + DHCP_HOLDOVER_GRACE_MS;
          Serial.print("[ETH] DHCP holdover grace started (ms): ");
          Serial.println((unsigned long)DHCP_HOLDOVER_GRACE_MS);
        }
        bool graceActive = (long)(dhcpHoldoverGraceUntilMs - now) > 0;
        networkReady = graceActive;
      } else {
        networkReady = false;
      }
      if (!invalidIpStateLogged) {
        Serial.println("[ETH] WARNING: Link is up but IP is invalid, waiting for DHCP recovery");
        invalidIpStateLogged = true;
      }
      linkDownStateLogged = false;

      // Actively re-request DHCP while link is up but lease is invalid.
      if (dhcpConfigured && (now - lastInvalidIpDhcpRetryMs >= DHCP_INVALID_IP_RETRY_INTERVAL_MS)) {
        lastInvalidIpDhcpRetryMs = now;
        Serial.println("[ETH] Attempting DHCP re-acquire for invalid IP state...");
        if (enableDhcpMode() && waitForEthIP(DHCP_REACQUIRE_WAIT_MS) && isUsableDhcpLease()) {
          Serial.println("[ETH] DHCP re-acquire successful from invalid IP state");
          runningOnStaticFallback = false;
          networkReady = true;
          hadDhcpLeaseSinceBoot = true;
          networkDegradedSinceMs = 0;
          lastKnownLinkState = true;
          lastDhcpReacquireMs = now;
          dhcpReacquireFailCount = 0;
          invalidIpDhcpFailCount = 0;
          invalidIpStateLogged = false;
          linkDownStateLogged = false;
          dhcpHoldoverGraceUntilMs = 0;
          logRecoveryOutcome("invalid-ip-dhcp");
        } else {
          invalidIpDhcpFailCount++;
          Serial.print("[ETH] DHCP re-acquire failed from invalid IP state, count=");
          Serial.println((unsigned int)invalidIpDhcpFailCount);

          if (invalidIpDhcpFailCount >= DHCP_INVALID_IP_FAILS_BEFORE_REINIT) {
            Serial.println("[ETH] Invalid-IP DHCP retries exhausted, forcing Ethernet reinit");
            if (reinitializeEthStackForDhcp()) {
              runningOnStaticFallback = false;
              networkReady = true;
              hadDhcpLeaseSinceBoot = true;
              networkDegradedSinceMs = 0;
              lastKnownLinkState = true;
              lastDhcpReacquireMs = now;
              dhcpReacquireFailCount = 0;
              invalidIpDhcpFailCount = 0;
              invalidIpStateLogged = false;
              linkDownStateLogged = false;
              dhcpHoldoverGraceUntilMs = 0;
              logRecoveryOutcome("invalid-ip-reinit-dhcp");
            } else {
              GatewaySettings settings = {};
              if (Shared_getGatewaySettings(settings)) {
                networkReady = applyStaticEthConfig(settings, "invalid-ip fallback");
                runningOnStaticFallback = networkReady;
                if (networkReady) logRecoveryOutcome("invalid-ip-static-fallback");
              } else {
                networkReady = false;
              }
              dhcpHoldoverGraceUntilMs = 0;
              invalidIpDhcpFailCount = 0;
            }
          }
        }
      }
    } else if (linkUp && hasValidIP && !lastKnownLinkState) {
      Serial.println("[ETH] Ethernet link restored");
      linkDownStateLogged = false;
      invalidIpStateLogged = false;
      dhcpHoldoverGraceUntilMs = 0;

      // If DHCP is configured, explicitly re-request it on link restore.
      // Without this, the stack can remain on a previously applied static IP.
      if (runningOnStaticFallback && !AUTO_PROMOTE_STATIC_FALLBACK_TO_DHCP) {
        GatewaySettings settings = {};
        if (Shared_getGatewaySettings(settings)) {
          recoverW5500Link(settings, "link-restored-static-fallback", true);
        } else {
          networkReady = false;
        }
      } else if (dhcpConfigured) {
        GatewaySettings settings = {};
        if (Shared_getGatewaySettings(settings)) {
          if (recoverW5500Link(settings, "link-restored", false)) {
            Serial.println(runningOnStaticFallback
              ? "[ETH] DHCP not available after link recovery, keeping static fallback"
              : "[ETH] DHCP restored after link recovery");
          }
        } else {
          networkReady = false;
        }
      } else {
        GatewaySettings settings = {};
        if (Shared_getGatewaySettings(settings)) {
          recoverW5500Link(settings, "link-restored-static", true);
        } else {
          networkReady = false;
        }
      }

      if (!Shared_lockSPI(pdMS_TO_TICKS(5))) return;
      Serial.print("[ETH] Mode: ");    Serial.println(currentEthModeLabel());
      Serial.print("[ETH] IP: ");      Serial.println(ETH.localIP());
      Serial.print("[ETH] Subnet: ");  Serial.println(ETH.subnetMask());
      Serial.print("[ETH] Gateway: "); Serial.println(ETH.gatewayIP());
      Shared_unlockSPI();

      lastKnownLinkState = true;
    }
  }
}

// Modbus/TCP client handling and register sync removed for RAMS build.
// The Web UI and REST API are served by AsyncWebServer in AP task.

void TCP_taskLoop(void *pvParameters) {
  (void)pvParameters;

  for (;;) {
    unsigned long now = millis();
    if (now - lastTcpWorkMs < TCP_IDLE_LOOP_MS) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    lastTcpWorkMs = now;

    TCP_monitorEthernetLink();  // Check link health every 5 seconds
    ensureNetworkServiceStarted();
    TCP_maintainDHCP();

    vTaskDelay(pdMS_TO_TICKS(25));
  }
}
