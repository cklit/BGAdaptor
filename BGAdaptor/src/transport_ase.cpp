#include "transport.h"
#include "beogram.h"
#include "halo.h"
#include "ha_mqtt.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <lwip/sockets.h>

// Expand/unexpand the ASE product's primary experience to a listener.
//   expand:   POST   /BeoZone/Zone/ActiveSources/primaryExperience  {"listener":{"jid":...}}
//   unexpand: DELETE /BeoZone/Zone/ActiveSources/primaryExperience/?jid=...
void aseBeolink(bool expand) {
    if (productIP.length() == 0 || playbackJid.length() == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;

    String base = "http://" + productIP + ":" + String(SSE_PORT) +
                  "/BeoZone/Zone/ActiveSources/primaryExperience";
    HTTPClient localHttp;
    int code;

    if (expand) {
        localHttp.begin(base);
        localHttp.addHeader("Content-Type", "application/json");
        code = localHttp.POST("{\"listener\":{\"jid\":\"" + playbackJid + "\"}}");
        Serial.println("Expanding to " + playbackName + " — status: " + String(code));
    } else {
        localHttp.begin(base + "/?jid=" + playbackJid);
        code = localHttp.sendRequest("DELETE");
        Serial.println("Unexpanding " + playbackName + " — status: " + String(code));
    }
    localHttp.end();
}

void forceSource() {
    if (productIP.length() == 0) return;   // nothing linked — don't build a bogus URL
    if (WiFi.status() == WL_CONNECTED) {
        String payload = "{\"sourceType\":{\"type\":\"" + triggerSource + "\"}}";
        String url = "http://" + productIP + ":" + String(SSE_PORT) + "/BeoZone/Zone/ActiveSourceType";
        Serial.println("Activating Line-In on the product.");  
        HTTPClient httpForce;
        httpForce.begin(url);
        httpForce.addHeader("Content-Type", "application/json");
        int code = httpForce.POST(payload);
        Serial.println("Status: " + String(code));
        httpForce.end();
    }
}

// The SSE stream is receive-only, so if the product reboots or drops off
// the network without closing the socket, the ESP32 never sends anything
// that would reveal the peer is gone: the TCP connection sits half-open,
// sseClient.connected() keeps returning true, and no notifications ever
// arrive again. TCP keepalive makes the stack probe the peer itself, so a
// dead connection is detected in ~25 s and the normal reconnect kicks in.
static void enableTcpKeepAlive(WiFiClient& client) {
    int fd = client.fd();
    if (fd < 0) return;
    int enable = 1;
    int idle = 10;      // seconds of silence before the first probe
    int interval = 5;   // seconds between probes
    int count = 3;      // unanswered probes before the connection is declared dead
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &enable,   sizeof(enable));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,     sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &count,    sizeof(count));
}

void connectToSSE() {
    if (sseClient.connected()) return;  // Avoid reconnecting if already connected

    Serial.printf("Connecting to SSE stream: %s\n", productIP.c_str());
    sseClient.stop();  // Ensure the previous client is closed

    if (!sseClient.connect(productIP.c_str(), SSE_PORT)) {
        Serial.println("Connection to server failed!");
        return;
    }
    enableTcpKeepAlive(sseClient);
    sseLastDataReceived = millis();

    sseClient.println("GET /BeoNotify/Notifications HTTP/1.1");
    sseClient.println("Host: " + productIP + ":" + String(SSE_PORT));
    sseClient.println("Accept: text/event-stream");
    sseClient.println("Connection: keep-alive");
    sseClient.println();

    Serial.println("Connected to SSE stream!");
}

void checkSSEConnection() {
    // Safety net on top of TCP keepalive: if the stream has been silent for a
    // long time, drop it and reconnect. Cheap on a healthy product (one GET).
    if (sseClient.connected() && millis() - sseLastDataReceived > SSE_IDLE_TIMEOUT_MS) {
        Serial.println("SSE stream silent too long — reconnecting");
        sseClient.stop();
    }

    if (!sseClient.connected()) {
        unsigned long now = millis();

        // Ensure Wi-Fi is connected before attempting SSE reconnect
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Wi-Fi disconnected. Waiting for reconnection...");
            return;
        }

        // Wait for the delay before retrying
        if (now - sseLastReconnectAttempt >= sseReconnectDelay) {
            Serial.println("SSE stream lost. Reconnecting...");
            sseLastReconnectAttempt = now;
            connectToSSE();

            // Implement exponential backoff (double the delay up to a max of 8s)
            sseReconnectDelay = min(sseReconnectDelay * 2, 8000UL);
        }
    } else {
        // Reset delay if connection is stable
        sseReconnectDelay = 1000;
    }
}

void processSSE(String message) {
    if (message.startsWith("data: ")) {
        message = message.substring(6);
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        Serial.print("JSON parse error: ");
        Serial.println(error.c_str());
        return;
    }

    JsonObject notification = doc["notification"];
    if (notification.isNull()) return;

    String type = notification["type"].as<String>();
    JsonObject data = notification["data"];
    if (data.isNull()) return;

    // Handle CONTROL commands
    if (type == "COMMAND") {
        String category = data["category"].as<String>();
        String key = data["key"].as<String>();
        String event = data["event"].as<String>();
        
        if (lineInActive && category == "Control" && event == "KeyPress") {
            if (key == "Play") {
                Serial.println("✅ Received Control/Play!");
                playbackState = PLAYING;
                sendHexCommand(PLAY);
                Serial.println("Sent PLAY command to Beogram");
                if (haloClient.available()) {
                    updateHaloPlayback(true);
                } 
            } else if (key == "Stop") {
                Serial.println("✅ Received Control/Stop!");
                playbackState = PAUSED;
                sendHexCommand(STOP);
                if (haloClient.available()) {
                    updateHaloPlayback(false); 
                }                
                Serial.println("Sent STOP command to Beogram");
            } else if (key == "Wind") {
                Serial.println("✅ Received Control/Wind!");
                sendHexCommand(NEXT);
                Serial.println("Sent NEXT command to Beogram");
            } else if (key == "Rewind") {
                Serial.println("✅ Received Control/Rewind!");
                sendHexCommand(PREVIOUS);
                Serial.println("Sent PREV command to Beogram");
            } else if (key.length() == 1 && isDigit(key[0])) {
                const BeogramCommand digitCommands[10] = {
                    DIGIT0, DIGIT1, DIGIT2, DIGIT3, DIGIT4, DIGIT5, DIGIT6, DIGIT7, DIGIT8, DIGIT9
                };
                BeogramCommand digitCommand = digitCommands[key[0] - '0'];
                sendHexCommand(OPEN_FOR_DIGIT);
                delay(50); 
                sendHexCommand(digitCommand);

                // Start the non-blocking delay
                delayPlayAfterDigit = millis();
                waitingForPlay = true;
                Serial.printf("🔢 Sent Digit %c\n", key[0]);               
            }
        }
    }

    if (type == "SOURCE" || type == "SOURCE_EXPERIENCE_CHANGED") {
        if (data.size() == 0) {
            Serial.println("🛑 Standby mode detected (empty SOURCE data).");
            lineInActive = false;
            speakerExpanded = false;   // product left the source; any expansion is gone
            playbackState = STOPPED;
            sendHexCommand(STANDBY);
            if (haloClient.available()) {
                updateHaloPlayback(false, "");  
            }                    
            Serial.println("Sent STBY command to Beogram");
        } else {
            JsonObject primaryExperience = data["primaryExperience"];
            if (primaryExperience.isNull()) return;

            JsonObject source = primaryExperience["source"];
            if (source.isNull()) return;

            String sourceType = source["sourceType"]["type"].as<String>();

            if (sourceType == triggerSource && !lineInActive) { //to avoid "re-activating" Line-in on SSE reconnect
                Serial.println("✅ Line-in activated!");
                lineInActive = true;
                // The product is now on the trigger source, so there is an
                // experience to expand. Doing this any earlier is a no-op.
                expandToPlaybackSpeaker();
                haloActionTime = millis();  // Store the current time  
                if (haloControls) {
                    haloUpdate = PAGE;
                }           
                if (playbackState != PLAYING) {
                    sendHexCommand(PLAY);
                    playbackState = PLAYING;
                    Serial.println("Sent PLAY command to Beogram");
                }
            } else if (sourceType != triggerSource) {
                Serial.println("❌ Line-in deactivated!");
                lineInActive = false;
                speakerExpanded = false;   // product left the source; any expansion is gone
                if (haloClient.available()) {
                    updateHaloPlayback(false, "");  
                }                             
                if (playbackState == PLAYING) {
                    playbackState = PAUSED;
                    sendHexCommand(STOP);    
                    Serial.println("Sent STOP command to Beogram to Pause playback.");
                }
            }
        }
    }
}

void readSSE() {
    static String lineBuffer = "";
    const size_t MAX_SSE_LINE = 2048;  // Reasonable max for SSE data

    while (sseClient.available()) {
        char c = sseClient.read();
        sseLastDataReceived = millis();
        if (c == '\n') {
            lineBuffer.trim();
            if (lineBuffer.length() > 0 && (lineBuffer.startsWith("data: ") || lineBuffer.startsWith("{"))) {
                processSSE(lineBuffer);
            }
            lineBuffer = "";
        } else {
            lineBuffer += c;
            // Add bounds check
            if (lineBuffer.length() >= MAX_SSE_LINE) {
                Serial.println("SSE buffer overflow - resetting");
                lineBuffer = "";
            }
        }
    }
}


