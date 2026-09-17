#include "webui.h"
#include "webpage.h"
#include "webpage_icons.h"
#include "beogram.h"
#include "transport.h"
#include "halo.h"
#include "ha_mqtt.h"
#include "discovery.h"
#include "peer.h"
#include "webpush.h"
#include <mdns.h>
#include <WiFi.h>
#include <WiFiManager.h>

extern WiFiManager wm;  // defined in main.cpp
#include <Update.h>
#include <ArduinoJson.h>

bool isValidIPAddress(const String& ip) {
    int parts[4];
    if (sscanf(ip.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (parts[i] < 0 || parts[i] > 255) return false;
        }
        return true;
    }
    return false;
}


void handleRoot() {
    server.send(200, "text/html", htmlPage);
}

void handleOTAUpdate() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA Update Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Serial.printf("Writing %d bytes...\n", upload.currentSize);
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA Update Success! %d bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

// Result page shown after an OTA upload finishes. Refreshes after 12 sec.
void handleOTAResult() {
    bool failed = Update.hasError();
    String page = String("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>BGAdaptor</title>");
    // Only bounce back automatically on success — on failure the adaptor
    // isn't restarting, so there's nothing to wait for.
    if (!failed) page += "<meta http-equiv='refresh' content='12;url=/'>";
    page += "<style>body{font-family:system-ui,sans-serif;background:#f0f0f0;color:#111;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;text-align:center}"
        "@media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}"
        "div{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:2rem;max-width:340px}"
        "@media(prefers-color-scheme:dark){div{background:#252525;border-color:#333}}"
        "h2{font-size:16px;font-weight:500;margin:0 0 .5rem}"
        "p{font-size:13px;color:#888;margin:0 0 1.25rem}"
        "a{display:inline-block;height:36px;line-height:36px;padding:0 14px;font-size:13px;"
        "border-radius:8px;background:" + String(failed ? "#D64545" : "#1D9E75") +
        ";color:#fff;text-decoration:none}</style></head><body><div>";
    page += failed
        ? "<h2>Update failed</h2><p>The firmware was not installed :(.</p>"
        : "<h2>Update successful</h2><p>Restarting the BGAdaptor. You will be redirected automatically in a few seconds.</p>";
    page += "<a href='/'>Back to main page</a></div></body></html>";

    server.send(200, "text/html", page);
    delay(1000);

    if (!failed) ESP.restart();
}

void handleUpdate() {
    if (server.hasArg("productIP")) {
        String newIP = server.arg("productIP");
        if (newIP == "") {
            if (platform == PLATFORM_MOZART) {
                wsClient.close();
                remoteClient.close();
            } else {
                sseClient.stop();
            }
            productIP = newIP;
            productSerial = "";
            productName = "";
            preferences.putString("productIP", productIP);
            preferences.putString("productSerial", productSerial);
            preferences.putString("productName", productName);
            server.send(200, "text/plain", "Unlinked product");
            Serial.println("Unlinked product."); 
            return;
        } else if (!isValidIPAddress(newIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved.");             
            return;
        }
        productIP = newIP;
        preferences.putString("productIP", productIP);
        // Serial number only accompanies discovery-based links; manual IP
        // entry sends none, which intentionally clears any old one.
        productSerial = server.hasArg("productSerial") ? server.arg("productSerial") : "";
        preferences.putString("productSerial", productSerial);
        productName = server.hasArg("productName") ? server.arg("productName") : "";
        preferences.putString("productName", productName);

        if (platform == PLATFORM_MOZART) {
            wsClient.close();
            remoteClient.close();
            wsClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT).c_str());
            remoteClient.connect(("ws://" + productIP + ":" + WEBSOCKET_PORT + "/remoteControl").c_str());
            server.send(200, "text/html", "<h2>IP Updated to " + productIP + "</h2><a href='/'>Go Back</a>");
        } else {
            server.send(200, "text/html", "<h2>IP Updated to " + productIP + "</h2><a href='/'>Go Back</a>");
            sseClient.stop();
            connectToSSE();
        }
    } else {
        server.send(400, "text/html", "<h2>No IP Address Provided</h2><a href='/'>Go Back</a>");
    }
}

void handleUpdateHalo() {
    if (server.hasArg("haloIP")) {
        String newHaloIP = server.arg("haloIP");
        if (newHaloIP == "") {
            haloClient.close();
            haloIP = newHaloIP;
            haloSerial = "";
            preferences.putString("haloIP", haloIP); // Store the Halo IP in preferences
            preferences.putString("haloSerial", haloSerial);
            // A peer adaptor exists only to occupy a page on this Halo, so a
            // link left behind here would be a setting with nothing to do —
            // and it would keep the secondary's own Halo card disabled.
            if (peerConfigured()) peerSetLink("", "", "");
            server.send(200, "text/plain", "Unlinked Halo");
            Serial.println("Unlinked Halo."); 
            return;
        } else if (!isValidIPAddress(newHaloIP)) {
            server.send(400, "text/html", "<h2>Invalid IP Address for Beoremote Halo</h2><a href='/'>Go Back</a>");
            Serial.println("Invalid IP Address - not saved."); 
            return;
        }
        haloIP = newHaloIP;
        preferences.putString("haloIP", haloIP); // Store the Halo IP in preferences
        // Serial only accompanies discovery-based links; manual IP entry
        // sends none, which intentionally clears any old one.
        haloSerial = server.hasArg("haloSerial") ? server.arg("haloSerial") : "";
        preferences.putString("haloSerial", haloSerial);

        // If there is an existing connection, close it first
        haloClient.close();

        // Now establish a new WebSocket connection to the Beoremote Halo WebSocket server
        haloClient.connect(("ws://" + haloIP + ":" + HALO_WEBSOCKET_PORT).c_str());
        server.send(200, "text/html", "<h2>Halo IP Updated to " + haloIP + "</h2><a href='/'>Go Back</a>");
    } else {
        server.send(400, "text/html", "<h2>No Halo IP Address Provided</h2><a href='/'>Go Back</a>");
    }
}

void handleUpdateTriggerSource() {
    if (server.hasArg("source")) {
        String newSource = server.arg("source");
        bool valid = (platform == PLATFORM_MOZART)
            ? (newSource == "lineIn" || newSource == "spdif")
            : (newSource == "LINE IN" || newSource == "TOSLINK");
        if (valid) {
            triggerSource = newSource;
            preferences.putString("triggerSource", triggerSource);
            server.send(200, "text/plain", "Source updated");
            return;
        }
    }
    server.send(400, "text/plain", "Invalid source");
}

void handleUpdatePlatform() {
    if (server.hasArg("platform")) {
        String newPlatform = server.arg("platform");
        if (newPlatform == "ase" || newPlatform == "mozart") {
            preferences.putString("platform", newPlatform);
            // The stored product IP points at the old platform's product, and the
            // trigger source values differ between platforms — reset both. If a
            // product IP is supplied (discovery flow), store it instead so the
            // adaptor connects to the discovered product right after restart.
            String newIP = server.hasArg("productIP") ? server.arg("productIP") : "";
            if (newIP != "" && !isValidIPAddress(newIP)) newIP = "";
            preferences.putString("productIP", newIP);
            preferences.putString("productSerial", newIP != "" && server.hasArg("productSerial") ? server.arg("productSerial") : "");
            preferences.putString("productName", newIP != "" && server.hasArg("productName") ? server.arg("productName") : "");
            preferences.putString("triggerSource", newPlatform == "mozart" ? "lineIn" : "LINE IN");
            server.send(200, "text/plain", "Platform updated. Restarting...");
            Serial.println("Platform changed to " + newPlatform + ". Restarting...");
            delay(500);
            ESP.restart();
            return;
        }
    }
    server.send(400, "text/plain", "Invalid platform");
}

void handleMqttReset() {
    preferences.remove("mqttIP");
    preferences.remove("mqttUser");
    preferences.remove("mqttPassword");

    mqttIP = "";
    mqttUser = "";
    mqttPassword = "";

    server.send(200, "text/html", R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <meta http-equiv="refresh" content="5;url=/mqtt">
        <title>Reset</title>
        <style>)rawliteral" PAGE_ICON_CSS R"rawliteral(
            *{box-sizing:border-box;margin:0;padding:0}
            body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
            @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
            .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
            .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
            .page-title .ic{font-size:22px;color:#666}
            .page-title h1{font-size:18px;font-weight:500}
            .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
            @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
            .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
            .card-header .ic{font-size:18px;color:#a32d2d}
            .card-header h2{font-size:15px;font-weight:500}
            .sub{font-size:13px;color:#888;margin-bottom:1rem}
            .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
            .btn:hover{background:#f5f5f5}
            @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
        </style>
        </head>
        <body>)rawliteral" PAGE_SPRITE R"rawliteral(
        <div class="page">
        <div class="page-title">
            <svg class="ic"><use href="#i-smart-home"/></svg>
            <h1>Home Assistant</h1>
        </div>
        <div class="card">
            <div class="card-header">
            <svg class="ic"><use href="#i-trash"/></svg>
            <h2>MQTT settings cleared</h2>
            </div>
            <p class="sub">Restarting to apply changes&hellip;</p>
            <a href="/mqtt" class="btn"><svg class="ic" style="font-size:14px"><use href="#i-arrow-left"/></svg>Back to MQTT</a>
        </div>
        </div>
        </body>
        </html>
        )rawliteral");

    delay(1000);
    ESP.restart();
}

void handleMqttUpdate() {
    if (server.hasArg("ip") && server.hasArg("user") && server.hasArg("pass")) {
        mqttIP = server.arg("ip");
        mqttUser = server.arg("user");
        mqttPassword = server.arg("pass");

        preferences.putString("mqttIP", mqttIP);
        preferences.putString("mqttUser", mqttUser);
        preferences.putString("mqttPassword", mqttPassword);

        server.send(200, "text/html", R"rawliteral(
            <!DOCTYPE html>
            <html lang="en">
            <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1.0">
            <meta http-equiv="refresh" content="5;url=/">
            <title>Saved</title>
            <style>)rawliteral" PAGE_ICON_CSS R"rawliteral(
                *{box-sizing:border-box;margin:0;padding:0}
                body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
                @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
                .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
                .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
                .page-title .ic{font-size:22px;color:#666}
                .page-title h1{font-size:18px;font-weight:500}
                .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
                @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
                .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
                .card-header .ic{font-size:18px;color:#1D9E75}
                .card-header h2{font-size:15px;font-weight:500}
                .sub{font-size:13px;color:#888;margin-bottom:1rem}
                .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
                .btn:hover{background:#f5f5f5}
                @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
            </style>
            </head>
            <body>)rawliteral" PAGE_SPRITE R"rawliteral(
            <div class="page">
            <div class="page-title">
                <svg class="ic"><use href="#i-smart-home"/></svg>
                <h1>Home Assistant</h1>
            </div>
            <div class="card">
                <div class="card-header">
                <svg class="ic"><use href="#i-circle-check"/></svg>
                <h2>Settings saved</h2>
                </div>
                <p class="sub">Restarting to apply changes&hellip;</p>
                <a href="/mqtt" class="btn"><svg class="ic" style="font-size:14px"><use href="#i-arrow-left"/></svg>Back to MQTT</a>
            </div>
            </div>
            </body>
            </html>
            )rawliteral");

        delay(1000);
        ESP.restart();
    } else {
        server.send(400, "text/html", R"rawliteral(
            <!DOCTYPE html>
            <html lang="en">
            <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1.0">
            <title>Error</title>
            <style>)rawliteral" PAGE_ICON_CSS R"rawliteral(
                *{box-sizing:border-box;margin:0;padding:0}
                body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
                @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
                .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
                .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
                .page-title .ic{font-size:22px;color:#666}
                .page-title h1{font-size:18px;font-weight:500}
                .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
                @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
                .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
                .card-header .ic{font-size:18px;color:#a32d2d}
                .card-header h2{font-size:15px;font-weight:500}
                .sub{font-size:13px;color:#888;margin-bottom:1rem}
                .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;gap:6px}
                .btn:hover{background:#f5f5f5}
                @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
            </style>
            </head>
            <body>)rawliteral" PAGE_SPRITE R"rawliteral(
            <div class="page">
            <div class="page-title">
                <svg class="ic"><use href="#i-smart-home"/></svg>
                <h1>Home Assistant</h1>
            </div>
            <div class="card">
                <div class="card-header">
                <svg class="ic"><use href="#i-circle-x"/></svg>
                <h2>Missing parameters</h2>
                </div>
                <p class="sub">Please fill in all fields and try again.</p>
                <a href="/mqtt" class="btn"><svg class="ic" style="font-size:14px"><use href="#i-arrow-left"/></svg>Back to MQTT</a>
            </div>
            </div>
            </body>
            </html>
            )rawliteral");
    }
}

void handleMqttConfig() {
    String html = R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>MQTT Configuration</title>
        <style>)rawliteral" PAGE_ICON_CSS R"rawliteral(
            *{box-sizing:border-box;margin:0;padding:0}
            body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
            @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
            .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
            .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
            .page-title .ic{font-size:22px;color:#666}
            .page-title h1{font-size:18px;font-weight:500}
            .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
            @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
            .card-header{display:flex;align-items:center;gap:10px;margin-bottom:1rem}
            .card-header .ic{font-size:18px;color:#888}
            .card-header h2{font-size:15px;font-weight:500}
            .form-group{display:flex;flex-direction:column;gap:6px;margin-bottom:.75rem}
            .form-group label{font-size:13px;color:#666}
            @media(prefers-color-scheme:dark){.form-group label{color:#aaa}}
            .form-group input{font-size:13px;padding:0 10px;height:36px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;width:100%}
            @media(prefers-color-scheme:dark){.form-group input{background:#1a1a1a;border-color:#444;color:#eee}}
            .divider{height:1px;background:#f0f0f0;margin:.75rem 0}
            @media(prefers-color-scheme:dark){.divider{background:#333}}
            .btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;white-space:nowrap;width:100%}
            .btn:hover{background:#f5f5f5}
            @media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
            .btn-danger{border-color:#f09595;color:#a32d2d}
            .btn-danger:hover{background:#fcebeb}
            @media(prefers-color-scheme:dark){.btn-danger{border-color:#793333;color:#f09595}.btn-danger:hover{background:#2a1a1a}}
            .back-link{display:flex;align-items:center;gap:8px;font-size:13px;color:#185fa5;text-decoration:none}
            .back-link:hover{text-decoration:underline}
            @media(prefers-color-scheme:dark){.back-link{color:#85b7eb}}
            .status-label{font-size:13px;color:#666}
            @media(prefers-color-scheme:dark){.status-label{color:#aaa}}
        </style>
        </head>
        <body>)rawliteral" PAGE_SPRITE R"rawliteral(
        <div class="page">
        <div class="page-title">
            <svg class="ic"><use href="#i-smart-home"/></svg>
            <h1>Home Assistant</h1>
        </div>

        <div class="card">
            <div class="card-header"><svg class="ic"><use href="#i-server"/></svg><h2>MQTT broker</h2></div>
            <form method="POST" action="/mqtt">
            <div class="form-group">
                <label for="ip">Broker IP address</label>
                <input type="text" id="ip" name="ip" placeholder="e.g. 192.168.1.10" value=")rawliteral" + mqttIP + R"rawliteral(">
            </div>
            <div class="form-group">
                <label for="user">Username</label>
                <input type="text" id="user" name="user" autocomplete="username" value=")rawliteral" + mqttUser + R"rawliteral(">
            </div>
            <div class="form-group" style="margin-bottom:1rem">
                <label for="pass">Password</label>
                <input type="password" id="pass" name="pass" autocomplete="current-password" value=")rawliteral" + mqttPassword + R"rawliteral(">
            </div>
            <button type="submit" class="btn">Save settings</button>
            </form>
            <div class="divider"></div>
            <form id="reset-form" method="GET" action="/mqtt/reset">
            <button type="submit" class="btn btn-danger">Reset MQTT settings</button>
            </form>
        </div>

        <a href="/" class="card back-link">
            <svg class="ic" style="font-size:16px"><use href="#i-arrow-left"/></svg>
            Back to main page
        </a>
        </div>
        <script>
        document.getElementById('reset-form').addEventListener('submit',function(e){
            if(!confirm('This will erase your MQTT settings. Are you sure?'))e.preventDefault();
        });
        </script>
        </body>
        </html>
        )rawliteral";
    server.send(200, "text/html", html);
}

void handleUpdateFeature() {
    if (server.hasArg("enabled")) {
        String value = server.arg("enabled");
        haloControls = (value == "true");
        preferences.putBool("feature_enabled", haloControls);
    }
    server.send(200, "text/plain", "OK");
}

void handleUpdateDeviceType() {
    if (server.hasArg("type")) {
        String value = server.arg("type");
        if (value == "cd" || value == "record" || value == "tape") {
            deviceType = (value == "record") ? DEVICE_RECORD
                       : (value == "tape")   ? DEVICE_TAPE
                                             : DEVICE_CD;
            preferences.putString("deviceType", value);
            // The Halo layout differs between deck types — and so does the
            // page title, when no custom name is set. A plain resend does not
            // retitle, so take the reconnect path.
            reconnectHalo("deck type changed");
            // Tell anyone listening on the state socket, including an adaptor
            // that renders a Halo page for this deck.
            broadcastBeogramState();
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    server.send(400, "text/plain", "Invalid device type");
}

void handleUpdateHaloPlayIcon() {
    if (server.hasArg("enabled")) {
        haloPlayIcon = (server.arg("enabled") == "true");
        preferences.putBool("haloPlayIcon", haloPlayIcon);
        // The button's content type changes, so the Halo needs the new config.
        if (haloClient.available()) sendConfigToHalo();
        server.send(200, "text/plain", "OK");
        return;
    }
    server.send(400, "text/plain", "Missing value");
}

// A custom Halo page title. It ends up inside hand-built JSON in both the
// Halo configuration and the state broadcast, so it is cleaned here, at the
// single point where it enters the system: quotes, backslashes and control
// characters out, then truncated. Everything downstream can then treat it as
// safe without repeating the check.
static String sanitiseName(const String& in) {
    String out;
    for (size_t i = 0; i < in.length() && out.length() < ADAPTOR_NAME_MAX; i++) {
        char c = in[i];
        if (c == '"' || c == '\\' || (uint8_t)c < 0x20 || (uint8_t)c == 0x7F) continue;
        out += c;
    }
    out.trim();
    return out;
}

void handleUpdateAdaptorName() {
    if (!server.hasArg("title")) {
        server.send(400, "text/plain", "Missing value");
        return;
    }
    adaptorName = sanitiseName(server.arg("title"));
    preferences.putString("adaptorName", adaptorName);

    // A peer adaptor titles its page 2 from the state socket, so tell it.
    broadcastBeogramState();

    // The TXT record is otherwise only written in setup(), which would leave
    // other adaptors seeing the old name in a scan until this one rebooted.
    // Rewriting it in place is enough — the responder announces the change.
    String fn = adaptorName.length() ? adaptorName
              : productName.length() ? productName : String(DEVICE_NAME);
    mdns_service_txt_item_set("_bgadaptor", "_tcp", "fn", fn.c_str());

    reconnectHalo("renamed");   // page 1 is titled from this

    server.send(200, "text/plain", adaptorName);
}

void handleUpdateHaloVolumeControls() {
    if (server.hasArg("enabled")) {
        haloVolumeControls = (server.arg("enabled") == "true");
        preferences.putBool("haloVolCtrl", haloVolumeControls);
        if (!haloVolumeControls && haloClient.available()) {
            setVolumeRing(100);
        }
        server.send(200, "text/plain", "OK");
        return;
    }
    server.send(400, "text/plain", "Missing value");
}


// Auto-expand sub-page: pick one speaker that joins the product's Beolink
// experience when the deck starts playing. Reached from the product card;
// scans via /discover-speakers (which excludes the linked product).
void handlePlaybackSpeaker() {
    server.send(200, "text/html", String(R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Auto-expand</title>
<style>)rawliteral") + PAGE_ICON_CSS + R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
@media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
.page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
.page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
.page-title .ic{font-size:22px;color:#666}
.page-title h1{font-size:18px;font-weight:500}
.card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
@media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
.card-header{display:flex;align-items:center;gap:10px;margin-bottom:1rem}
.card-header .ic{font-size:18px;color:#888}
.card-header h2{font-size:15px;font-weight:500}
.status-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:.65rem}
.status-label{font-size:13px;color:#666}
@media(prefers-color-scheme:dark){.status-label{color:#aaa}}
.chip{font-size:12px;font-family:monospace;color:#666;background:#f5f5f5;padding:2px 8px;border-radius:4px}
@media(prefers-color-scheme:dark){.chip{background:#333;color:#bbb}}
.input-row{display:flex;gap:8px;margin-top:8px}
.btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;white-space:nowrap;display:inline-flex;align-items:center;justify-content:center;line-height:1}
.btn:hover{background:#f5f5f5}
@media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
.btn.scanning{animation:scanPulse 1.3s ease-in-out infinite}
@keyframes scanPulse{0%,100%{background:#fff;border-color:#ddd;color:#666}50%{background:#e1f5ee;border-color:#1D9E75;color:#0f6e56}}
@media(prefers-color-scheme:dark){@keyframes scanPulse{0%,100%{background:#2a2a2a;border-color:#444;color:#aaa}50%{background:#1e3d34;border-color:#1D9E75;color:#7fd9bb}}}
@media(prefers-reduced-motion:reduce){.btn.scanning{animation:none;border-color:#1D9E75}}
.btn-highlight{background:#1D9E75;border-color:#1D9E75;color:#fff}
.btn-highlight:hover{background:#178a65}
@media(prefers-color-scheme:dark){.btn-highlight{background:#1D9E75;border-color:#1D9E75;color:#fff}.btn-highlight:hover{background:#178a65}}
select{width:100%;height:34px;padding:0 10px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111}
@media(prefers-color-scheme:dark){select{background:#1a1a1a;border-color:#444;color:#eee}}
label{font-size:13px;color:#666;display:block;margin-bottom:6px}
@media(prefers-color-scheme:dark){label{color:#aaa}}
p,li{font-size:13px;color:#666;line-height:1.55}
@media(prefers-color-scheme:dark){p,li{color:#aaa}}
ul{margin:.5rem 0 0 1.1rem}
li{margin-bottom:.4rem}
h3{font-size:13px;font-weight:600;margin:1rem 0 0;color:#111}
@media(prefers-color-scheme:dark){h3{color:#eee}}
a{color:#185fa5;text-decoration:none}
a:hover{text-decoration:underline}
@media(prefers-color-scheme:dark){a{color:#85b7eb}}
.info-text{display:none;font-size:12px;color:#888;margin-top:6px}
.back-link{display:flex;align-items:center;gap:8px;font-size:13px;color:#185fa5;text-decoration:none}
.back-link:hover{text-decoration:underline}
@media(prefers-color-scheme:dark){.back-link{color:#85b7eb}}
</style>
</head>
<body>)rawliteral" + PAGE_SPRITE + R"rawliteral(
<div class="page">
  <div class="page-title">
    <svg class="ic"><use href="#i-device-speaker"/></svg>
    <h1>Auto-expand (optional)</h1>
  </div>

  <div class="card">
    <div class="card-header"><svg class="ic"><use href="#i-device-speaker"/></svg><h2>Speaker</h2></div>
    <div class="status-row">
      <span class="status-label">Selected speaker</span>
      <span class="chip" id="current">)rawliteral" + (playbackName.length()
        ? playbackName + (playbackSerial.length() ? " - " + playbackSerial : String(""))
        : String("None")) + R"rawliteral(</span>
    </div>
    <label for="speaker">Choose a speaker</label>
    <select id="speaker">
      <option value="">None</option>
    </select>
    <div class="input-row">
      <button class="btn" id="scan-btn"><svg class="ic"><use href="#i-radar-2"/></svg>&nbsp;Start product scan</button>
    </div>
    <span class="info-text" id="note">Scanning the network &mdash; this takes around 10 seconds&hellip;</span>
    <div class="input-row">
      <button class="btn" id="save-btn">Save</button>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><svg class="ic"><use href="#i-circle-check"/></svg><h2>About this setting</h2></div>
    <p>Some products, for example Beosound Core and Beoconnect Core, can be set up as a source without its own speakers. Auto-expand lets the adaptor expand the experience to another speaker automatically when the deck starts playing.</p><br><p><b>To disable this feature, select "None" from the dropdown and press Save.</b></p>
    <h3>What it does</h3>
    <ul>
      <li>When the deck starts playing, the music will be expanded to the selected speaker.</li>
      <li>When the deck is set to standby, the speaker leaves again.</li>
    </ul>
    <h3>What it does not do</h3>
    <ul>
      <li><strong>Only one speaker.</strong> To play on more speakers, expand from the Bang &amp; Olufsen app or Home Assistant instead.</li>
      <li><strong>No control from the selected speaker.</strong> Use Beoremote Halo, the <a href="/">BGAdaptor front page</a>, or Home Assistant to control playback.</li>
      <li><strong>It takes over any ongoing experiences.</strong> If the selected speaker is playing something else, it will be interrupted by the BGAdaptor regardless, once the deck starts.</li>
    </ul>
  </div>

  <a href="/" class="card back-link">
    <svg class="ic" style="font-size:16px"><use href="#i-arrow-left"/></svg>
    Back to main page
  </a>
</div>

<script>
let found={};
let savedJid=)rawliteral" + String("'") + playbackJid + String("'") + R"rawliteral(;

// Mirror the front page: the button that performs the pending action is
// highlighted, so an unsaved selection is obvious.
function refreshSaveHighlight(){
  let dirty=document.getElementById('speaker').value!==savedJid;
  document.getElementById('save-btn').classList.toggle('btn-highlight',dirty);
}
document.getElementById('speaker').addEventListener('change',refreshSaveHighlight);
document.getElementById('scan-btn').addEventListener('click',function(){
  let btn=this,note=document.getElementById('note');
  btn.disabled=true;
  btn.classList.add('scanning');
  btn.innerHTML='<svg class="ic"><use href="#i-loader-2"/></svg>&nbsp;Scanning\u2026';
  note.style.display='block';
  fetch('/discover-speakers').then(r=>r.json()).then(d=>{
    let sel=document.getElementById('speaker');
    sel.innerHTML='<option value="">None</option>';
    ((d&&d.devices)||[]).forEach(dev=>{
      if(!dev.jid)return;                    // no JID means it cannot be expanded to
      found[dev.jid]={name:dev.name,serial:dev.serial||''};
      let o=document.createElement('option');
      o.value=dev.jid;
      o.textContent=(dev.serial? dev.name+' - '+dev.serial : dev.name)+' ('+dev.ip+')';
      sel.appendChild(o);
    });
    if(savedJid&&found[savedJid])sel.value=savedJid;   // keep the saved speaker selected
    refreshSaveHighlight();
  }).catch(()=>{})
  .finally(()=>{
    btn.disabled=false;
    btn.classList.remove('scanning');
    btn.innerHTML='<svg class="ic"><use href="#i-radar-2"/></svg>&nbsp;Start product scan';
    note.style.display='none';
  });
});

document.getElementById('save-btn').addEventListener('click',function(){
  let jid=document.getElementById('speaker').value,d=found[jid]||{name:'',serial:''};
  fetch('/update-playback-speaker?jid='+encodeURIComponent(jid)
        +'&name='+encodeURIComponent(d.name)+'&serial='+encodeURIComponent(d.serial))
    .then(()=>{
      document.getElementById('current').textContent=
        d.name ? (d.serial ? d.name+' - '+d.serial : d.name) : 'None';
      savedJid=jid;
      let b=document.getElementById('save-btn');
      b.classList.remove('btn-highlight');
      b.textContent='Saved';
      setTimeout(()=>{b.textContent='Save';refreshSaveHighlight();},1500);
    });
});
</script>
</body>
</html>
)rawliteral");
}

void handleUpdatePlaybackSpeaker() {
    // A different speaker means a different expansion. Release whatever is
    // currently joined (this still uses the old JID) and clear the flag —
    // otherwise expandToPlaybackSpeaker() sees speakerExpanded and skips the
    // newly selected speaker until the next reboot.
    if (speakerExpanded) unexpandPlaybackSpeaker();
    speakerExpanded = false;

    playbackJid  = server.hasArg("jid")  ? server.arg("jid")  : "";
    playbackName = server.hasArg("name") ? server.arg("name") : "";
    playbackSerial = server.hasArg("serial") ? server.arg("serial") : "";
    preferences.putString("playbackJid", playbackJid);
    preferences.putString("playbackName", playbackName);
    preferences.putString("playbackSerial", playbackSerial);
    Serial.println(playbackJid.length() ? "Auto-expand speaker set to " + playbackName
                                        : "Auto-expand cleared");
    server.send(200, "text/plain", "OK");
}

// Peer adaptor sub-page: link a second BGAdaptor so its deck gets its own
// Halo page. Scans via /discover-peers (which excludes this adaptor).
void handlePeerConfig() {
    server.send(200, "text/html", String(R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Peer adaptor</title>
<style>)rawliteral") + PAGE_ICON_CSS + R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
@media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
.page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
.page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
.page-title .ic{font-size:22px;color:#666}
.page-title h1{font-size:18px;font-weight:500}
.card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
@media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
.card-header{display:flex;align-items:center;gap:10px;margin-bottom:1rem}
.card-header .ic{font-size:18px;color:#888}
.card-header h2{font-size:15px;font-weight:500}
.status-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:.65rem}
.status-label{font-size:13px;color:#666}
@media(prefers-color-scheme:dark){.status-label{color:#aaa}}
.chip{font-size:12px;font-family:monospace;color:#666;background:#f5f5f5;padding:2px 8px;border-radius:4px}
@media(prefers-color-scheme:dark){.chip{background:#333;color:#bbb}}
.input-row{display:flex;gap:8px;margin-top:8px}
.btn{height:36px;padding:0 14px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111;cursor:pointer;white-space:nowrap;display:inline-flex;align-items:center;justify-content:center;line-height:1}
.btn:hover{background:#f5f5f5}
@media(prefers-color-scheme:dark){.btn{background:#2a2a2a;border-color:#444;color:#eee}.btn:hover{background:#333}}
.btn.scanning{animation:scanPulse 1.3s ease-in-out infinite}
@keyframes scanPulse{0%,100%{background:#fff;border-color:#ddd;color:#666}50%{background:#e1f5ee;border-color:#1D9E75;color:#0f6e56}}
@media(prefers-color-scheme:dark){@keyframes scanPulse{0%,100%{background:#2a2a2a;border-color:#444;color:#aaa}50%{background:#1e3d34;border-color:#1D9E75;color:#7fd9bb}}}
@media(prefers-reduced-motion:reduce){.btn.scanning{animation:none;border-color:#1D9E75}}
.btn-highlight{background:#1D9E75;border-color:#1D9E75;color:#fff}
.btn-highlight:hover{background:#178a65}
@media(prefers-color-scheme:dark){.btn-highlight{background:#1D9E75;border-color:#1D9E75;color:#fff}.btn-highlight:hover{background:#178a65}}
select{width:100%;height:34px;padding:0 10px;font-size:13px;border:1px solid #ddd;border-radius:8px;background:#fff;color:#111}
@media(prefers-color-scheme:dark){select{background:#1a1a1a;border-color:#444;color:#eee}}
label{font-size:13px;color:#666;display:block;margin-bottom:6px}
@media(prefers-color-scheme:dark){label{color:#aaa}}
p,li{font-size:13px;color:#666;line-height:1.55}
@media(prefers-color-scheme:dark){p,li{color:#aaa}}
ul{margin:.5rem 0 0 1.1rem}
li{margin-bottom:.4rem}
h3{font-size:13px;font-weight:600;margin:1rem 0 0;color:#111}
@media(prefers-color-scheme:dark){h3{color:#eee}}
a{color:#185fa5;text-decoration:none}
a:hover{text-decoration:underline}
@media(prefers-color-scheme:dark){a{color:#85b7eb}}
.info-text{display:none;font-size:12px;color:#888;margin-top:6px}
.toggle-row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:.35rem}
.back-link{display:flex;align-items:center;gap:8px;font-size:13px;color:#185fa5;text-decoration:none}
.back-link:hover{text-decoration:underline}
@media(prefers-color-scheme:dark){.back-link{color:#85b7eb}}
</style>
</head>
<body>)rawliteral" + PAGE_SPRITE + R"rawliteral(
<div class="page">
  <div class="page-title">
    <svg class="ic"><use href="#i-server"/></svg>
    <h1>Peer adaptor (optional)</h1>
  </div>

  <div class="card">
    <div class="card-header"><svg class="ic"><use href="#i-server"/></svg><h2>Adaptor</h2></div>
    <div class="status-row">
      <span class="status-label">Linked peer adaptor</span>
      <span class="chip" id="current">)rawliteral" + (peerIP.length()
        ? (peerName.length() ? peerName + " - " + peerIP : peerIP)
        : String("None")) + R"rawliteral(</span>
    </div>
    <div class="status-row">
      <span class="status-label">Name reported by the peer</span>
      <span class="chip">)rawliteral" + String(!peerIP.length() ? "-"
        : peerTitle.length() ? peerTitle : String("From deck type")) + R"rawliteral(</span>
    </div>
    <div class="status-row">
      <span class="status-label">Deck reported by the peer</span>
      <span class="chip">)rawliteral" + String(!peerIP.length() ? "-"
        : peerDeck == DEVICE_RECORD ? "Record player"
        : peerDeck == DEVICE_TAPE   ? "Tape deck" : "CD player") + R"rawliteral(</span>
    </div>
    <label for="peer">Choose an adaptor</label>
    <select id="peer">
      <option value="">None</option>)rawliteral" + (peerIP.length()
        ? "<option value=\"" + peerIP + "\" selected>" +
          (peerTitle.length() ? peerTitle : peerName.length() ? peerName : peerIP) +
          " (" + peerIP + ")</option>"
        : String("")) + R"rawliteral(
    </select>
    <div class="input-row">
      <button class="btn" id="scan-btn"><svg class="ic"><use href="#i-radar-2"/></svg>&nbsp;Start adaptor scan</button>
    </div>
    <span class="info-text" id="note">Scanning the network &mdash; this takes around 10 seconds&hellip;</span>
    <div class="input-row">
      <button class="btn" id="save-btn">Save</button>)rawliteral" + (peerIP.length()
        ? "<a href=\"http://" + peerIP + "/\" target=\"_blank\" rel=\"noopener\">"
          "<button class=\"btn\" type=\"button\">Open peer's settings page</button></a>"
        : String("")) + R"rawliteral(
    </div>
  </div>

  <div class="card">
    <div class="card-header"><svg class="ic"><use href="#i-circle-check"/></svg><h2>Stop the other deck automatically</h2></div>
    <p>Turn this setting on to stop the other deck when one starts.</p>
    <div class="toggle-row">
      <span class="status-label">Auto-stop</span>
      <button class="btn" id="autostop-btn">)rawliteral" + String(peerAutoStop ? "On" : "Off") + R"rawliteral(</button>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><svg class="ic"><use href="#i-circle-check"/></svg><h2>About this setting</h2></div>
    <p>The peer functionality is an advanced feature that lets you control two BGAdaptors using one Beoremote Halo. In conjunction with the Auto-expand feature, you can make two adaptors act as if they were connected to the same speaker.</p>
    <p>Prerequisites:</p>
    <ul>
        <li>A "primary" BGAdaptor connected to a deck, Bang & Olufsen product and Beoremote Halo.</li>
        <li>A "secondary" (or peer) BGAdaptor connected to a deck and another Bang & Olufsen product.</li>
        <li>Both adaptors must be on the same network, since Beolink Multiroom is used to distribute audio.</li>
        <li>Setup Auto-expand on the peer BGAdaptor. Point to the Bang & Olufsen product the primary BGAdaptor is connected to (given it has speakers connected). Alternatively, setup Auto-expand on both BGAdaptors to point to a third Bang & Olufsen product.
    </ul>
    <h3>What it does</h3>
    <ul>
      <li>Adds a second player controls page on Beoremote Halo, corresponding to the deck connected to the BGAdaptor peer.</li>
      <li>Adds player controls from the peer deck on the "primary" BGAdaptors front page.</li>
      <li>Shows the peer's real playing state on that page, pushed live as it changes.</li>
      <li>It stops the other deck when one starts, if the Auto-stop setting is enabled.</li>
    </ul>
    <h3>What it does not do</h3>
    <ul>
      <li>Starting a deck on the decks physical user interface will not stop the other deck, regardless of setting.</li>
      <li>If volume control is enabled on the "primary" BGAdaptor, it will not control the volume of the peer deck.</li>
    </ul>
  </div>

  <a href="/" class="card back-link">
    <svg class="ic" style="font-size:16px"><use href="#i-arrow-left"/></svg>
    Back to main page
  </a>
</div>

<script>
let found={};
let savedIP=)rawliteral" + String("'") + peerIP + String("'") + R"rawliteral(;
// Seed from what is already stored, so saving without rescanning keeps the
// peer's name and id rather than blanking them.
if(savedIP)found[savedIP]={name:)rawliteral" + String("'") + peerName + String("'") + R"rawliteral(,
                           id:)rawliteral" + String("'") + peerId + String("'") + R"rawliteral(};
let autoStop=)rawliteral" + String(peerAutoStop ? "true" : "false") + R"rawliteral(;

function refreshSaveHighlight(){
  let dirty=document.getElementById('peer').value!==savedIP;
  document.getElementById('save-btn').classList.toggle('btn-highlight',dirty);
}
document.getElementById('peer').addEventListener('change',refreshSaveHighlight);

document.getElementById('scan-btn').addEventListener('click',function(){
  let btn=this,note=document.getElementById('note');
  btn.disabled=true;
  btn.classList.add('scanning');
  btn.innerHTML='<svg class="ic"><use href="#i-loader-2"/></svg>&nbsp;Scanning\u2026';
  note.style.display='block';
  fetch('/discover-peers').then(r=>r.json()).then(d=>{
    let sel=document.getElementById('peer');
    sel.innerHTML='<option value="">None</option>';
    ((d&&d.devices)||[]).forEach(dev=>{
      if(!dev.ip)return;
      found[dev.ip]={name:dev.name||'',id:dev.serial||''};
      let o=document.createElement('option');
      o.value=dev.ip;
      o.textContent=(dev.name? dev.name+' ' : '')+'('+dev.ip+')';
      sel.appendChild(o);
    });
    // An offline peer will not answer the scan. Dropping it from the list
    // would silently turn the stored link into an unsaved "None".
    if(savedIP&&!sel.querySelector('option[value="'+savedIP+'"]')){
      let o=document.createElement('option');
      o.value=savedIP;
      o.textContent=(found[savedIP]&&found[savedIP].name? found[savedIP].name+' ' : '')
                    +'('+savedIP+', offline)';
      sel.appendChild(o);
    }
    if(savedIP)sel.value=savedIP;
    refreshSaveHighlight();
  }).catch(()=>{})
  .finally(()=>{
    btn.disabled=false;
    btn.classList.remove('scanning');
    btn.innerHTML='<svg class="ic"><use href="#i-radar-2"/></svg>&nbsp;Start adaptor scan';
    note.style.display='none';
  });
});

document.getElementById('save-btn').addEventListener('click',function(){
  let ip=document.getElementById('peer').value,d=found[ip]||{name:'',id:''};
  fetch('/update-peer?ip='+encodeURIComponent(ip)
        +'&name='+encodeURIComponent(d.name)+'&id='+encodeURIComponent(d.id))
    .then(()=>{
      document.getElementById('current').textContent=
        ip ? (d.name ? d.name+' - '+ip : ip) : 'None';
      savedIP=ip;
      let b=document.getElementById('save-btn');
      b.classList.remove('btn-highlight');
      b.textContent='Saved';
      // The deck chip is rendered server-side, so reload to show what the
      // peer actually reported rather than leaving a stale value on screen.
      setTimeout(()=>{location.reload();},1200);
    });
});

document.getElementById('autostop-btn').addEventListener('click',function(){
  autoStop=!autoStop;
  this.textContent=autoStop?'On':'Off';
  fetch('/update-peer-autostop?enabled='+(autoStop?'true':'false'));
});
</script>
</body>
</html>
)rawliteral");
}

void handleUpdatePeer() {
    String ip = server.hasArg("ip") ? server.arg("ip") : "";
    if (ip.length() > 0 && !isValidIPAddress(ip)) {
        server.send(400, "text/plain", "Invalid peer IP");
        return;
    }

    peerSetLink(ip,
                server.hasArg("name") ? server.arg("name") : "",
                server.hasArg("id")   ? server.arg("id")   : "");
    server.send(200, "text/plain", "OK");
}

void handleUpdatePeerAutoStop() {
    if (server.hasArg("enabled")) {
        peerAutoStop = (server.arg("enabled") == "true");
        preferences.putBool("peerAutoStop", peerAutoStop);
        server.send(200, "text/plain", "OK");
        return;
    }
    server.send(400, "text/plain", "Missing value");
}

void handleStatus() {
    JsonDocument doc;
    doc["platform"] = platform == PLATFORM_MOZART ? "mozart" : "ase";
    doc["product_ip"] = productIP;
    doc["product_serial"] = productSerial;
    doc["product_name"] = productName;
    doc["playback_speaker"] = playbackName;
    doc["playback_jid"] = playbackJid;
    doc["beogram_state"] = beogramStateText;
    doc["beogram_track"] = beogramTrack;
    doc["beogram_playing"] = beogramPlaying;
    doc["product_connected"] = productConnected();
    doc["halo_ip"] = haloIP;
    doc["halo_serial"] = haloSerial;
    doc["halo_ws_connected"] = haloClient.available();
    doc["firmware"] = FIRMWARE_VERSION;
    doc["device_type"] = deviceType == DEVICE_RECORD ? "record" : deviceType == DEVICE_TAPE ? "tape" : "cd";
    doc["feature_enabled"] = haloControls;
    doc["halo_play_icon"] = haloPlayIcon;
    doc["halo_volume_controls"] = haloVolumeControls;
    doc["adaptor_name"] = adaptorName;
    doc["driven_by"] = drivenByPeer;
    doc["mqtt_connected"] = mqttConnected;
    doc["peer_ip"] = peerIP;
    doc["peer_name"] = peerName;
    doc["peer_title"] = peerTitle;
    doc["peer_deck"] = peerDeck == DEVICE_RECORD ? "record" : peerDeck == DEVICE_TAPE ? "tape" : "cd";
    doc["peer_online"] = peerOnline;
    doc["peer_playing"] = peerPlaying;
    doc["peer_state"] = peerStateText;
    doc["peer_track"] = peerTrack;
    doc["peer_auto_stop"] = peerAutoStop;
    doc["trigger_source"] = triggerSource;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

void handleResetWifi() {
    server.send(200, "text/html", R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>WiFi Reset</title>
        <style>)rawliteral" PAGE_ICON_CSS R"rawliteral(
            *{box-sizing:border-box;margin:0;padding:0}
            body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
            @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
            .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
            .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
            .page-title .ic{font-size:22px;color:#666}
            .page-title h1{font-size:18px;font-weight:500}
            .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
            @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
            .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
            .card-header .ic{font-size:18px;color:#a32d2d}
            .card-header h2{font-size:15px;font-weight:500}
            .sub{font-size:13px;color:#888}
        </style>
        </head>
        <body>)rawliteral" PAGE_SPRITE R"rawliteral(
        <div class="page">
        <div class="page-title">
            <svg class="ic"><use href="#i-wifi"/></svg>
            <h1>WiFi</h1>
        </div>
        <div class="card">
            <div class="card-header">
            <svg class="ic"><use href="#i-wifi-off"/></svg>
            <h2>WiFi settings cleared</h2>
            </div>
            <p class="sub">Restarting in AP mode&hellip; Connect to <strong>BGAdaptor</strong> to reconfigure.</p>
        </div>
        </div>
        </body>
        </html>
        )rawliteral");
    wm.resetSettings();
    delay(1000);
    ESP.restart(); 
}

// Full reset: wipe every stored preference and the WiFi credentials, then
// restart. The adaptor comes back up in AP mode with nothing configured.
void handleFactoryReset() {
    server.send(200, "text/html", R"rawliteral(
        <!DOCTYPE html>
        <html lang="en">
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>Reset</title>
        <style>)rawliteral" PAGE_ICON_CSS R"rawliteral(
            *{box-sizing:border-box;margin:0;padding:0}
            body{font-family:system-ui,sans-serif;background:#f0f0f0;padding:1.5rem 1rem;color:#111}
            @media(prefers-color-scheme:dark){body{background:#1a1a1a;color:#eee}}
            .page{max-width:560px;margin:0 auto;display:flex;flex-direction:column;gap:1rem}
            .page-title{display:flex;align-items:center;gap:10px;padding:.25rem 0 .5rem}
            .page-title .ic{font-size:22px;color:#666}
            .page-title h1{font-size:18px;font-weight:500}
            .card{background:#fff;border:1px solid #e0e0e0;border-radius:12px;padding:1.25rem 1.5rem}
            @media(prefers-color-scheme:dark){.card{background:#252525;border-color:#333}}
            .card-header{display:flex;align-items:center;gap:10px;margin-bottom:.5rem}
            .card-header .ic{font-size:18px;color:#a32d2d}
            .card-header h2{font-size:15px;font-weight:500}
            .sub{font-size:13px;color:#888}
        </style>
        </head>
        <body>)rawliteral" PAGE_SPRITE R"rawliteral(
        <div class="page">
        <div class="page-title">
            <svg class="ic"><use href="#i-refresh-alert"/></svg>
            <h1>Reset</h1>
        </div>
        <div class="card">
            <div class="card-header">
            <svg class="ic"><use href="#i-settings-off"/></svg>
            <h2>All settings cleared</h2>
            </div>
            <p class="sub">Restarting in AP mode&hellip; Close this window and connect to the <strong>BGAdaptor</strong> hotspot to set it up again.</p>
        </div>
        </div>
        </body>
        </html>
        )rawliteral");

    // A clean close here lets the peer notice at once, via the same closed
    // socket it already watches for. Otherwise ESP.restart() below just cuts
    // the connection, and the peer is left thinking it is still driven by
    // this adaptor — with its own Halo card disabled — until it works that
    // out on its own.
    if (peerConfigured()) peerSetLink("", "", "");

    preferences.clear();   // product, Halo, MQTT, deck type, trigger source
    wm.resetSettings();    // WiFi credentials
    delay(1000);
    ESP.restart();
}

void registerWebRoutes() {
    // ── Web server routes ───────────────────────────────────────────
    server.on("/update-source", HTTP_GET, handleUpdateTriggerSource);
    server.on("/update-platform", HTTP_GET, handleUpdatePlatform);
    server.on("/discover", HTTP_GET, handleDiscover);
    server.on("/discover-halo", HTTP_GET, handleDiscoverHalo);
    server.on("/discover-speakers", HTTP_GET, handleDiscoverSpeakers);
    server.on("/playback-speaker", HTTP_GET, handlePlaybackSpeaker);
    server.on("/update-playback-speaker", HTTP_GET, handleUpdatePlaybackSpeaker);
    server.on("/discover-peers", HTTP_GET, handleDiscoverPeers);
    server.on("/peer", HTTP_GET, handlePeerConfig);
    server.on("/update-peer", HTTP_GET, handleUpdatePeer);
    server.on("/update-peer-autostop", HTTP_GET, handleUpdatePeerAutoStop);
    
    server.on("/settings/reset-wifi", HTTP_GET, handleResetWifi);
    server.on("/settings/factory-reset", HTTP_GET, handleFactoryReset);
    
    server.on("/mqtt", HTTP_GET, handleMqttConfig);
    server.on("/mqtt", HTTP_POST, handleMqttUpdate);
    server.on("/mqtt/reset", HTTP_GET, handleMqttReset);    
    
    server.on("/command/play", HTTP_POST, []() {
        sendHexCommand(PLAY);  // PLAY
        server.send(200, "application/json", "{\"status\":\"Play command sent\"}");
    });
    
    server.on("/command/stop", HTTP_POST, []() {
        sendHexCommand(STOP);  // STOP
        server.send(200, "application/json", "{\"status\":\"Stop command sent\"}");
    });
    
    server.on("/command/next", HTTP_POST, []() {
        sendHexCommand(NEXT);  // NEXT
        server.send(200, "application/json", "{\"status\":\"Next command sent\"}");
    });
    
    server.on("/command/prev", HTTP_POST, []() {
        sendHexCommand(PREVIOUS);  // PREVIOUS
        server.send(200, "application/json", "{\"status\":\"Previous command sent\"}");
    });
    
    server.on("/command/standby", HTTP_POST, []() {
        sendHexCommand(STANDBY);  // STANDBY
        server.send(200, "application/json", "{\"status\":\"Standby command sent\"}");
    });
    
    
    // Player controls for a linked peer's deck. Queued and sent from the
    // loop like every other peer command, so an unreachable peer cannot hold
    // up the web server.
    server.on("/peer-command/play",    HTTP_POST, []() { peerSendCommand("play");    server.send(200, "application/json", "{\"status\":\"queued\"}"); });
    server.on("/peer-command/stop",    HTTP_POST, []() { peerSendCommand("stop");    server.send(200, "application/json", "{\"status\":\"queued\"}"); });
    server.on("/peer-command/next",    HTTP_POST, []() { peerSendCommand("next");    server.send(200, "application/json", "{\"status\":\"queued\"}"); });
    server.on("/peer-command/prev",    HTTP_POST, []() { peerSendCommand("prev");    server.send(200, "application/json", "{\"status\":\"queued\"}"); });
    server.on("/peer-command/standby", HTTP_POST, []() { peerSendCommand("standby"); server.send(200, "application/json", "{\"status\":\"queued\"}"); });

    server.on("/", handleRoot);
    server.on("/update", HTTP_GET, handleUpdate);
    server.on("/update-halo", HTTP_GET, handleUpdateHalo);
    server.on("/update-feature", HTTP_GET, handleUpdateFeature);
    server.on("/update-devicetype", HTTP_GET, handleUpdateDeviceType);
    server.on("/update-haloplayicon", HTTP_GET, handleUpdateHaloPlayIcon);
    server.on("/update-halovolume", HTTP_GET, handleUpdateHaloVolumeControls);
    server.on("/update-name", HTTP_GET, handleUpdateAdaptorName);
    server.on("/status", handleStatus);
    server.on("/update-ota", HTTP_POST, handleOTAResult, handleOTAUpdate); 
    server.begin();
}