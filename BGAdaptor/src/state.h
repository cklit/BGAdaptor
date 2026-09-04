#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"

using namespace websockets;

// Shared globals. This is the honest representation of how the modules
// communicate; definitions live in state.cpp (HA objects in ha_mqtt.cpp,
// see the init-order note there).

extern bool debugSerial;
extern Platform platform;
extern DeviceType deviceType;

// Timers
extern unsigned long sseLastReconnectAttempt;
extern unsigned long sseReconnectDelay;      // ASE: exponential backoff, start at 1s
extern unsigned long sseLastDataReceived;    // ASE: last byte seen on the SSE stream
extern unsigned long wsLastReconnectAttempt;    // Mozart
extern unsigned long wsReconnectDelay;          // Mozart: grows while the product is unreachable
extern unsigned long mqttReconnectDelay;        // grows while the broker is unreachable
extern unsigned long haloLastReconnectAttempt;
extern unsigned long mqttLastReconnectAttempt;
extern unsigned long wsLastPingReceived;        // Mozart
extern unsigned long wsRemoteLastPingReceived;  // Mozart
extern unsigned long haloLastPingReceived;
extern unsigned long haloActionTime;   // set when certain Halo state/page updates are triggered
extern unsigned long lastStartEventTime;   // Mozart: debounce "started" events
extern unsigned long delayPlayAfterDigit;  // delay PLAY command to CD player, when using digits

// Playback / Halo state
extern PlaybackState playbackState;
extern String beogramStateText;   // human-readable, mirrors the HA sensor
extern String beogramTrack;
extern bool beogramPlaying;
extern bool beogramStateDirty;    // set by processBuffer, consumed by webpushLoop
extern HaloUpdate haloUpdate;

// Flags
extern bool haloControls;
extern bool haloPlayIcon;      // turntable only: show a turntable icon on the Halo Play button
extern bool haloVolumeControls;
extern bool lineInActive;
extern bool speakerExpanded;   // true while the secondary speaker is expanded in
extern unsigned long expandDueAt;  // 0 = nothing pending, else millis() when the expand fires
extern bool waitingForPlay;
extern bool mqttConnected;

// Config values (persisted in Preferences)
extern String productIP;       // unified — replaces sseIP (ASE) / wsIP (Mozart)
extern String productSerial;     // serial number from discovery TXT records; empty for manual IP entry
extern String productName;       // friendly name from discovery; empty for manual IP entry
extern String haloIP;
extern String haloSerial;      // serial from discovery TXT "serial"; empty for manual IP entry
extern String playbackJid;     // Beolink JID of the secondary playback speaker ("" = off)
extern String playbackName;    // its friendly name, for the UI
extern String playbackSerial;  // its serial number, shown alongside the name
extern String triggerSource;   // ASE: "LINE IN"/"TOSLINK" — Mozart: "lineIn"/"spdif"
extern String mqttIP;
extern String mqttUser;
extern String mqttPassword;

// Peripherals & connections
extern Adafruit_NeoPixel pixels;
extern Preferences preferences;
extern HTTPClient http;
extern WiFiClient sseClient;         // ASE: raw TCP client for the SSE stream
extern WebsocketsClient wsClient;    // Mozart: product notification websocket
extern WebsocketsClient remoteClient;// Mozart: /remoteControl websocket
extern WebsocketsClient haloClient;  // shared: Beoremote Halo
extern WebServer server;

extern byte mac[6];
extern char configUrl[25];
extern char idPlay[35];
extern char idNext[35];
extern char idPrev[35];
extern char idStop[35];
extern char idStandby[35];
extern char idTrack[35];
extern char idPlayback[35];
extern char idPlaying[35];
