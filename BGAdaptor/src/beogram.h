#pragma once
#include "config.h"

// Beogram serial protocol: command transmission, feedback decoding, and
// the state machine reacting to what the deck reports. Fully
// transport-agnostic — knows nothing about ASE, Mozart, or WiFi.

BeogramFeedback identifyState(const uint8_t* sequence, size_t length);
void sendHexCommand(BeogramCommand command);
void processBuffer(BeogramFeedback state);
void handleSerial1Data();
void sendPlayAfterDelay();
void checkStoppedTrackTimeout();