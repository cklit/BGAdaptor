#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>

#include "config.h"
#include "state.h"
#include "beogram.h"
#include "transport.h"
#include "halo.h"
#include "ha_mqtt.h"
#include "discovery.h"
#include "webui.h"
#include "led.h"
#include "webpush.h"

WiFiManager wm;

void checkWiFiConnection() {
    static unsigned long lastWifiAttempt = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > 10000) {
        lastWifiAttempt = millis();
        Serial.println("WiFi lost, attempting to reconnect...");
        WiFi.reconnect();
    }
}

void checkPingWebsocket() {
    if (platform == PLATFORM_MOZART) {
        if (wsClient.available() && productIP.length() > 0) {    
            if (millis() - wsLastPingReceived >= pingTimeout) {
                wsClient.ping();
                wsLastPingReceived = millis();
            }
        } else if (!wsClient.available() && productIP.length() > 0) {    
            if (millis() - wsLastPingReceived >= pingTimeout) {
                wsClient.close();
                checkWebSocketConnection();
            }
        }

        if (remoteClient.available() && productIP.length() > 0) {    
            if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
                remoteClient.ping();
                wsRemoteLastPingReceived = millis();
            }
        } else if (!remoteClient.available() && productIP.length() > 0) {    
            if (millis() - wsRemoteLastPingReceived >= pingTimeout) {
                remoteClient.close();
                checkWebSocketConnection();
            }
        }
    }

    if (haloClient.available()) {
        if (millis() - haloLastPingReceived >= pingTimeout) {
            haloClient.ping();
            haloLastPingReceived = millis();
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(320, SERIAL_7N1, RXD2, TXD2, true);

    pixels.begin();
    pixels.clear();
    pixels.setPixelColor(0, pixels.Color(0, 2, 0));  // Red for WiFi issue

    WiFi.mode(WIFI_STA);

    bool res = wm.autoConnect(AP_SSID, AP_PASSWORD);
    if (!res) {
        Serial.println("Failed to connect to WiFi. Retrying...");
        for (int i = 0; i < 5; i++) {
            Serial.println("Retrying WiFi...");
            WiFi.begin();
            delay(5000);
            if (WiFi.status() == WL_CONNECTED) break;
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Could not connect to WiFi. Restarting ESP32...");
            ESP.restart();
        }
    } else {
        Serial.println("Connected to WiFi!");
    }

    // Disable WiFi modem power save. With power save on, the ESP32 sleeps
    // between beacons and drops many multicast frames — the main cause of
    // mDNS discovery intermittently missing products. Also improves the
    // latency of incoming SSE/websocket/MQTT traffic.
    WiFi.setSleep(false);

    // Cap a single connect() attempt. Without this an unreachable host
    // blocks the loop for ~3 s per try, stalling the web UI and serial.
    wifi.setTimeout(2);        // MQTT
    sseClient.setTimeout(2);   // ASE notification stream

    if (!MDNS.begin(DEVICE_NAME)) {
    Serial.println("Error setting up MDNS responder!");
    while (1) {
        delay(1000);
        }
    }
    Serial.println("mDNS responder started");

    // ── Preferences & platform resolution ───────────────────────────
    preferences.begin("beogramadaptor", false);

    String platformStr = preferences.getString("platform", "");
    if (platformStr == "") {
        // Migration from single-platform firmware: infer platform from
        // which legacy IP key is populated, and carry the IP over.
        String legacyWs = preferences.getString("wsIP", "");
        String legacySse = preferences.getString("sseIP", "");
        if (legacyWs.length() > 0) {
            platformStr = "mozart";
            preferences.putString("productIP", legacyWs);
            Serial.println("Migrated Mozart install (wsIP → productIP)");
        } else if (legacySse.length() > 0) {
            platformStr = "ase";
            preferences.putString("productIP", legacySse);
            Serial.println("Migrated ASE install (sseIP → productIP)");
        } else {
            platformStr = "ase";  // fresh install default
        }
        preferences.putString("platform", platformStr);
    }
    platform = (platformStr == "mozart") ? PLATFORM_MOZART : PLATFORM_ASE;
    Serial.println(String("Platform: ") + (platform == PLATFORM_MOZART ? "Mozart" : "ASE"));

    productIP = preferences.getString("productIP", "");
    productSerial = preferences.getString("productSerial", "");
    productName = preferences.getString("productName", "");
    playbackJid = preferences.getString("playbackJid", "");
    playbackName = preferences.getString("playbackName", "");
    playbackSerial = preferences.getString("playbackSerial", "");
    haloIP = preferences.getString("haloIP", "");
    haloSerial = preferences.getString("haloSerial", "");
    haloControls = preferences.getBool("feature_enabled", false);
    haloPlayIcon = preferences.getBool("haloPlayIcon", false);
    String storedDeck = preferences.getString("deviceType", "cd");
    deviceType = (storedDeck == "record") ? DEVICE_RECORD
               : (storedDeck == "tape")   ? DEVICE_TAPE
                                          : DEVICE_CD;
    mqttIP = preferences.getString("mqttIP", "");
    mqttUser = preferences.getString("mqttUser", "");
    mqttPassword = preferences.getString("mqttPassword", "");
    triggerSource = preferences.getString("triggerSource", platform == PLATFORM_MOZART ? "lineIn" : "LINE IN");    

    // ── Home Assistant / MQTT device ────────────────────────────────
    WiFi.macAddress(mac);
    String macSuffix = macToUnderscoreString(mac, sizeof(mac));

    snprintf(idPlay,     sizeof(idPlay),     "beogramPlay_%s",     macSuffix.c_str());
    snprintf(idNext,     sizeof(idNext),     "beogramNext_%s",     macSuffix.c_str());
    snprintf(idPrev,     sizeof(idPrev),     "beogramPrev_%s",     macSuffix.c_str());
    snprintf(idStop,     sizeof(idStop),     "beogramStop_%s",     macSuffix.c_str());
    snprintf(idStandby,  sizeof(idStandby),  "beogramStandby_%s",  macSuffix.c_str());
    snprintf(idTrack,    sizeof(idTrack),    "beogramCDTrack_%s",  macSuffix.c_str());
    snprintf(idPlayback, sizeof(idPlayback), "beogramState_%s",    macSuffix.c_str());
    snprintf(idPlaying,  sizeof(idPlaying),  "beogramPlaying_%s",  macSuffix.c_str());
    snprintf(configUrl,  sizeof(configUrl),  "http://%s/",         WiFi.localIP().toString().c_str());
    
    // Unified HA device unique ID: raw MAC bytes (canonical ArduinoHA
    // pattern, hex-encoded by the library). The legacy Mozart firmware
    // passed the MAC-suffix *string* as bytes instead, so devices
    // upgrading from Moz will re-register once under the new ID; the
    // stale device can be removed from the MQTT integration in HA.
    device.setUniqueId(mac, sizeof(mac));

    device.setConfigurationUrl(configUrl);
    device.setName("BGAdaptor");
    device.setSoftwareVersion(FIRMWARE_VERSION);
    device.enableSharedAvailability();
    device.enableLastWill();    
    bgPlay.setIcon("mdi:play-circle");
    bgPlay.setName("Play");
    bgNext.setIcon("mdi:skip-next-circle");
    bgNext.setName("Next");
    bgPrev.setIcon("mdi:skip-previous-circle");
    bgPrev.setName("Prev");  
    bgStop.setIcon("mdi:stop-circle");
    bgStop.setName("Stop");
    bgStandby.setIcon("mdi:power-standby");
    bgStandby.setName("Standby"); 
    bgTrack.setIcon("mdi:music-note-eighth");
    bgTrack.setName("Track");  
    bgPlaybackState.setIcon("mdi:album");
    bgPlaybackState.setName("State");
    bgPlaying.setDeviceClass("running");
    bgPlaying.setName("Playing");
    bgPlaying.setIcon("mdi:disc-player");      
    mqtt.setDiscoveryPrefix("homeassistant");

    bgPlay.onCommand(onButtonCommand);
    bgNext.onCommand(onButtonCommand);
    bgPrev.onCommand(onButtonCommand);
    bgStop.onCommand(onButtonCommand);
    bgStandby.onCommand(onButtonCommand);      

    // ── Mozart product websockets ───────────────────────────────────
    wsClient.onMessage([](WebsocketsMessage msg) { processWebSocketMessage(msg.data()); });
    wsClient.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            wsLastPingReceived = millis();
            Serial.println("Websocket connected");
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            wsLastPingReceived = millis();
        }
    });

    remoteClient.onMessage([](WebsocketsMessage msg) { processRemoteWebSocketMessage(msg.data()); });
    remoteClient.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            Serial.println("Secondary websocket connected");
            wsRemoteLastPingReceived = millis();
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Secondary websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            wsRemoteLastPingReceived = millis();
        }
    });

    if (platform == PLATFORM_MOZART && productIP.length() > 0) {
        wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str());
        remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
    }

    // ── Beoremote Halo (shared) ─────────────────────────────────────
    haloClient.onMessage(onMessageCallback); 
    haloClient.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            haloLastPingReceived = millis();
            Serial.println("Halo Websocket connected");
            sendConfigToHalo();
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Halo Websocket closed");
        } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
            haloLastPingReceived = millis();
        }
    });
  
    if (haloIP.length() > 0) {
        haloClient.connect(("ws://" + haloIP + ":" + HALO_WEBSOCKET_PORT).c_str());
    }

    // ── Web server routes ───────────────────────────────────────────
    registerWebRoutes();   // webui.cpp — includes server.begin()
    webpushBegin();

    // Only send greeting if connection was actually established
    if (platform == PLATFORM_MOZART && productIP.length() > 0 && wsClient.available()) {
        wsClient.send("Hi Server!");
    }

    MDNS.addService("http", "tcp", 80);

    checkMQTTConnection(true);  // Force immediate connect attempt

    if (platform == PLATFORM_ASE && productIP.length() > 0) {
        connectToSSE();
    }
}

void loop() {
    updateLEDStatus();

    if (platform == PLATFORM_MOZART) {
        wsClient.poll();
        remoteClient.poll();
    } else {
        if (productIP.length() > 0) {
            checkSSEConnection();
        }
        readSSE();
    }

    connectToHalo();
    checkPingWebsocket();
    checkProductRecovery();
    checkPendingExpand();
    handleSerial1Data();
    webpushLoop();
    server.handleClient();
    checkWiFiConnection();
    sendPlayAfterDelay();
    checkStoppedTrackTimeout();
    secondButtonUpdate();
    activateHaloPage();
    mqtt.loop();
    checkMQTTConnection();    

    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        
        if (input == "debug 1") {
            debugSerial = true;
            Serial.println("Debug mode enabled");
        } else if (input == "debug 0") {
            debugSerial = false;
            Serial.println("Debug mode disabled");
        } else if (input.startsWith("hex ")) {
            // Send an arbitrary Datalink command, e.g. "hex 0x2A" or "hex 2a".
            // Handy for probing undocumented Beogram commands from the monitor.
            debugSerial = true;
            String arg = input.substring(4);
            arg.trim();
            if (arg.startsWith("0x") || arg.startsWith("0X")) arg = arg.substring(2);

            char* end;
            long value = strtol(arg.c_str(), &end, 16);
            if (arg.length() == 0 || *end != '\0' || value < 0 || value > 0xFF) {
                Serial.println("Usage: hex <00-FF>, e.g. hex 0x2A");
            } else {
                Serial.printf("Sending 0x%02X to the Beogram\n", (uint8_t)value);
                sendHexCommand((BeogramCommand)value);
            }
        }    
    }
}
