#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/**
 * A single learned RF code for one remote command.
 * value == 0 means the code has not been learned yet.
 */
struct RFCode {
    uint32_t value    = 0;
    uint16_t pulse    = 350;
    uint8_t  protocol = 1;
    uint8_t  bits     = 24;
};

/**
 * Configuration for one fan / RF device.
 */
struct FanConfig {
    int                        id        = 0;
    String                     name;
    int                        max_speed = 3;
    bool                       lights    = false;
    bool                       reverse   = false;
    std::map<String, RFCode>   codes;
};

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

static constexpr size_t MAX_FAN_NAME_LEN = 64;

// ---------------------------------------------------------------------------
// Valid command names
// ---------------------------------------------------------------------------
// All possible command keys regardless of fan capabilities.
// Callers should validate against fan.max_speed and fan.lights before use.
static const char* const ALL_COMMANDS[] = {
    "off",
    "speed_1", "speed_2", "speed_3",
    "speed_4", "speed_5", "speed_6",
    "light_on", "light_off",
    "reverse_on", "reverse_off",
    nullptr
};

// ---------------------------------------------------------------------------
// Config management API
// ---------------------------------------------------------------------------

/**
 * Mount LittleFS and load config.json into memory.
 * Returns true on success.  On failure the in-memory state is an empty fan list.
 */
bool configLoad();

/**
 * Serialise in-memory config to /config.json on LittleFS.
 * Returns true on success.
 */
bool configSave();

/**
 * Build a JsonDocument representation of the full config.
 * Caller owns the returned document.
 */
JsonDocument configToJson();

/**
 * Return a pointer to the fan with the given id, or nullptr if not found.
 * Pointer is valid until the fan list is mutated.
 */
FanConfig* getFanById(int id);

/**
 * Add a new fan.  Auto-increments the id (max existing id + 1, minimum 1).
 * Initialises all command codes to the "not learned" state (value=0).
 * Returns the new fan's id, or -1 on error (e.g. name is empty).
 */
int addFan(const String& name, int max_speed, bool lights, bool reverse);

/**
 * Update mutable fan fields.  Pass nullptr to leave a field unchanged.
 * Returns false if the fan is not found or if the new values are invalid.
 */
bool updateFan(int id, const String* name, const int* max_speed, const bool* lights, const bool* reverse);

/**
 * Delete a fan by id.
 * Returns false if the fan is not found.
 */
bool deleteFan(int id);

/**
 * Set (or overwrite) the RF code for a particular command on a particular fan.
 * Returns false if the fan is not found or the command name is unknown.
 */
bool setFanCode(int id, const String& command, uint32_t value,
                uint16_t pulse, uint8_t protocol, uint8_t bits);

/**
 * Return a const reference to the in-memory fan list.
 * Valid until the list is mutated (add/delete).
 */
const std::vector<FanConfig>& getAllFans();

/**
 * Return true if the given command string is one of ALL_COMMANDS.
 */
bool isValidCommand(const String& command);
