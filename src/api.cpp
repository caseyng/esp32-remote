/**
 * api.cpp — HTTP REST API for the ESP32-S3 Fan Remote Controller
 *
 * All endpoints return JSON.  Error responses always include an "error" field.
 * Endpoints that mutate state immediately persist via configSave().
 *
 * Endpoint summary:
 *   GET  /fans
 *   POST /fans/add
 *   POST /fans/{id}/update
 *   DEL  /fans/{id}
 *   POST /fans/{id}/learn/{command}
 *   POST /fans/{id}/off
 *   POST /fans/{id}/speed/{level}
 *   POST /fans/{id}/light/on
 *   POST /fans/{id}/light/off
 *   GET  /status
 */

#include "api.h"
#include "config.h"
#include "rf.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

// ---------------------------------------------------------------------------
// Server instance
// ---------------------------------------------------------------------------

static AsyncWebServer g_server(80);

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

static void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    req->send(code, "application/json", body);
}

static void sendOk(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
}

static void sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    JsonDocument doc;
    doc["ok"]    = false;
    doc["error"] = msg;
    sendJson(req, code, doc);
}

// ---------------------------------------------------------------------------
// Parse the integer fan id from a URL parameter.
// Returns -1 on error and sends a 400 response.
// ---------------------------------------------------------------------------

static int parseFanId(AsyncWebServerRequest* req, const String& paramStr) {
    // paramStr comes from the URL path segment e.g. "42"
    for (char c : paramStr) {
        if (!isdigit(c)) return -1;
    }
    return paramStr.toInt();
}

// ---------------------------------------------------------------------------
// Validate that a command is applicable to a specific fan
// (e.g. light commands only for fans that have lights)
// Returns nullptr on success, or a human-readable error string on failure.
// ---------------------------------------------------------------------------

static const char* validateCommandForFan(const FanConfig* fan, const String& command) {
    if (!isValidCommand(command)) {
        return "unknown command";
    }
    if ((command == "light_on" || command == "light_off") && !fan->lights) {
        return "fan does not have lights";
    }
    // speed_4..speed_6 — check against max_speed
    if (command.startsWith("speed_")) {
        int level = command.substring(6).toInt();
        if (level > fan->max_speed) {
            return "speed level exceeds fan max_speed";
        }
    }
    return nullptr;  // valid
}

// ---------------------------------------------------------------------------
// Build the "codes_learned" object for a fan
// ---------------------------------------------------------------------------

static void appendCodesLearned(JsonObject& fanObj, const FanConfig& fan) {
    JsonObject cl = fanObj["codes_learned"].to<JsonObject>();
    for (int i = 0; ALL_COMMANDS[i] != nullptr; ++i) {
        const String cmd = ALL_COMMANDS[i];
        auto it = fan.codes.find(cmd);
        bool learned = (it != fan.codes.end() && it->second.value != 0);
        cl[cmd] = learned;
    }
}

// ---------------------------------------------------------------------------
// GET /fans
// ---------------------------------------------------------------------------

static void handleGetFans(AsyncWebServerRequest* req) {
    Serial.println("[api] GET /fans");

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const FanConfig& fan : getAllFans()) {
        JsonObject fanObj = arr.add<JsonObject>();
        fanObj["id"]        = fan.id;
        fanObj["name"]      = fan.name;
        fanObj["max_speed"] = fan.max_speed;
        fanObj["lights"]    = fan.lights;
        appendCodesLearned(fanObj, fan);
    }

    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// POST /fans/add
// Body: {"name": "...", "max_speed": N, "lights": bool}
// ---------------------------------------------------------------------------

static void handleAddFan(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                          size_t index, size_t total) {
    if (index + len < total) return;  // wait for full body

    Serial.println("[api] POST /fans/add");

    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);
    if (err) {
        sendError(req, 400, "invalid JSON body");
        return;
    }

    if (!body["name"].is<const char*>() && !body["name"].is<String>()) {
        sendError(req, 400, "name is required and must be a string");
        return;
    }
    String name = body["name"].as<String>();
    if (name.isEmpty()) {
        sendError(req, 400, "name cannot be empty");
        return;
    }

    if (!body["max_speed"].is<int>()) {
        sendError(req, 400, "max_speed is required and must be an integer");
        return;
    }
    int max_speed = body["max_speed"].as<int>();
    if (max_speed < 1 || max_speed > 6) {
        sendError(req, 400, "max_speed must be between 1 and 6");
        return;
    }

    if (!body["lights"].is<bool>()) {
        sendError(req, 400, "lights is required and must be a boolean");
        return;
    }
    bool lights = body["lights"].as<bool>();

    int newId = addFan(name, max_speed, lights);
    if (newId < 0) {
        sendError(req, 400, "failed to add fan");
        return;
    }

    if (!configSave()) {
        Serial.println("[api] WARNING: configSave failed after addFan");
    }

    JsonDocument resp;
    resp["ok"] = true;
    resp["id"] = newId;
    sendJson(req, 200, resp);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/update
// Body: any subset of {"name", "max_speed", "lights"}
// ---------------------------------------------------------------------------

static void handleUpdateFan(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                              size_t index, size_t total, int fanId) {
    if (index + len < total) return;

    Serial.printf("[api] POST /fans/%d/update\n", fanId);

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        sendError(req, 404, "fan not found");
        return;
    }

    JsonDocument body;
    DeserializationError err = deserializeJson(body, data, len);
    if (err) {
        sendError(req, 400, "invalid JSON body");
        return;
    }

    // Optional fields — nullptr means "don't change"
    String nameVal;
    const String* namePtr = nullptr;
    int msVal = 0;
    const int* msPtr = nullptr;
    bool lightsVal = false;
    const bool* lightsPtr = nullptr;

    if (body["name"].is<const char*>() || body["name"].is<String>()) {
        nameVal = body["name"].as<String>();
        if (nameVal.isEmpty()) {
            sendError(req, 400, "name cannot be empty");
            return;
        }
        namePtr = &nameVal;
    }

    if (body["max_speed"].is<int>()) {
        msVal = body["max_speed"].as<int>();
        if (msVal < 1 || msVal > 6) {
            sendError(req, 400, "max_speed must be between 1 and 6");
            return;
        }
        msPtr = &msVal;
    }

    if (body["lights"].is<bool>()) {
        lightsVal = body["lights"].as<bool>();
        lightsPtr = &lightsVal;
    }

    if (!updateFan(fanId, namePtr, msPtr, lightsPtr)) {
        sendError(req, 400, "update failed");
        return;
    }

    if (!configSave()) {
        Serial.println("[api] WARNING: configSave failed after updateFan");
    }

    sendOk(req);
}

// ---------------------------------------------------------------------------
// DELETE /fans/{id}
// ---------------------------------------------------------------------------

static void handleDeleteFan(AsyncWebServerRequest* req, int fanId) {
    Serial.printf("[api] DELETE /fans/%d\n", fanId);

    if (!getFanById(fanId)) {
        sendError(req, 404, "fan not found");
        return;
    }

    if (!deleteFan(fanId)) {
        sendError(req, 404, "fan not found");
        return;
    }

    if (!configSave()) {
        Serial.println("[api] WARNING: configSave failed after deleteFan");
    }

    sendOk(req);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/learn/{command}
// ---------------------------------------------------------------------------

static void handleLearn(AsyncWebServerRequest* req, int fanId, const String& command) {
    Serial.printf("[api] POST /fans/%d/learn/%s\n", fanId, command.c_str());

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        sendError(req, 404, "fan not found");
        return;
    }

    const char* cmdErr = validateCommandForFan(fan, command);
    if (cmdErr) {
        sendError(req, 400, cmdErr);
        return;
    }

    uint32_t value    = 0;
    uint16_t pulse    = 350;
    uint8_t  protocol = 1;
    uint8_t  bits     = 24;

    String fanIdStr = String(fanId);
    bool captured = rfLearn(fanIdStr, command, &value, &pulse, &protocol, &bits, 30000);

    if (!captured) {
        sendError(req, 408, "timeout: no RF signal received within 30 seconds");
        return;
    }

    if (!setFanCode(fanId, command, value, pulse, protocol, bits)) {
        sendError(req, 500, "failed to save learned code");
        return;
    }

    if (!configSave()) {
        Serial.println("[api] WARNING: configSave failed after learn");
    }

    JsonDocument resp;
    resp["ok"]       = true;
    resp["value"]    = value;
    resp["pulse"]    = pulse;
    resp["protocol"] = protocol;
    resp["bits"]     = bits;
    sendJson(req, 200, resp);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/off
// ---------------------------------------------------------------------------

static void handleFanOff(AsyncWebServerRequest* req, int fanId) {
    Serial.printf("[api] POST /fans/%d/off\n", fanId);

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        sendError(req, 404, "fan not found");
        return;
    }

    auto it = fan->codes.find("off");
    if (it == fan->codes.end() || it->second.value == 0) {
        sendError(req, 409, "off code not yet learned");
        return;
    }

    const RFCode& code = it->second;
    rfSend(code.value, code.pulse, code.protocol, code.bits);
    sendOk(req);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/speed/{level}
// ---------------------------------------------------------------------------

static void handleFanSpeed(AsyncWebServerRequest* req, int fanId, int level) {
    Serial.printf("[api] POST /fans/%d/speed/%d\n", fanId, level);

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        sendError(req, 404, "fan not found");
        return;
    }

    if (level < 1 || level > fan->max_speed) {
        sendError(req, 400, "speed level out of range for this fan");
        return;
    }

    String cmd = "speed_" + String(level);
    auto it = fan->codes.find(cmd);
    if (it == fan->codes.end() || it->second.value == 0) {
        sendError(req, 409, "speed code not yet learned");
        return;
    }

    const RFCode& code = it->second;
    rfSend(code.value, code.pulse, code.protocol, code.bits);
    sendOk(req);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/light/on
// POST /fans/{id}/light/off
// ---------------------------------------------------------------------------

static void handleFanLight(AsyncWebServerRequest* req, int fanId, bool turnOn) {
    Serial.printf("[api] POST /fans/%d/light/%s\n", fanId, turnOn ? "on" : "off");

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        sendError(req, 404, "fan not found");
        return;
    }

    if (!fan->lights) {
        sendError(req, 400, "fan does not have lights");
        return;
    }

    String cmd = turnOn ? "light_on" : "light_off";
    auto it = fan->codes.find(cmd);
    if (it == fan->codes.end() || it->second.value == 0) {
        sendError(req, 409, "light code not yet learned");
        return;
    }

    const RFCode& code = it->second;
    rfSend(code.value, code.pulse, code.protocol, code.bits);
    sendOk(req);
}

// ---------------------------------------------------------------------------
// apiInit — register all routes and start the server
// ---------------------------------------------------------------------------

void apiInit() {
    // -----------------------------------------------------------------------
    // GET /fans
    // -----------------------------------------------------------------------
    g_server.on("/fans", HTTP_GET, [](AsyncWebServerRequest* req) {
        handleGetFans(req);
    });

    // -----------------------------------------------------------------------
    // POST /fans/add
    // -----------------------------------------------------------------------
    g_server.on("/fans/add", HTTP_POST,
        // onRequest — only called when there is no body handler; not used here
        [](AsyncWebServerRequest* req) {},
        // onUpload — not used
        nullptr,
        // onBody
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            handleAddFan(req, data, len, index, total);
        }
    );

    // -----------------------------------------------------------------------
    // Dynamic routes via URL regex-style matching with path params.
    // ESPAsyncWebServer supports {variable} placeholders in paths.
    // -----------------------------------------------------------------------

    // POST /fans/{id}/update
    g_server.on("/fans/{id}/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            int fanId = parseFanId(req, req->pathArg(0));
            if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
            handleUpdateFan(req, data, len, index, total, fanId);
        }
    );

    // DELETE /fans/{id}
    g_server.on("/fans/{id}", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleDeleteFan(req, fanId);
    });

    // POST /fans/{id}/learn/{command}
    g_server.on("/fans/{id}/learn/{command}", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            int fanId = parseFanId(req, req->pathArg(0));
            if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
            String command = req->pathArg(1);
            handleLearn(req, fanId, command);
        }
    );

    // POST /fans/{id}/off
    g_server.on("/fans/{id}/off", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanOff(req, fanId);
    });

    // POST /fans/{id}/speed/{level}
    g_server.on("/fans/{id}/speed/{level}", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        String levelStr = req->pathArg(1);
        for (char c : levelStr) {
            if (!isdigit(c)) { sendError(req, 400, "invalid speed level"); return; }
        }
        int level = levelStr.toInt();
        handleFanSpeed(req, fanId, level);
    });

    // POST /fans/{id}/light/on
    g_server.on("/fans/{id}/light/on", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanLight(req, fanId, true);
    });

    // POST /fans/{id}/light/off
    g_server.on("/fans/{id}/light/off", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanLight(req, fanId, false);
    });

    // GET /status
    g_server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["uptime_s"]        = millis() / 1000;
        doc["free_heap"]       = ESP.getFreeHeap();
        doc["min_free_heap"]   = ESP.getMinFreeHeap();
        doc["wifi_rssi"]       = WiFi.RSSI();
        doc["wifi_ssid"]       = WiFi.SSID();
        doc["ip"]              = WiFi.localIP().toString();

        size_t fsTotal = 0, fsUsed = 0;
        if (LittleFS.totalBytes() > 0) {
            fsTotal = LittleFS.totalBytes();
            fsUsed  = LittleFS.usedBytes();
        }
        doc["fs_total_bytes"]  = fsTotal;
        doc["fs_used_bytes"]   = fsUsed;
        doc["fan_count"]       = (int)getAllFans().size();

        String body;
        serializeJson(doc, body);
        req->send(200, "application/json", body);
    });

    // -----------------------------------------------------------------------
    // 404 catch-all
    // -----------------------------------------------------------------------
    g_server.onNotFound([](AsyncWebServerRequest* req) {
        sendError(req, 404, "endpoint not found");
    });

    g_server.begin();
    Serial.println("[api] HTTP server started on port 80");
}
