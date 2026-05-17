/**
 * main.cpp — ESP32-S3 Fan Remote Controller
 *
 * Boot sequence:
 *   1. Serial (USB CDC) at 115200 baud.
 *   2. Mount LittleFS and load config.json.
 *   3. Initialise the 433 MHz RF subsystem (TX GPIO18, RX GPIO6).
 *   4. Connect to WiFi (station) or fall back to AP config mode.
 *   5. Start the HTTP API server on port 80.
 *   6. Print the device IP to serial.
 *
 * The AsyncWebServer is fully asynchronous; loop() has nothing to do.
 */

#include <Arduino.h>
#include <LittleFS.h>

#include "config.h"
#include "rf.h"
#include "api.h"
#include "wifi_manager.h"

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);

    // Give the USB CDC serial port a moment to enumerate on the host so that
    // early log messages are not lost when using native USB (ESP32-S3).
    delay(500);

    Serial.println("\n========================================");
    Serial.println("  ESP32-S3 Fan Remote Controller");
    Serial.println("  Firmware build: " __DATE__ " " __TIME__);
    Serial.println("========================================\n");

    // -----------------------------------------------------------------------
    // 1. Mount LittleFS and load config
    // -----------------------------------------------------------------------
    if (!configLoad()) {
        // configLoad() already logs the error; continue with empty config.
        Serial.println("[main] WARNING: starting with empty fan config");
    }

    // -----------------------------------------------------------------------
    // 2. Initialise the 433 MHz RF subsystem
    // -----------------------------------------------------------------------
    rfInit();

    // -----------------------------------------------------------------------
    // 3. Connect to WiFi (blocks until connected or AP mode activated)
    // -----------------------------------------------------------------------
    wifiInit();

    // -----------------------------------------------------------------------
    // 4. Start the HTTP API server
    // -----------------------------------------------------------------------
    apiInit();

    // -----------------------------------------------------------------------
    // 5. Print operational info
    // -----------------------------------------------------------------------
    Serial.println("\n[main] System ready.");
    Serial.printf("[main] API server:  http://%s/\n", wifiGetIP().c_str());
    Serial.printf("[main] Fans loaded: %zu\n", getAllFans().size());  // const ref, no copy
    Serial.println("[main] Waiting for requests...\n");
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void loop() {
    // AsyncWebServer handles everything on its own FreeRTOS task.
    // We just yield to keep the idle watchdog happy.
    delay(10);
}
