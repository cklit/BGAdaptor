#pragma once
#include <Arduino.h>

// ── Pins & identity ─────────────────────────────────────────────────
#define RXD2 16
#define TXD2 17
#define LEDPIN 47
#define NUMPIXELS 1

// Version scheme: BGAdaptor.<year>.<month>.<day> of the build/release
<<<<<<< HEAD
<<<<<<< HEAD
#define FIRMWARE_VERSION "BGAdaptor.2026.8.20"
||||||| parent of 9ffbfe0 (New function: Auto-expand)
#define FIRMWARE_VERSION "BGAdaptor.2026.8.19_beta12"
=======
#define FIRMWARE_VERSION "BGAdaptor.2026.8.25"
>>>>>>> 9ffbfe0 (New function: Auto-expand)
||||||| parent of 72eed90 (Help section corrected)
#define FIRMWARE_VERSION "BGAdaptor.2026.8.25"
=======
#define FIRMWARE_VERSION "BGAdaptor.2026.9.3"
>>>>>>> 72eed90 (Help section corrected)

// ── Platform selection ──────────────────────────────────────────────
// The adaptor supports two B&O platforms with different transports:
//   ASE    → SSE stream on :8080 (/BeoNotify/Notifications) + BeoZone REST
//   Mozart → WebSockets on :9339 (+ /remoteControl) + /api/v1 REST
enum Platform { PLATFORM_ASE, PLATFORM_MOZART };

// Which kind of deck is connected. CD players report their true state, so
// one button can toggle Play/Stop. Record players report "playing" but
// never report the tonearm being lifted, so a toggle would get out of sync
// — those get dedicated Play and Stop buttons instead.
enum DeviceType { DEVICE_CD, DEVICE_RECORD, DEVICE_TAPE };

// ── Network constants ───────────────────────────────────────────────

static const int SSE_PORT = 8080;           // ASE notification stream
static const int WEBSOCKET_PORT = 9339;     // Mozart notification websocket
static const int HALO_WEBSOCKET_PORT = 8080;// Halo notification websocket
static const int UI_WS_PORT = 81;           // live state push to the web UI

static const char* const DEVICE_NAME = "Beogram";
static const char* const AP_SSID = "BGAdaptor";
static const char* const AP_PASSWORD = "password";

// ── Tunables ────────────────────────────────────────────────────────
static const unsigned long reconnectInterval = 10000;
// Reconnect backoff. A connect() to an unreachable host blocks the loop for
// seconds, so repeated failures must slow down or the adaptor spends most of
// its time stalled (no web UI, no serial handling, no Halo updates).
static const unsigned long reconnectMaxInterval = 60000;

// Secondary playback speaker: wait a moment after the product reports the
// trigger source before expanding. The source event arrives as soon as the
// product switches, which can be before its Beolink experience is ready to
// accept a listener.
static const unsigned long expandDelayMs = 250;
static const unsigned long pingTimeout = 10000;
static const unsigned long haloActionDelay = 800;
static const unsigned long stateDebounceDelay = 100;
static const unsigned long SSE_IDLE_TIMEOUT_MS = 300000;  // ASE: reconnect if stream silent for 5 min
#define MDNS_SCAN_TIME_MS 5000   // mDNS discovery: per service type
// Self-healing: if a discovery-linked product stays unreachable, rescan
// mDNS for its serial number and follow it to a new IP address.
static const unsigned long RECOVERY_AFTER_MS = 120000;         // 2 min of failed reconnects first
static const unsigned long RECOVERY_SCAN_INTERVAL_MS = 180000; // then rescan every 3 min

// ── Beogram serial protocol ─────────────────────────────────────────
enum BeogramCommand : uint8_t {
    PLAY = 0x35,
    STOP = 0x26,
    STANDBY = 0x16,
    NEXT = 0x2B,
    PREVIOUS = 0x18,
    WIND = 0x1A,
    REWIND = 0x3A,
    OPEN_FOR_DIGIT = 0x66,
    DIGIT1 = 0x1F,
    DIGIT2 = 0x2F,
    DIGIT3 = 0x0F,
    DIGIT4 = 0x37,
    DIGIT5 = 0x17,
    DIGIT6 = 0x27,
    DIGIT7 = 0x07,
    DIGIT8 = 0x3B,
    DIGIT9 = 0x1B,
    DIGIT0 = 0x3F
};

// Tape decks speak the same Datalink but with different byte values, both
// for commands and for feedback. sendHexCommand() translates outgoing
// commands and identifyState() translates incoming feedback, so the rest
// of the firmware keeps using the canonical names below.
enum TapeCommand : uint8_t {
    TAPE_PLAY = 0x15,
    TAPE_STOP = 0x29,
    TAPE_STANDBY = 0x16,
    TAPE_NEXT = 0x39,      // cue forward
    TAPE_PREVIOUS = 0x05   // cue backwards
};

enum TapeFeedback : uint8_t {
    TAPE_PLAYING_FB = 0x09,
    TAPE_STOPPED_FB = 0x69,
    TAPE_STANDBY_FB = 0x3E
};

enum PlaybackState {
    PLAYING,
    PAUSED,
    STOPPED,
    BOOT
};

enum HaloUpdate {
    CONFIG,
    STATE,
    PAGE,
    NONE
};

enum BeogramFeedback : uint8_t {
    TRACK1 = 0x01,   
    TRACK2 = 0x02,
    TRACK3 = 0x03,
    TRACK4 = 0x04,
    TRACK5 = 0x05,
    TRACK6 = 0x06,
    TRACK7 = 0x07,
    TRACK8 = 0x08,
    TRACK9 = 0x09,
    TRACK10 = 0x0A,
    TRACK11 = 0x0B,
    TRACK12 = 0x0C,
    TRACK13 = 0x0D,
    TRACK14 = 0x0E,
    TRACK14_PLUS = 0x0F,
    PLAYING_FB = 0x1E,
    STOPPED_FB = 0x46,
    STANDBY_FB = 0x2E,
    EJECTED_FB = 0x76,
    UNKNOWN_STATE = 0xFF
};
