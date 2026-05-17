#include "rf.h"
#include <RCSwitch.h>

// ---------------------------------------------------------------------------
// Module-level RCSwitch instance
// ---------------------------------------------------------------------------

static RCSwitch g_rc;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void rfInit() {
    // Transmitter: enable on TX pin
    g_rc.enableTransmit(RF_TX_PIN);

    // Set sensible defaults for the transmitter; these are overridden per-send
    g_rc.setProtocol(1);
    g_rc.setPulseLength(350);
    g_rc.setRepeatTransmit(3);  // send each code 3 times for reliability

    // Receiver: enable on RX pin with interrupt
    // RCSwitch uses attachInterrupt internally; the pin must support interrupts.
    g_rc.enableReceive(RF_RX_PIN);

    Serial.printf("[rf] Initialised — TX GPIO%d, RX GPIO%d\n", RF_TX_PIN, RF_RX_PIN);
}

bool rfSend(uint32_t value, uint16_t pulse, uint8_t protocol, uint8_t bits) {
    if (value == 0) {
        Serial.println("[rf] rfSend: refusing to send code value=0 (not learned)");
        return false;
    }

    Serial.printf("[rf] Sending value=%lu pulse=%u protocol=%u bits=%u\n",
                  (unsigned long)value, pulse, (unsigned)protocol, (unsigned)bits);

    // Disable receiver during transmission to avoid self-reception
    g_rc.disableReceive();

    g_rc.setProtocol(protocol);
    g_rc.setPulseLength(pulse);
    g_rc.send(value, bits);

    // Re-enable receiver
    g_rc.enableReceive(RF_RX_PIN);

    Serial.println("[rf] Send complete");
    return true;
}

bool rfLearn(const String& fanId, const String& command,
             uint32_t* outValue, uint16_t* outPulse,
             uint8_t*  outProtocol, uint8_t* outBits,
             int timeoutMs) {

    Serial.printf("[rf] Learning: fan='%s' command='%s' timeout=%dms\n",
                  fanId.c_str(), command.c_str(), timeoutMs);

    // Clear any previously captured signal
    g_rc.resetAvailable();

    const unsigned long start = millis();

    while ((millis() - start) < static_cast<unsigned long>(timeoutMs)) {
        if (g_rc.available()) {
            *outValue    = static_cast<uint32_t>(g_rc.getReceivedValue());
            *outPulse    = static_cast<uint16_t>(g_rc.getReceivedDelay());
            *outProtocol = static_cast<uint8_t>(g_rc.getReceivedProtocol());
            *outBits     = static_cast<uint8_t>(g_rc.getReceivedBitlength());

            g_rc.resetAvailable();

            if (*outValue == 0) {
                // RCSwitch reports value=0 for failed decodes; keep waiting
                Serial.println("[rf] Received value=0, ignoring (decode error)");
                continue;
            }

            Serial.printf("[rf] Captured value=%lu pulse=%u protocol=%u bits=%u\n",
                          (unsigned long)*outValue, *outPulse,
                          (unsigned)*outProtocol, (unsigned)*outBits);
            return true;
        }

        // Yield to keep FreeRTOS watchdog happy
        delay(1);
    }

    Serial.println("[rf] Learn timed out");
    return false;
}
