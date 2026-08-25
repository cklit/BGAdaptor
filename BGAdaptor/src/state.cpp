#include "state.h"

bool debugSerial = false;
Platform platform = PLATFORM_ASE;
DeviceType deviceType = DEVICE_CD;

unsigned long sseLastReconnectAttempt = 0;
unsigned long sseReconnectDelay = 1000;
unsigned long sseLastDataReceived = 0;
unsigned long wsLastReconnectAttempt = millis();
unsigned long wsReconnectDelay = reconnectInterval;
unsigned long mqttReconnectDelay = reconnectInterval;
unsigned long haloLastReconnectAttempt = millis();
unsigned long mqttLastReconnectAttempt = millis();
unsigned long wsLastPingReceived = millis();
unsigned long wsRemoteLastPingReceived = millis();
unsigned long haloLastPingReceived = millis();
unsigned long haloActionTime = 0;
unsigned long lastStartEventTime = 0;
unsigned long delayPlayAfterDigit = 0;

PlaybackState playbackState = BOOT;
String beogramStateText = "Unknown";
String beogramTrack = "-";
bool beogramPlaying = false;
bool beogramStateDirty = false;
HaloUpdate haloUpdate = NONE;

bool haloControls;
bool haloPlayIcon = false;
bool lineInActive = false;
bool speakerExpanded = false;
bool waitingForPlay = false;
bool mqttConnected = false;

String productIP;
String productSerial;
String productName;
String haloIP;
String haloSerial;
String playbackJid;
String playbackName;
String playbackSerial;
String triggerSource;
String mqttIP;
String mqttUser;
String mqttPassword;

Adafruit_NeoPixel pixels(NUMPIXELS, LEDPIN, NEO_GRB + NEO_KHZ800);
Preferences preferences;
HTTPClient http;
WiFiClient sseClient;
WebsocketsClient wsClient;
WebsocketsClient remoteClient;
WebsocketsClient haloClient;
WebServer server(80);

byte mac[6];
char configUrl[25];
char idPlay[35];
char idNext[35];
char idPrev[35];
char idStop[35];
char idStandby[35];
char idTrack[35];
char idPlayback[35];
char idPlaying[35];
