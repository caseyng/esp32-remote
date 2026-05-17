#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// WiFi Manager
//
// Manages WiFi connectivity with automatic fallback to AP configuration mode.
//
// Boot sequence:
//   1. Load SSID/password from NVS.
//   2. If creds exist, attempt to connect (15 s timeout).
//   3. If connection succeeds → station mode, API available on assigned IP.
//   4. If no creds or timeout → start AP mode on 192.168.4.1.
//      A simple HTML page lets the user submit new credentials.
//      On save the device restores to NVS and reboots.
// ---------------------------------------------------------------------------

/**
 * Initialise WiFi.  Blocks until either:
 *   - Station mode connection succeeds, or
 *   - AP mode is active and the config web server is running.
 *
 * Must be called after Serial.begin().
 */
void wifiInit();

/**
 * Returns true if the device is in station mode and has an IP address.
 */
bool wifiIsConnected();

/**
 * Returns the current IP address as a dotted-decimal string.
 * Returns "0.0.0.0" if not connected.
 */
String wifiGetIP();
