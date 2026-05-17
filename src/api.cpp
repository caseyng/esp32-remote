/**
 * api.cpp — HTTP REST API for the ESP32-S3 Fan Remote Controller
 *
 * All endpoints return JSON.  Error responses always include an "error" field.
 * Endpoints that mutate state immediately persist via configSave().
 *
 * Endpoint summary (grouped by function):
 *
 * Fan management:
 *   GET    /fans                        list all fans and learned codes
 *   POST   /fans/add                    add a fan  {"name","max_speed","lights","reverse"}
 *   POST   /fans/{id}/update            update fan {"name","max_speed","lights","reverse"}
 *   DELETE /fans/{id}                   delete a fan
 *
 * Learning (mirrors control paths):
 *   POST   /fans/{id}/learn/off         learn the off code
 *   POST   /fans/{id}/learn/speed/{n}   learn speed n (1-6)
 *   POST   /fans/{id}/learn/light/on    learn light on code
 *   POST   /fans/{id}/learn/light/off   learn light off code
 *   POST   /fans/{id}/learn/reverse/on  learn reverse on code
 *   POST   /fans/{id}/learn/reverse/off learn reverse off code
 *   GET    /fans/{id}/learn/status      poll learn result (RUNNING/DONE_OK/DONE_TIMEOUT)
 *
 * Unlearning:
 *   POST   /fans/{id}/unlearn/off
 *   POST   /fans/{id}/unlearn/speed/{n}
 *   POST   /fans/{id}/unlearn/light/on
 *   POST   /fans/{id}/unlearn/light/off
 *   POST   /fans/{id}/unlearn/reverse/on
 *   POST   /fans/{id}/unlearn/reverse/off
 *
 * Control:
 *   POST   /fans/{id}/off
 *   POST   /fans/{id}/speed/{n}
 *   POST   /fans/{id}/light/on
 *   POST   /fans/{id}/light/off
 *   POST   /fans/{id}/reverse/on
 *   POST   /fans/{id}/reverse/off
 *
 * System:
 *   GET    /help                        show all endpoints with examples
 *   GET    /status                      uptime, heap, wifi, filesystem stats
 *   POST   /factory-reset               wipe LittleFS and restart
 */

#include "api.h"
#include "config.h"
#include "rf.h"

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <map>

// ---------------------------------------------------------------------------
// Server instance
// ---------------------------------------------------------------------------

static AsyncWebServer g_server(80);

// ---------------------------------------------------------------------------
// Per-request body accumulator
// ESPAsyncWebServer may deliver the body in multiple chunks. We accumulate
// into a String keyed by the request pointer, then parse once complete.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Background RF learn task
// Only one learn operation runs at a time. The task writes results here and
// deletes itself; the poll endpoint reads the state.
// ---------------------------------------------------------------------------

enum class LearnState { IDLE, RUNNING, DONE_OK, DONE_TIMEOUT, DONE_ERROR };

struct LearnResult {
    LearnState state   = LearnState::IDLE;
    int        fanId   = 0;
    String     command;
    uint32_t   value   = 0;
    uint16_t   pulse   = 350;
    uint8_t    protocol = 1;
    uint8_t    bits    = 24;
    String     errorMsg;
};

static LearnResult g_learn;

static void learnTask(void* /*param*/) {
    uint32_t value = 0; uint16_t pulse = 350; uint8_t protocol = 1, bits = 24;
    String fanIdStr = String(g_learn.fanId);

    bool ok = rfLearn(fanIdStr, g_learn.command, &value, &pulse, &protocol, &bits, 30000);
    if (ok) {
        bool saved = setFanCode(g_learn.fanId, g_learn.command, value, pulse, protocol, bits);
        if (saved) {
            configSave();
            g_learn.value    = value;
            g_learn.pulse    = pulse;
            g_learn.protocol = protocol;
            g_learn.bits     = bits;
            g_learn.state    = LearnState::DONE_OK;
        } else {
            g_learn.errorMsg = "failed to save code";
            g_learn.state    = LearnState::DONE_ERROR;
        }
    } else {
        g_learn.state = LearnState::DONE_TIMEOUT;
    }
    vTaskDelete(nullptr);
}

static std::map<AsyncWebServerRequest*, String> g_bodyBufs;

static void bodyAppend(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                       size_t index, size_t total) {
    if (index == 0) g_bodyBufs[req].reserve(total);
    g_bodyBufs[req].concat(reinterpret_cast<const char*>(data), len);
}

static bool bodyComplete(AsyncWebServerRequest* req, size_t index,
                         size_t len, size_t total) {
    return (index + len >= total);
}

static String bodyConsume(AsyncWebServerRequest* req) {
    String s = std::move(g_bodyBufs[req]);
    g_bodyBufs.erase(req);
    return s;
}

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
    if (paramStr.isEmpty()) return -1;
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
    if ((command == "reverse_on" || command == "reverse_off") && !fan->reverse) {
        return "fan does not have reverse mode";
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

static void appendCodesLearned(JsonObject& fanObj, const FanConfig& fan, bool verbose) {
    JsonObject cl = fanObj["codes_learned"].to<JsonObject>();
    for (int i = 0; ALL_COMMANDS[i] != nullptr; ++i) {
        const String cmd = ALL_COMMANDS[i];
        auto it = fan.codes.find(cmd);
        bool learned = (it != fan.codes.end() && it->second.value != 0);
        if (verbose && learned) {
            JsonObject codeObj = cl[cmd].to<JsonObject>();
            codeObj["learned"]  = true;
            codeObj["value"]    = it->second.value;
            codeObj["pulse"]    = it->second.pulse;
            codeObj["protocol"] = it->second.protocol;
            codeObj["bits"]     = it->second.bits;
        } else {
            cl[cmd] = learned;
        }
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
        fanObj["reverse"]   = fan.reverse;
        bool verbose = req->hasParam("verbose");
        appendCodesLearned(fanObj, fan, verbose);
    }

    sendJson(req, 200, doc);
}

// ---------------------------------------------------------------------------
// POST /fans/add
// Body: {"name": "...", "max_speed": N, "lights": bool}
// ---------------------------------------------------------------------------

static void handleAddFan(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                          size_t index, size_t total) {
    bodyAppend(req, data, len, index, total);
    if (!bodyComplete(req, index, len, total)) return;

    Serial.println("[api] POST /fans/add");
    String raw = bodyConsume(req);

    JsonDocument body;
    DeserializationError err = deserializeJson(body, raw);
    if (err) {
        sendError(req, 400, "invalid JSON body");
        return;
    }

    if (!body["name"].is<const char*>()) {
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

    if (!body["reverse"].is<bool>()) {
        sendError(req, 400, "reverse is required and must be a boolean");
        return;
    }
    bool reverse = body["reverse"].as<bool>();

    int newId = addFan(name, max_speed, lights, reverse);
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
    sendJson(req, 201, resp);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/update
// Body: any subset of {"name", "max_speed", "lights"}
// ---------------------------------------------------------------------------

static void handleUpdateFan(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                              size_t index, size_t total, int fanId) {
    bodyAppend(req, data, len, index, total);
    if (!bodyComplete(req, index, len, total)) return;

    Serial.printf("[api] POST /fans/%d/update\n", fanId);

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        bodyConsume(req);  // discard buffer
        sendError(req, 404, "fan not found");
        return;
    }

    String raw = bodyConsume(req);
    JsonDocument body;
    DeserializationError err = deserializeJson(body, raw);
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
    bool reverseVal = false;
    const bool* reversePtr = nullptr;

    if (body["name"].is<const char*>()) {
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

    if (body["reverse"].is<bool>()) {
        reverseVal = body["reverse"].as<bool>();
        reversePtr = &reverseVal;
    }

    if (!updateFan(fanId, namePtr, msPtr, lightsPtr, reversePtr)) {
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
// Path-to-command conversion
// Converts URL path segments to internal command strings.
// e.g. "speed/3" → "speed_3", "light/on" → "light_on", "off" → "off"
// Returns empty string if the path is not a valid command.
// ---------------------------------------------------------------------------

static String pathToCommand(const String& path) {
    if (path == "off")          return "off";
    if (path == "light/on")     return "light_on";
    if (path == "light/off")    return "light_off";
    if (path == "reverse/on")   return "reverse_on";
    if (path == "reverse/off")  return "reverse_off";
    if (path.startsWith("speed/")) {
        String level = path.substring(6);
        if (level.length() > 0) return "speed_" + level;
    }
    return "";
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/learn/...
// ---------------------------------------------------------------------------

static void handleLearn(AsyncWebServerRequest* req, int fanId, const String& command) {
    Serial.printf("[api] POST /fans/%d/learn/%s\n", fanId, command.c_str());

    if (g_learn.state == LearnState::RUNNING) {
        sendError(req, 409, "another learn operation is already running");
        return;
    }

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

    g_learn = LearnResult{};
    g_learn.state   = LearnState::RUNNING;
    g_learn.fanId   = fanId;
    g_learn.command = command;

    // Run in a separate task so the async_tcp task is not blocked for 30 s
    xTaskCreate(learnTask, "rfLearn", 4096, nullptr, 1, nullptr);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["status"]  = "learning";
    resp["poll"]    = "/fans/" + String(fanId) + "/learn/status";
    sendJson(req, 202, resp);
}

static void handleLearnStatus(AsyncWebServerRequest* req, int fanId) {
    Serial.printf("[api] GET /fans/%d/learn/status\n", fanId);

    JsonDocument resp;
    switch (g_learn.state) {
        case LearnState::IDLE:
            resp["status"] = "idle";
            sendJson(req, 200, resp);
            break;
        case LearnState::RUNNING:
            if (g_learn.fanId != fanId) {
                resp["status"]  = "running";
                resp["fan_id"]  = g_learn.fanId;
                resp["command"] = g_learn.command;
                sendJson(req, 200, resp);
            } else {
                resp["status"]  = "running";
                resp["command"] = g_learn.command;
                sendJson(req, 200, resp);
            }
            break;
        case LearnState::DONE_OK:
            resp["ok"]       = true;
            resp["status"]   = "done";
            resp["value"]    = g_learn.value;
            resp["pulse"]    = g_learn.pulse;
            resp["protocol"] = g_learn.protocol;
            resp["bits"]     = g_learn.bits;
            g_learn.state    = LearnState::IDLE;
            sendJson(req, 200, resp);
            break;
        case LearnState::DONE_TIMEOUT:
            resp["ok"]     = false;
            resp["status"] = "timeout";
            resp["error"]  = "no RF signal received within 30 seconds";
            g_learn.state  = LearnState::IDLE;
            sendJson(req, 408, resp);
            break;
        case LearnState::DONE_ERROR:
            resp["ok"]     = false;
            resp["status"] = "error";
            resp["error"]  = g_learn.errorMsg;
            g_learn.state  = LearnState::IDLE;
            sendJson(req, 500, resp);
            break;
    }
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
    if (!rfSend(code.value, code.pulse, code.protocol, code.bits)) {
        sendError(req, 500, "RF transmit failed");
        return;
    }
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
    if (!rfSend(code.value, code.pulse, code.protocol, code.bits)) {
        sendError(req, 500, "RF transmit failed");
        return;
    }
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
    if (!rfSend(code.value, code.pulse, code.protocol, code.bits)) {
        sendError(req, 500, "RF transmit failed");
        return;
    }
    sendOk(req);
}

// ---------------------------------------------------------------------------
// POST /fans/{id}/reverse/on
// POST /fans/{id}/reverse/off
// ---------------------------------------------------------------------------

static void handleFanReverse(AsyncWebServerRequest* req, int fanId, bool turnOn) {
    Serial.printf("[api] POST /fans/%d/reverse/%s\n", fanId, turnOn ? "on" : "off");

    FanConfig* fan = getFanById(fanId);
    if (!fan) {
        sendError(req, 404, "fan not found");
        return;
    }

    if (!fan->reverse) {
        sendError(req, 400, "fan does not have reverse mode");
        return;
    }

    String cmd = turnOn ? "reverse_on" : "reverse_off";
    auto it = fan->codes.find(cmd);
    if (it == fan->codes.end() || it->second.value == 0) {
        sendError(req, 409, "reverse code not yet learned");
        return;
    }

    const RFCode& code = it->second;
    if (!rfSend(code.value, code.pulse, code.protocol, code.bits)) {
        sendError(req, 500, "RF transmit failed");
        return;
    }
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
    // Dynamic routes using ASYNCWEBSERVER_REGEX regex patterns.
    // pathArg(0) = first capture group, pathArg(1) = second, etc.
    // -----------------------------------------------------------------------

    // POST /fans/<id>/update
    g_server.on("^\\/fans\\/([0-9]+)\\/update$", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            int fanId = parseFanId(req, req->pathArg(0));
            if (fanId < 0) {
                sendError(req, 400, "invalid fan id");
                req->_tempObject = reinterpret_cast<void*>(-1);
            } else {
                req->_tempObject = reinterpret_cast<void*>(static_cast<intptr_t>(fanId));
            }
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            intptr_t stored = reinterpret_cast<intptr_t>(req->_tempObject);
            if (stored == -1) return;  // error already sent in onRequest
            bodyAppend(req, data, len, index, total);
            if (!bodyComplete(req, index, len, total)) return;
            handleUpdateFan(req, data, len, index, total, static_cast<int>(stored));
        }
    );

    // DELETE /fans/<id>
    g_server.on("^\\/fans\\/([0-9]+)$", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleDeleteFan(req, fanId);
    });

    // -----------------------------------------------------------------------
    // Learning — GET status first so it doesn't match the generic learn route
    // -----------------------------------------------------------------------

    // GET /fans/<id>/learn/status
    g_server.on("^\\/fans\\/([0-9]+)\\/learn\\/status$", HTTP_GET, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleLearnStatus(req, fanId);
    });

    // POST /fans/<id>/learn/off|speed/<n>|light/on|light/off|reverse/on|reverse/off
    g_server.on("^\\/fans\\/([0-9]+)\\/learn\\/(.+)$", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            int fanId = parseFanId(req, req->pathArg(0));
            if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
            String command = pathToCommand(req->pathArg(1));
            if (command.isEmpty()) { sendError(req, 400, "invalid command path"); return; }
            handleLearn(req, fanId, command);
        }
    );

    // -----------------------------------------------------------------------
    // Unlearning
    // -----------------------------------------------------------------------

    // POST /fans/<id>/unlearn/off|speed/<n>|light/on|light/off|reverse/on|reverse/off
    g_server.on("^\\/fans\\/([0-9]+)\\/unlearn\\/(.+)$", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            int fanId = parseFanId(req, req->pathArg(0));
            if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
            String command = pathToCommand(req->pathArg(1));
            if (command.isEmpty()) { sendError(req, 400, "invalid command path"); return; }
            if (!clearFanCode(fanId, command)) { sendError(req, 404, "fan not found or command unknown"); return; }
            sendOk(req);
        }
    );

    // -----------------------------------------------------------------------
    // Control
    // -----------------------------------------------------------------------

    // POST /fans/<id>/off
    g_server.on("^\\/fans\\/([0-9]+)\\/off$", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanOff(req, fanId);
    });

    // POST /fans/<id>/speed/<level>
    g_server.on("^\\/fans\\/([0-9]+)\\/speed\\/([0-9]+)$", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        int level = req->pathArg(1).toInt();
        handleFanSpeed(req, fanId, level);
    });

    // POST /fans/<id>/light/on
    g_server.on("^\\/fans\\/([0-9]+)\\/light\\/on$", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanLight(req, fanId, true);
    });

    // POST /fans/<id>/light/off
    g_server.on("^\\/fans\\/([0-9]+)\\/light\\/off$", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanLight(req, fanId, false);
    });

    // POST /fans/<id>/reverse/on
    g_server.on("^\\/fans\\/([0-9]+)\\/reverse\\/on$", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanReverse(req, fanId, true);
    });

    // POST /fans/<id>/reverse/off
    g_server.on("^\\/fans\\/([0-9]+)\\/reverse\\/off$", HTTP_POST, [](AsyncWebServerRequest* req) {
        int fanId = parseFanId(req, req->pathArg(0));
        if (fanId < 0) { sendError(req, 400, "invalid fan id"); return; }
        handleFanReverse(req, fanId, false);
    });

    // -----------------------------------------------------------------------
    // System
    // -----------------------------------------------------------------------

    // GET /help
    g_server.on("/help", HTTP_GET, [](AsyncWebServerRequest* req) {
        String ip = WiFi.localIP().toString();
        String base = "http://" + ip;
        JsonDocument doc;

        JsonObject fans = doc["fan_management"].to<JsonObject>();
        fans["list_fans"]   = "GET "    + base + "/fans";
        fans["add_fan"]     = "POST "   + base + "/fans/add  {\"name\":\"Living Room\",\"max_speed\":6,\"lights\":true,\"reverse\":true}";
        fans["update_fan"]  = "POST "   + base + "/fans/{id}/update  {\"name\":\"New Name\"}";
        fans["delete_fan"]  = "DELETE " + base + "/fans/{id}";

        JsonObject learn = doc["learning"].to<JsonObject>();
        learn["learn_off"]        = "POST " + base + "/fans/{id}/learn/off";
        learn["learn_speed"]      = "POST " + base + "/fans/{id}/learn/speed/{1-6}";
        learn["learn_light_on"]   = "POST " + base + "/fans/{id}/learn/light/on";
        learn["learn_light_off"]  = "POST " + base + "/fans/{id}/learn/light/off";
        learn["learn_reverse_on"] = "POST " + base + "/fans/{id}/learn/reverse/on";
        learn["learn_reverse_off"]= "POST " + base + "/fans/{id}/learn/reverse/off";
        learn["learn_status"]     = "GET "  + base + "/fans/{id}/learn/status";

        JsonObject unlearn = doc["unlearning"].to<JsonObject>();
        unlearn["unlearn_off"]        = "POST " + base + "/fans/{id}/unlearn/off";
        unlearn["unlearn_speed"]      = "POST " + base + "/fans/{id}/unlearn/speed/{1-6}";
        unlearn["unlearn_light_on"]   = "POST " + base + "/fans/{id}/unlearn/light/on";
        unlearn["unlearn_light_off"]  = "POST " + base + "/fans/{id}/unlearn/light/off";
        unlearn["unlearn_reverse_on"] = "POST " + base + "/fans/{id}/unlearn/reverse/on";
        unlearn["unlearn_reverse_off"]= "POST " + base + "/fans/{id}/unlearn/reverse/off";

        JsonObject control = doc["control"].to<JsonObject>();
        control["off"]        = "POST " + base + "/fans/{id}/off";
        control["speed"]      = "POST " + base + "/fans/{id}/speed/{1-6}";
        control["light_on"]   = "POST " + base + "/fans/{id}/light/on";
        control["light_off"]  = "POST " + base + "/fans/{id}/light/off";
        control["reverse_on"] = "POST " + base + "/fans/{id}/reverse/on";
        control["reverse_off"]= "POST " + base + "/fans/{id}/reverse/off";

        JsonObject system = doc["system"].to<JsonObject>();
        system["help"]          = "GET "  + base + "/help";
        system["status"]        = "GET "  + base + "/status";
        system["factory_reset"] = "POST " + base + "/factory-reset";

        sendJson(req, 200, doc);
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
        doc["fan_count"]       = (int)getAllFans().size();  // const ref, no copy

        String body;
        serializeJson(doc, body);
        req->send(200, "application/json", body);
    });

    // POST /factory-reset — deliberately wipes LittleFS and restarts
    g_server.on("/factory-reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        Serial.println("[api] POST /factory-reset — formatting LittleFS");
        LittleFS.format();
        JsonDocument doc;
        doc["ok"]      = true;
        doc["message"] = "LittleFS formatted, restarting in 2 seconds";
        sendJson(req, 200, doc);
        delay(2000);
        ESP.restart();
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
