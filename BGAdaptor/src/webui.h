#pragma once
#include "state.h"

// Web UI: route registration and all HTTP request handlers except
// discovery (discovery.h).

bool isValidIPAddress(const String& ip);
void registerWebRoutes();

void handleRoot();
void handleOTAUpdate();
void handleOTAResult();
void handleUpdate();
void handleUpdateHalo();
void handleUpdateTriggerSource();
void handleUpdatePlatform();
void handleMqttReset();
void handleMqttUpdate();
void handleMqttConfig();
void handleUpdateFeature();
void handleUpdateDeviceType();
void handleUpdateHaloPlayIcon();
void handlePlaybackSpeaker();
void handleUpdatePlaybackSpeaker();
void handleStatus();
void handleResetWifi();
void handleFactoryReset();
