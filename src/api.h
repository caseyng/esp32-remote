#pragma once

#include <Arduino.h>

/**
 * Initialise and start the AsyncWebServer on port 80.
 * Registers all REST endpoints.
 * Must be called after WiFi is connected (or in AP mode) and after
 * configLoad() / rfInit() have been called.
 */
void apiInit();
