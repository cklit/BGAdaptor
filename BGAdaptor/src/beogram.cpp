#include "beogram.h"
#include "state.h"
#include "transport.h"
#include "halo.h"
#include "ha_mqtt.h"

BeogramFeedback identifyState(const uint8_t* sequence, size_t length) {
    if (debugSerial == true) {
        Serial.print("Identifying state for sequence: ");
        for (size_t i = 0; i < length; ++i) {
            Serial.print(sequence[i], HEX);
            Serial.print(" ");
        }
        Serial.print("Length: ");
        Serial.println(length);
    }
    // A tape deck reports different bytes and has no track numbers, so it
    // gets its own lookup — mapped onto the canonical feedback values so
    // processBuffer() needs no special case.
    if (deviceType == DEVICE_TAPE) {
        if (length == 2 && sequence[0] == sequence[1]) {
            if (sequence[0] == TAPE_PLAYING_FB) return PLAYING_FB;
            if (sequence[0] == TAPE_STOPPED_FB) return STOPPED_FB;
            if (sequence[0] == TAPE_STANDBY_FB) return STANDBY_FB;
        }
        return UNKNOWN_STATE;
    }

    if (length == 5) {
        if (sequence[0] == 0x78 && sequence[4] == 0x7D) return TRACK5;
        if (sequence[0] == 0x78 && sequence[4] == 0x7E) return TRACK6;
        if (sequence[0] == 0x78 && sequence[4] == 0x7C) return TRACK7;  
        if (sequence[0] == 0x78 && sequence[4] == 0x7F) return TRACK13;          
    } else if (length == 2) {
        if (sequence[0] == PLAYING_FB && sequence[1] == PLAYING_FB) return PLAYING_FB;
        if (sequence[0] == STOPPED_FB && sequence[1] == STOPPED_FB) return STOPPED_FB;
        if (sequence[0] == STANDBY_FB && sequence[1] == STANDBY_FB) return STANDBY_FB;
        if (sequence[0] == EJECTED_FB && sequence[1] == EJECTED_FB) return EJECTED_FB;
    } else if (length == 4) {
        if (sequence[0] == 0x78 && sequence[3] == 0x77) return TRACK1;
        if (sequence[0] == 0x78 && sequence[3] == 0x7B) return TRACK2;
        if (sequence[0] == 0x78 && sequence[3] == 0x73) return TRACK3;
        if (sequence[0] == 0x78 && sequence[3] == 0x7D) return TRACK4;    
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x75) return TRACK5;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x79) return TRACK6;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x71) return TRACK7;                        
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x7E) return TRACK8;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x76) return TRACK9;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x7A) return TRACK10;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x72) return TRACK11;
        if (sequence[0] == 0x78 && sequence[2] == 0x78 && sequence[3] == 0x7C) return TRACK12;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && (sequence[3] == 0x1E || sequence[3] == 0x74)) return TRACK13;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && (sequence[3] == 0x78 || sequence[3] == 0xF)) return TRACK14;
        if (sequence[0] == 0x78 && sequence[1] == 0x70 && sequence[3] == 0x70) return TRACK14_PLUS;
    }
    return UNKNOWN_STATE;
}

// Translate a canonical command to the byte this deck actually expects.
// Only the transport commands differ; anything else passes through.
static uint8_t deckCommandByte(BeogramCommand command) {
    if (deviceType != DEVICE_TAPE) return command;
    switch (command) {
        case PLAY:     return TAPE_PLAY;
        case STOP:     return TAPE_STOP;
        case STANDBY:  return TAPE_STANDBY;
        case NEXT:     return TAPE_NEXT;
        case PREVIOUS: return TAPE_PREVIOUS;
        default:       return command;
    }
}

void sendHexCommand(BeogramCommand command) {
    uint8_t byte = deckCommandByte(command);
    Serial1.write(byte);
    delayMicroseconds(49991);
    Serial1.write(byte);
}

void sendPlayAfterDelay() {
    if (waitingForPlay && millis() - delayPlayAfterDigit >= 1200) {
        sendHexCommand(PLAY);           
        waitingForPlay = false; // Reset flag
        Serial.println("▶️ Sent PLAY after 1200ms delay");
    }
}

// Mirror state for the web UI (webpush) — kept separate from the HA
// entities so the page is live even when MQTT isn't configured.
static void setUiState(const char* state, const char* track, int playing) {
    if (state != nullptr) beogramStateText = state;
    if (track != nullptr) beogramTrack = track;
    if (playing >= 0) beogramPlaying = (playing == 1);
    beogramStateDirty = true;
}

// A manual Stop and a natural end-of-disc both report STOPPED_FB, but only
// a manual stop is followed by a track-echo burst (the deck confirming the
// track it's paused on). Defer clearing the track until that window has
// passed with no echo, which means the disc genuinely finished.
static bool stoppedPendingClear = false;
static unsigned long stoppedAt = 0;
static const unsigned long STOPPED_CLEAR_WINDOW_MS = 800;

void checkStoppedTrackTimeout() {
    if (stoppedPendingClear && millis() - stoppedAt >= STOPPED_CLEAR_WINDOW_MS) {
        stoppedPendingClear = false;
        setUiState(nullptr, "-", -1);
        if (mqtt.isConnected()) {
            bgTrack.setValue("-");
        }
    }
}

void processBuffer(BeogramFeedback state) {
    if (state == PLAYING_FB) {
        playbackState = PLAYING;
        stoppedPendingClear = false;
        setUiState("Playing", nullptr, 1);
        Serial.println("▶️ Beogram reported ON state.");
        if (mqtt.isConnected()) {
            bgPlaybackState.setValue("Playing");
            bgPlaying.setState(true);
        }
        if (haloClient.available()) {
            updateHaloPlayback(true);
        }       
        if (platform == PLATFORM_MOZART) {
            if (!lineInActive) {
                sendHttpRequest("/api/v1/playback/sources/active/" + triggerSource, "POST");
            } else {
                sendHttpRequest("/api/v1/playback/command/play", "POST");
            }
        } else {
            if (!lineInActive) {
                forceSource();
            }
        }
    } else if (state == STOPPED_FB || state == STANDBY_FB) {
        Serial.println(state == STOPPED_FB ? "Beogram reported OFF state." : "Beogram reported STANDBY state.");
<<<<<<< HEAD
        // Stop is really a pause: keep the last track shown for now. If no
        // track-echo follows within the window, checkStoppedTrackTimeout()
        // clears it (that means the disc actually finished). Standby
        // clears it immediately.
        setUiState(state == STOPPED_FB ? "Stopped" : "Standby", state == STOPPED_FB ? nullptr : "-", 0);
        if (state == STOPPED_FB) {
            stoppedPendingClear = true;
            stoppedAt = millis();
        } else {
            stoppedPendingClear = false;
        }
||||||| parent of 9ffbfe0 (New function: Auto-expand)
        setUiState(state == STOPPED_FB ? "Stopped" : "Standby", "-", 0);
=======
        // Stop is really a pause: keep the last track shown for now. If no
        // track-echo follows within the window, checkStoppedTrackTimeout()
        // clears it (that means the disc actually finished). Standby
        // clears it immediately.
        setUiState(state == STOPPED_FB ? "Stopped" : "Standby", state == STOPPED_FB ? nullptr : "-", 0);
        // Secondary playback speaker: release on standby only. Stop is really a pause,
        // and a lifted tonearm is never reported at all, so standby is the
        // one unambiguous "finished" event across every deck type.
        if (state == STANDBY_FB) unexpandPlaybackSpeaker();
        if (state == STOPPED_FB) {
            stoppedPendingClear = true;
            stoppedAt = millis();
        } else {
            stoppedPendingClear = false;
        }
>>>>>>> 9ffbfe0 (New function: Auto-expand)
        if (mqtt.isConnected()) {
            if (state == STANDBY_FB) {
                bgTrack.setValue("-");
            }
            bgPlaybackState.setValue(state == STOPPED_FB ? "Stopped" : "Standby");
            bgPlaying.setState(false);
        }
        if (playbackState == PLAYING && lineInActive) {
            playbackState = STOPPED;
            Serial.println(state == STOPPED_FB ? "⏹️ Beogram has stopped." : "⏹️ Beogram has turned off.");
            if (platform == PLATFORM_MOZART) {
                sendHttpRequest("/api/v1/playback/command/stop", "POST");
            }
            if (haloClient.available()) {
                updateHaloPlayback(false, " ");
            }
        }        
    } else if (state == EJECTED_FB) {
        playbackState = STOPPED;
        stoppedPendingClear = false;
        Serial.println("⏏️ Beogram tray was ejected");
        setUiState("Ejected", "-", 0);
        if (mqtt.isConnected()) {
            bgTrack.setValue("-");
            bgPlaybackState.setValue("Ejected"); 
            bgPlaying.setState(false);    
        }
        if (haloClient.available()) {
            updateHaloPlayback(false, "Tray ejected");  
        }            
        if (platform == PLATFORM_MOZART && lineInActive) {
            sendHttpRequest("/api/v1/playback/command/stop", "POST");
        }
    } else if (state == TRACK14_PLUS && (playbackState == PLAYING || stoppedPendingClear)) {
        stoppedPendingClear = false;
        Serial.print("Track identified: ");
        Serial.println("14+");
        setUiState(nullptr, "14+", -1);
        if (mqtt.isConnected()) {
            bgTrack.setValue("14+");
        }
        if (haloClient.available()) {
            updateHaloSubtitle("Track 14+");
        }
    } else if (state != UNKNOWN_STATE && (playbackState == PLAYING || stoppedPendingClear)) {
        stoppedPendingClear = false;
        Serial.print("Track identified: ");
        Serial.println(state, DEC);
        if (haloClient.available()) {
            char subtitle[20];
            sprintf(subtitle, "Track %d", state);
            updateHaloSubtitle(subtitle);
        }
        char trackNumber[20];
        sprintf(trackNumber, "%d", state);
        setUiState(nullptr, trackNumber, -1);
        if (mqtt.isConnected()) {
            bgTrack.setValue(trackNumber);
        }
    }
}

void handleSerial1Data() {
    static uint8_t buffer[5];
    static size_t bufferIndex = 0;
    static unsigned long lastByteTime = 0;

    while (Serial1.available()) {
        uint8_t receivedByte = Serial1.read();
        unsigned long currentTime = millis();

        if (debugSerial == true) {
          Serial.print("Received byte: 0x");
          Serial.println(receivedByte, HEX);
        }

        // Add bounds check before storing
        if (bufferIndex < sizeof(buffer)) {
            buffer[bufferIndex++] = receivedByte;
        } else {
            // Buffer overflow - reset and log
            Serial.println("Serial buffer overflow - resetting");
            bufferIndex = 0;
            buffer[bufferIndex++] = receivedByte;
        }

        lastByteTime = currentTime;

        if (bufferIndex == 5) {
            BeogramFeedback state = identifyState(buffer, bufferIndex);
            processBuffer(state);
            bufferIndex = 0;  
        }
    }

    // Check if 55 ms have passed since the last byte was received
    if (millis() - lastByteTime > 55 && bufferIndex > 0) {
        BeogramFeedback state = identifyState(buffer, bufferIndex);
        processBuffer(state);
        bufferIndex = 0;  
    }
}
