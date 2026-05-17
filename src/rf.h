#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// GPIO assignments
// ---------------------------------------------------------------------------
static constexpr int RF_RX_PIN = 6;
static constexpr int RF_TX_PIN = 18;

// ---------------------------------------------------------------------------
// RF subsystem API
// ---------------------------------------------------------------------------

/**
 * Initialise the RCSwitch transmitter and receiver.
 * Must be called once in setup() after Serial is started.
 */
void rfInit();

/**
 * Transmit an RF code.
 *
 * @param value     The 32-bit code value.
 * @param pulse     Pulse length in µs (e.g. 350).
 * @param protocol  RCSwitch protocol number (1..12).
 * @param bits      Number of bits (typically 24 or 32).
 * @return true if the send was attempted (RCSwitch does not provide ACK).
 */
bool rfSend(uint32_t value, uint16_t pulse, uint8_t protocol, uint8_t bits);

/**
 * Arm the receiver and wait for a code to arrive.
 *
 * Blocks for up to @p timeoutMs milliseconds, yielding to the RTOS scheduler
 * every millisecond to keep the watchdog fed.
 *
 * @param fanId      For logging purposes only.
 * @param command    For logging purposes only.
 * @param outValue   Set to the received code value on success.
 * @param outPulse   Set to the received pulse length on success.
 * @param outProtocol Set to the received protocol on success.
 * @param outBits    Set to the received bit count on success.
 * @param timeoutMs  How long to wait before giving up (default: 30 000 ms).
 * @return true if a code was captured, false on timeout.
 */
bool rfLearn(const String& fanId, const String& command,
             uint32_t* outValue, uint16_t* outPulse,
             uint8_t*  outProtocol, uint8_t* outBits,
             int timeoutMs = 30000);
