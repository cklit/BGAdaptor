#include "transport.h"
#include "beogram.h"
#include "halo.h"
#include "ha_mqtt.h"
#include <WiFi.h>
#include <ArduinoJson.h>

static int mozartVolumeLevel = -1;
static int mozartVolumeMaximum = 100;

void handleHttpResponse(const String& endpoint, const String& response) {
    if (endpoint == "/api/v1/playback/state") {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, response);
        if (!error) {
            String source = doc["source"]["id"].as<String>();
            String state = doc["state"]["value"].as<String>();
            if (source == triggerSource && state == "started" && haloClient.available()) {
                updateHaloPlayback(true);
                lineInActive = true;
                playbackState = PLAYING;
                Serial.println("Polled Playing state from product");
                expandToPlaybackSpeaker();   // idempotent — may also fire from the websocket
            } else {
                updateHaloPlayback(false);
                Serial.println("Polled Stopped state from product");
            }
        } else {
            Serial.println("JSON parsing failed!");
        }
    }
}

bool sendHttpRequest(const String& endpoint, const String& method, const String& payload) {
    if (productIP.length() == 0) return false;
    if (WiFi.status() == WL_CONNECTED) {
        String url = "http://" + productIP + endpoint;
        Serial.println("Sending " + method + " request to: " + url);

        HTTPClient localHttp;  // Use local instance instead of global
        if (localHttp.begin(url)) {
            if (method == "POST" || method == "PUT") localHttp.addHeader("Content-Type", "application/json");
            int httpResponseCode;
            if (method == "POST") {
                httpResponseCode = payload.isEmpty() ? localHttp.POST("") : localHttp.POST(payload);
            } else if (method == "PUT") {
                httpResponseCode = localHttp.sendRequest("PUT", payload);
            } else {
                httpResponseCode = localHttp.GET();
            }
            Serial.println("HTTP Response code: " + String(httpResponseCode));
            if (httpResponseCode >= 200 && httpResponseCode < 300) {
                String response = localHttp.getString();
                handleHttpResponse(endpoint, response);
            }
            localHttp.end();
            return httpResponseCode >= 200 && httpResponseCode < 300;
        } else {
            Serial.println("HTTP begin failed");
        }
    } else {
        Serial.println("WiFi not connected, cannot send request.");
    }
    return false;
}

void mozartAdjustVolume(int delta) {
    if ((delta != 1 && delta != -1) || productIP.length() == 0 ||
        mozartVolumeLevel < 0 || WiFi.status() != WL_CONNECTED) return;

    int level = constrain(mozartVolumeLevel + delta, 0, mozartVolumeMaximum);
    if (level == mozartVolumeLevel) return;

    bool success = sendHttpRequest("/api/v1/sound/volume/level", "PUT",
                                   "{\"level\":" + String(level) + "}");
    if (success) {
        mozartVolumeLevel = level;
        updateHaloVolume(mozartVolumeLevel, 0, mozartVolumeMaximum);
    }
}

// Reconnect both Mozart WebSockets only if not already connected
// Expand/unexpand the Mozart product's Beolink experience to a listener.
//   POST /api/v1/beolink/expand/{jid}
//   POST /api/v1/beolink/unexpand/{jid}
void mozartBeolink(bool expand) {
    if (playbackJid.length() == 0) return;
    Serial.println(String(expand ? "Expanding to " : "Unexpanding ") + playbackName);
    sendHttpRequest(String("/api/v1/beolink/") + (expand ? "expand/" : "unexpand/") + playbackJid, "POST");
}

void checkWebSocketConnection() {
    if (productIP.length() == 0) return;   // nothing to connect to
    if (millis() - wsLastReconnectAttempt <= wsReconnectDelay) return;
    wsLastReconnectAttempt = millis();

    bool allConnected = true;

    if (!wsClient.available()) {
        Serial.println("Reconnecting product websocket...");
        if (wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str())) {
            wsClient.send("Hi Server!");
            Serial.println("Product webSocket reconnected!");
        } else {
            Serial.println("Product webSocket reconnection failed.");
            allConnected = false;
        }
    }
    if (!remoteClient.available()) {
        Serial.println("Reconnecting remote websocket...");
        if (remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str())) {
            remoteClient.send("Hi Server!");
            Serial.println("Secondary websocket reconnected!");
        } else {
            Serial.println("Remote webSocket reconnection failed.");
            allConnected = false;
        }
    }

    // Each failed connect() blocks for seconds, so an unreachable product
    // must be retried progressively less often — otherwise the loop stalls.
    if (allConnected) {
        wsReconnectDelay = reconnectInterval;
    } else {
        wsReconnectDelay = min(wsReconnectDelay * 2, reconnectMaxInterval);
        Serial.println("Next product retry in " + String(wsReconnectDelay / 1000) + "s");
    }
}

// Mozart's websocket message shapes aren't officially documented, and the
// exact nesting depth of most fields below was inferred rather than
// confirmed against a live product. These helpers search for a key/value
// pair anywhere in the parsed document instead of assuming a specific
// depth, so they match whatever the previous indexOf-based checks matched
// without hard-coding a possibly-wrong path — and they're immune to
// whitespace/formatting changes, unlike a literal substring search.
static bool jsonKeyEquals(JsonVariantConst node, const char* key, const char* value) {
    if (node.is<JsonObjectConst>()) {
        JsonObjectConst obj = node.as<JsonObjectConst>();
        if (obj[key] == value) return true;
        for (JsonPairConst kv : obj) {
            if (jsonKeyEquals(kv.value(), key, value)) return true;
        }
    } else if (node.is<JsonArrayConst>()) {
        for (JsonVariantConst item : node.as<JsonArrayConst>()) {
            if (jsonKeyEquals(item, key, value)) return true;
        }
    }
    return false;
}

// Finds the first string value for `key` anywhere in the document.
static bool jsonFindKeyString(JsonVariantConst node, const char* key, String& out) {
    if (node.is<JsonObjectConst>()) {
        JsonObjectConst obj = node.as<JsonObjectConst>();
        JsonVariantConst v = obj[key];
        if (v.is<const char*>()) {
            out = v.as<const char*>();
            return true;
        }
        for (JsonPairConst kv : obj) {
            if (jsonFindKeyString(kv.value(), key, out)) return true;
        }
    } else if (node.is<JsonArrayConst>()) {
        for (JsonVariantConst item : node.as<JsonArrayConst>()) {
            if (jsonFindKeyString(item, key, out)) return true;
        }
    }
    return false;
}

void processWebSocketMessage(const String& message) {
    unsigned long currentTime = millis();

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        // Unlike the old substring scan, a message that isn't valid JSON is
        // now dropped rather than silently mis-scanned — log it so a real
        // format change is visible instead of just "nothing happened".
        Serial.println("⚠️ Mozart websocket message failed to parse as JSON: " + message);
        return;
    }

    if (doc["eventType"] == "WebSocketEventVolume") {
        JsonObject level = doc["eventData"]["level"];
        JsonObject maximum = doc["eventData"]["maximum"];
        if (level["level"].is<int>()) {
            mozartVolumeLevel = level["level"].as<int>();
            if (maximum["level"].is<int>()) {
                mozartVolumeMaximum = maximum["level"].as<int>();
            }
            updateHaloVolume(mozartVolumeLevel, 0, mozartVolumeMaximum);
        }
        return;
    }

    if (jsonKeyEquals(doc, "eventType", "WebSocketEventSourceChange")) {
        if (jsonKeyEquals(doc, "id", triggerSource.c_str())) {
            lineInActive = true;
            Serial.println("✅ Line-in activated");
            // The product is now on the trigger source, so there is an
            // experience to expand. Doing this any earlier is a no-op.
            expandToPlaybackSpeaker();
            haloActionTime = millis();
            if (haloControls) haloUpdate = PAGE;
        } else {
            lineInActive = false;
            speakerExpanded = false;   // product left the source; any expansion is gone
            expandDueAt = 0;
            Serial.println("❌ Source changed, Line-in deactivated");
            if (playbackState == PLAYING) {
                playbackState = PAUSED;
                sendHexCommand(STOP);
                Serial.println("⏹️ Sent STOP command to Beogram to Pause playback.");
                if (haloClient.available()) {
                    updateHaloPlayback(false, "");
                }
            }
        }
    } else if (jsonKeyEquals(doc, "value", "networkStandby")) {
        playbackState = STOPPED;
        clearBeogramTrack();
        if (haloClient.available()) {
            // A blank string is silently ignored by the Halo — a lone
            // space is what actually blanks the subtitle (see beogram.cpp).
            updateHaloPlayback(false, " ");
        }
        sendHexCommand(STANDBY);
        Serial.println("🛑 Standby command detected on websocket. Sent STBY command to Beogram");
    } else if (lineInActive) {
        if (jsonKeyEquals(doc, "value", "started")) {
            // This is Mozart's own confirmation that our source is actually
            // playing — it won't join a speaker to the experience before
            // this, whether the resume was initiated here (Beogram Play,
            // with no source-change event to trigger the usual expand) or
            // from the product's own remote. Idempotent, so safe outside the
            // debounce below, which exists only to avoid resending PLAY.
            expandToPlaybackSpeaker();
            if (currentTime - lastStartEventTime > stateDebounceDelay) {
                lastStartEventTime = currentTime;
                if (playbackState != PLAYING) {
                    sendHexCommand(PLAY);
                    if (haloClient.available()) {
                        updateHaloPlayback(true);
                    }
                    Serial.println("▶️ Product changed state to Play from Pause or Standby. Sent PLAY command to Beogram");
                }
            }
        } else if (jsonKeyEquals(doc, "value", "stopped") && playbackState != STOPPED) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                updateHaloPlayback(false);
            }
            Serial.println("⏸️ Product changed state to Stopped. Sent STOP command to Beogram");
        } else if (jsonKeyEquals(doc, "value", "paused")) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                updateHaloPlayback(false);
            }
            Serial.println("⏸️ Product changed state to Paused. Sent STOP command to Beogram");
        } else if (jsonKeyEquals(doc, "button", "Next")) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Sent NEXT command to Beogram");
        } else if (jsonKeyEquals(doc, "button", "Previous")) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Sent PREV command to Beogram");
        }
        // "Unrecognized message" logging temporarily disabled to test
        // whether the serial write itself was contributing to slow
        // websocket feedback.
    }
}

void processRemoteWebSocketMessage(const String& message) {
    if (!lineInActive) return;

    JsonDocument doc;
    // The remote socket also carries non-button frames (connection acks,
    // etc) — those aren't JSON button events, so just drop them quietly.
    if (deserializeJson(doc, message)) return;

    if (!jsonKeyEquals(doc, "eventType", "WebSocketEventBeoRemoteButton") ||
        !jsonKeyEquals(doc, "Type", "KeyPress")) {
        return;
    }

    String key;
    if (!jsonFindKeyString(doc, "Key", key)) return;

    if (key == "Wind") {
        sendHexCommand(NEXT);
        Serial.println("⏭️ Remote command: NEXT (Wind)");
    } else if (key == "Rewind") {
        sendHexCommand(PREVIOUS);
        Serial.println("⏮️ Remote command: PREV (Rewind)");
    } else if (key == "Control/Wind") {
        sendHexCommand(NEXT);
        Serial.println("⏭️ Remote command: Control/Wind");
    } else if (key == "Control/Rewind") {
        sendHexCommand(PREVIOUS);
        Serial.println("⏮️ Remote command: Control/Rewind");
    } else if (key == "Control/Stop") {
        sendHexCommand(STOP);
        Serial.println("⏹️ Remote command: Control/Stop");
    } else if (key == "Control/Play") {
        sendHexCommand(PLAY);
        Serial.println("▶️ Remote command: Control/Play");
    } else if (key.startsWith("Control/Digit")) {
        // Reads the trailing digit character directly from the matched key
        // instead of indexing a fixed offset into the raw message, so this
        // no longer breaks if the "Control/Digit" prefix length ever changes.
        char digitChar = key.charAt(key.length() - 1);
        if (isdigit(digitChar)) {
            const BeogramCommand digitCommands[10] = {
                DIGIT0, DIGIT1, DIGIT2, DIGIT3, DIGIT4,
                DIGIT5, DIGIT6, DIGIT7, DIGIT8, DIGIT9
            };
            BeogramCommand digitCommand = digitCommands[digitChar - '0'];
            sendHexCommand(OPEN_FOR_DIGIT);
            delay(50);
            sendHexCommand(digitCommand);
            delayPlayAfterDigit = millis();
            waitingForPlay = true;
            Serial.printf("🔢 Sent Digit %c\n", digitChar);
        }
    }
    // "Unrecognized Key" logging temporarily disabled to test whether the
    // serial write itself was contributing to slow websocket feedback.
}

