#include "transport.h"
#include "beogram.h"
#include "halo.h"
#include "ha_mqtt.h"
#include <WiFi.h>
#include <ArduinoJson.h>

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

void sendHttpRequest(const String& endpoint, const String& method, const String& payload) {
    if (productIP.length() == 0) return;
    if (WiFi.status() == WL_CONNECTED) {
        String url = "http://" + productIP + endpoint;
        Serial.println("Sending " + method + " request to: " + url);

        HTTPClient localHttp;  // Use local instance instead of global
        if (localHttp.begin(url)) {
            if (method == "POST") localHttp.addHeader("Content-Type", "application/json");
            int httpResponseCode;
            if (method == "POST") {
                httpResponseCode = payload.isEmpty() ? localHttp.POST("") : localHttp.POST(payload);
            } else {
                httpResponseCode = localHttp.GET();
            }
            Serial.println("HTTP Response code: " + String(httpResponseCode));
            if (httpResponseCode == HTTP_CODE_OK) {
                String response = localHttp.getString();
                handleHttpResponse(endpoint, response);
            }
            localHttp.end();
        } else {
            Serial.println("HTTP begin failed");
        }
    } else {
        Serial.println("WiFi not connected, cannot send request.");
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

void processWebSocketMessage(const String& message) {
    unsigned long currentTime = millis();

    if (message.indexOf("\"eventType\":\"WebSocketEventSourceChange\"") != -1) {
        if (message.indexOf("\"id\":\"" + triggerSource + "\"") != -1) {
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
    } else if (message.indexOf("\"value\":\"networkStandby\"") != -1) {
        playbackState = STOPPED;
        if (haloClient.available()) {
            updateHaloPlayback(false, "");
        }
        sendHexCommand(STANDBY);
        Serial.println("🛑 Standby command detected on websocket. Sent STBY command to Beogram");
    } else if (lineInActive) {
        if (message.indexOf("\"value\":\"started\"") != -1) {
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
        } else if (message.indexOf("\"value\":\"stopped\"") != -1 && playbackState != STOPPED) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                updateHaloPlayback(false);
            }
            Serial.println("⏸️ Product changed state to Stopped. Sent STOP command to Beogram");
        } else if (message.indexOf("\"value\":\"paused\"") != -1) {
            playbackState = PAUSED;
            sendHexCommand(STOP);
            if (haloClient.available()) {
                updateHaloPlayback(false);
            }
            Serial.println("⏸️ Product changed state to Paused. Sent STOP command to Beogram");
        } else if (message.indexOf("\"button\":\"Next\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Sent NEXT command to Beogram");
        } else if (message.indexOf("\"button\":\"Previous\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Sent PREV command to Beogram");
        }
    }
}

void processRemoteWebSocketMessage(const String& message) {
    if (message.indexOf("\"eventType\":\"WebSocketEventBeoRemoteButton\"") != -1 &&
       message.indexOf("\"Type\":\"KeyPress\"") != -1 && lineInActive) {
        if (message.indexOf("\"Key\":\"Wind\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Remote command: NEXT (Wind)");
        } else if (message.indexOf("\"Key\":\"Rewind\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Remote command: PREV (Rewind)");
        } else if (message.indexOf("\"Key\":\"Control/Wind\"") != -1) {
            sendHexCommand(NEXT);
            Serial.println("⏭️ Remote command: Control/Wind");
        } else if (message.indexOf("\"Key\":\"Control/Rewind\"") != -1) {
            sendHexCommand(PREVIOUS);
            Serial.println("⏮️ Remote command: Control/Rewind");
        } else if (message.indexOf("\"Key\":\"Control/Stop\"") != -1) {
            sendHexCommand(STOP);
            Serial.println("⏹️ Remote command: Control/Stop");
        } else if (message.indexOf("\"Key\":\"Control/Play\"") != -1) {
            sendHexCommand(PLAY);
            Serial.println("▶️ Remote command: Control/Play");
        } else if (message.indexOf("\"Key\":\"Control/Digit") != -1) {
            int digitIndex = message.indexOf("\"Key\":\"Control/Digit") + 20;
            char digitChar = message[digitIndex];
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
    }
}

