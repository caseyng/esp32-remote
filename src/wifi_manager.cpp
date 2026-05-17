/**
 * wifi_manager.cpp — WiFi station/AP management with NVS credential storage.
 *
 * Credentials are stored in the "wifi" NVS namespace under keys "ssid" and "pass".
 * The captive AP config page is served on 192.168.4.1 port 80 only while in AP
 * mode; once the main API server starts the AP server has already been stopped.
 */

#include "wifi_manager.h"

#include <WiFi.h>
#include <Preferences.h>         // NVS wrapper in Arduino-ESP32
#include <WebServer.h>           // Synchronous server — only used during AP config

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr const char* AP_SSID      = "ESP32-Fan-Remote";
static constexpr const char* AP_PASSWORD  = "fanremote123";
static constexpr const char* NVS_NS       = "wifi";
static constexpr const char* NVS_KEY_SSID = "ssid";
static constexpr const char* NVS_KEY_PASS = "pass";
static constexpr unsigned long STA_TIMEOUT_MS = 15000;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bool       g_connected = false;
static Preferences g_prefs;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

static String nvsGetSSID() {
    g_prefs.begin(NVS_NS, true);  // read-only
    String val = g_prefs.getString(NVS_KEY_SSID, "");
    g_prefs.end();
    return val;
}

static String nvsGetPass() {
    g_prefs.begin(NVS_NS, true);
    String val = g_prefs.getString(NVS_KEY_PASS, "");
    g_prefs.end();
    return val;
}

static void nvsSaveCreds(const String& ssid, const String& pass) {
    g_prefs.begin(NVS_NS, false);  // read-write
    g_prefs.putString(NVS_KEY_SSID, ssid);
    g_prefs.putString(NVS_KEY_PASS, pass);
    g_prefs.end();
    Serial.printf("[wifi] Saved credentials for SSID '%s'\n", ssid.c_str());
}

// ---------------------------------------------------------------------------
// HTML for the AP configuration page
// ---------------------------------------------------------------------------

static const char AP_CONFIG_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Fan Remote — WiFi Setup</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 400px; margin: 40px auto; padding: 0 16px; }
    h1   { font-size: 1.4em; color: #333; }
    label { display: block; margin-top: 12px; font-weight: bold; }
    input[type=text], input[type=password] {
      width: 100%; padding: 8px; box-sizing: border-box;
      border: 1px solid #ccc; border-radius: 4px; margin-top: 4px;
    }
    button {
      margin-top: 20px; width: 100%; padding: 10px;
      background: #0078d4; color: white; border: none;
      border-radius: 4px; font-size: 1em; cursor: pointer;
    }
    button:hover { background: #005fa3; }
    .msg { margin-top: 16px; padding: 10px; border-radius: 4px; }
    .ok  { background: #d4edda; color: #155724; }
    .err { background: #f8d7da; color: #721c24; }
  </style>
</head>
<body>
  <h1>ESP32 Fan Remote — WiFi Setup</h1>
  <p>Enter your home WiFi credentials. The device will restart and connect automatically.</p>
  <form method="POST" action="/save">
    <label for="ssid">WiFi Network (SSID)</label>
    <input type="text" id="ssid" name="ssid" required placeholder="Your WiFi name" autocomplete="off">
    <label for="pass">Password</label>
    <input type="password" id="pass" name="pass" placeholder="Leave blank for open networks">
    <button type="submit">Save &amp; Restart</button>
  </form>
</body>
</html>
)rawhtml";

static const char AP_SAVED_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Saved!</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 400px; margin: 40px auto; padding: 0 16px; }
  </style>
</head>
<body>
  <h1>Credentials saved!</h1>
  <p>The device is restarting. Connect back to your home WiFi and find the device's IP on your router or via the serial monitor.</p>
</body>
</html>
)rawhtml";

// ---------------------------------------------------------------------------
// AP mode — serve config page and wait for credentials
// ---------------------------------------------------------------------------

static void runAPMode() {
    Serial.println("[wifi] Starting AP mode: SSID=" AP_SSID);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[wifi] AP IP: %s\n", apIP.toString().c_str());

    WebServer apServer(80);

    // Serve the config form
    apServer.on("/", HTTP_GET, [&]() {
        apServer.send_P(200, "text/html", AP_CONFIG_HTML);
    });

    // Handle form submission
    apServer.on("/save", HTTP_POST, [&]() {
        String ssid = apServer.arg("ssid");
        String pass = apServer.arg("pass");

        if (ssid.isEmpty()) {
            apServer.send(400, "text/plain", "SSID cannot be empty");
            return;
        }

        nvsSaveCreds(ssid, pass);
        apServer.send_P(200, "text/html", AP_SAVED_HTML);

        // Brief delay so the browser can receive the response before reboot
        delay(1500);
        Serial.println("[wifi] Credentials saved — restarting...");
        ESP.restart();
    });

    // Captive-portal redirect: any unknown path → config page
    apServer.onNotFound([&]() {
        apServer.sendHeader("Location", "http://192.168.4.1/", true);
        apServer.send(302, "text/plain", "");
    });

    apServer.begin();
    Serial.println("[wifi] AP config server running at http://192.168.4.1");
    Serial.println("[wifi] Connect to '" AP_SSID "' (password: " AP_PASSWORD ") to configure WiFi.");

    // Loop forever — the only way out is a reboot triggered by /save
    while (true) {
        apServer.handleClient();
        delay(10);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void wifiInit() {
    Serial.println("[wifi] Initialising...");

    String ssid = nvsGetSSID();
    String pass = nvsGetPass();

    if (ssid.isEmpty()) {
        Serial.println("[wifi] No stored credentials found");
        runAPMode();  // does not return
        return;
    }

    Serial.printf("[wifi] Connecting to SSID '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start >= STA_TIMEOUT_MS) {
            Serial.println("[wifi] Connection timed out");
            runAPMode();  // does not return
            return;
        }
        delay(250);
        Serial.print(".");
    }

    Serial.println();
    g_connected = true;
    Serial.printf("[wifi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

bool wifiIsConnected() {
    return g_connected && (WiFi.status() == WL_CONNECTED);
}

String wifiGetIP() {
    if (!wifiIsConnected()) return "0.0.0.0";
    return WiFi.localIP().toString();
}
