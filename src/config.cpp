#include "config.h"
#include <LittleFS.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static std::vector<FanConfig> g_fans;

static constexpr const char* CONFIG_PATH = "/config.json";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void initFanCodes(FanConfig& fan) {
    // Populate every possible command key so the map is always complete.
    for (int i = 0; ALL_COMMANDS[i] != nullptr; ++i) {
        const String cmd = ALL_COMMANDS[i];
        if (fan.codes.find(cmd) == fan.codes.end()) {
            fan.codes[cmd] = RFCode{};  // value=0 → not learned
        }
    }
}

static RFCode rfCodeFromJson(const JsonObjectConst& obj) {
    RFCode code;
    code.value    = obj["value"]    | 0u;
    code.pulse    = obj["pulse"]    | 350u;
    code.protocol = obj["protocol"] | 1u;
    code.bits     = obj["bits"]     | 24u;
    return code;
}

static void rfCodeToJson(JsonObject obj, const RFCode& code) {
    obj["value"]    = code.value;
    obj["pulse"]    = code.pulse;
    obj["protocol"] = code.protocol;
    obj["bits"]     = code.bits;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool isValidCommand(const String& command) {
    for (int i = 0; ALL_COMMANDS[i] != nullptr; ++i) {
        if (command == ALL_COMMANDS[i]) return true;
    }
    return false;
}

bool configLoad() {
    g_fans.clear();

    if (!LittleFS.begin(false)) {
        Serial.println("[config] LittleFS mount failed — run POST /factory-reset to reformat");
        return false;
    }
    Serial.println("[config] LittleFS mounted");

    if (!LittleFS.exists(CONFIG_PATH)) {
        Serial.println("[config] config.json not found, starting with empty config");
        return true;
    }

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        Serial.println("[config] Failed to open config.json for reading");
        return false;
    }

    // ArduinoJson v7: use DynamicJsonDocument via JsonDocument
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[config] JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray fans = doc["fans"].as<JsonArray>();
    for (JsonObject fanObj : fans) {
        FanConfig fan;
        fan.id        = fanObj["id"]        | 0;
        fan.name      = fanObj["name"]      | String("");
        fan.max_speed = fanObj["max_speed"] | 3;
        fan.lights    = fanObj["lights"]    | false;
        fan.reverse   = fanObj["reverse"]   | false;

        // Load codes
        JsonObject codesObj = fanObj["codes"].as<JsonObject>();
        for (JsonPair kv : codesObj) {
            const String key = kv.key().c_str();
            if (kv.value().is<JsonObject>()) {
                fan.codes[key] = rfCodeFromJson(kv.value().as<JsonObjectConst>());
            }
        }

        // Ensure all command keys exist (may add missing ones with value=0)
        initFanCodes(fan);

        g_fans.push_back(std::move(fan));
    }

    Serial.printf("[config] Loaded %zu fan(s)\n", g_fans.size());
    return true;
}

bool configSave() {
    JsonDocument doc = configToJson();

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) {
        Serial.println("[config] Failed to open config.json for writing");
        return false;
    }

    size_t written = serializeJson(doc, f);
    f.close();

    if (written == 0) {
        Serial.println("[config] serializeJson wrote 0 bytes");
        return false;
    }

    Serial.printf("[config] Saved config.json (%zu bytes)\n", written);
    return true;
}

JsonDocument configToJson() {
    JsonDocument doc;
    JsonArray fans = doc["fans"].to<JsonArray>();

    for (const FanConfig& fan : g_fans) {
        JsonObject fanObj = fans.add<JsonObject>();
        fanObj["id"]        = fan.id;
        fanObj["name"]      = fan.name;
        fanObj["max_speed"] = fan.max_speed;
        fanObj["lights"]    = fan.lights;
        fanObj["reverse"]   = fan.reverse;

        JsonObject codesObj = fanObj["codes"].to<JsonObject>();
        for (const auto& [cmd, code] : fan.codes) {
            JsonObject codeObj = codesObj[cmd].to<JsonObject>();
            rfCodeToJson(codeObj, code);
        }
    }

    return doc;
}

FanConfig* getFanById(int id) {
    for (FanConfig& fan : g_fans) {
        if (fan.id == id) return &fan;
    }
    return nullptr;
}

int addFan(const String& name, int max_speed, bool lights, bool reverse) {
    if (name.isEmpty()) {
        Serial.println("[config] addFan: name is empty");
        return -1;
    }
    if (name.length() > MAX_FAN_NAME_LEN) {
        Serial.printf("[config] addFan: name too long (%u > %u)\n",
                      name.length(), MAX_FAN_NAME_LEN);
        return -1;
    }
    if (max_speed < 1 || max_speed > 6) {
        Serial.printf("[config] addFan: invalid max_speed %d\n", max_speed);
        return -1;
    }

    // Auto-increment id
    int newId = 1;
    for (const FanConfig& fan : g_fans) {
        if (fan.id >= newId) newId = fan.id + 1;
    }

    FanConfig fan;
    fan.id        = newId;
    fan.name      = name;
    fan.max_speed = max_speed;
    fan.lights    = lights;
    fan.reverse   = reverse;
    initFanCodes(fan);

    g_fans.push_back(std::move(fan));
    Serial.printf("[config] Added fan id=%d name='%s'\n", newId, name.c_str());
    return newId;
}

bool updateFan(int id, const String* name, const int* max_speed, const bool* lights, const bool* reverse) {
    FanConfig* fan = getFanById(id);
    if (!fan) {
        Serial.printf("[config] updateFan: fan id=%d not found\n", id);
        return false;
    }

    if (name) {
        if (name->isEmpty()) {
            Serial.println("[config] updateFan: name cannot be empty");
            return false;
        }
        if (name->length() > MAX_FAN_NAME_LEN) {
            Serial.printf("[config] updateFan: name too long (%u > %u)\n",
                          name->length(), MAX_FAN_NAME_LEN);
            return false;
        }
        fan->name = *name;
    }
    if (max_speed) {
        if (*max_speed < 1 || *max_speed > 6) {
            Serial.printf("[config] updateFan: invalid max_speed %d\n", *max_speed);
            return false;
        }
        fan->max_speed = *max_speed;
    }
    if (lights) {
        fan->lights = *lights;
    }
    if (reverse) {
        fan->reverse = *reverse;
    }

    Serial.printf("[config] Updated fan id=%d\n", id);
    return true;
}

bool deleteFan(int id) {
    for (auto it = g_fans.begin(); it != g_fans.end(); ++it) {
        if (it->id == id) {
            g_fans.erase(it);
            Serial.printf("[config] Deleted fan id=%d\n", id);
            return true;
        }
    }
    Serial.printf("[config] deleteFan: fan id=%d not found\n", id);
    return false;
}

bool setFanCode(int id, const String& command, uint32_t value,
                uint16_t pulse, uint8_t protocol, uint8_t bits) {
    if (!isValidCommand(command)) {
        Serial.printf("[config] setFanCode: unknown command '%s'\n", command.c_str());
        return false;
    }

    FanConfig* fan = getFanById(id);
    if (!fan) {
        Serial.printf("[config] setFanCode: fan id=%d not found\n", id);
        return false;
    }

    RFCode code;
    code.value = value;
    code.pulse = pulse;
    code.protocol = protocol;
    code.bits = bits;
    fan->codes[command] = code;
    Serial.printf("[config] Set code for fan id=%d command='%s' value=%lu\n",
                  id, command.c_str(), (unsigned long)value);
    return true;
}

const std::vector<FanConfig>& getAllFans() {
    return g_fans;
}

bool clearFanCode(int id, const String& command) {
    if (!isValidCommand(command)) return false;
    FanConfig* fan = getFanById(id);
    if (!fan) return false;
    fan->codes.erase(command);
    Serial.printf("[config] Cleared code for fan id=%d command='%s'\n", id, command.c_str());
    return configSave();
}
