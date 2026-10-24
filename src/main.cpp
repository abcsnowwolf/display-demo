#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "DisplayService.h"
#include "MCPService.h"
#include "NetworkManager.h"

// MCP server port; the endpoint is http://<ip>:9000/mcp.
constexpr uint16_t MCP_HTTP_PORT = 9000;

// POSIX TZ for the on-screen clock (China Standard Time).
#ifndef DISPLAY_TZ
#define DISPLAY_TZ "CST-8"
#endif

NetworkManager networkManager;
MCPService mcpService(networkManager, MCP_HTTP_PORT);

namespace {

bool ntpStarted = false;

SystemInfo::Wifi wifiState() {
    switch (networkManager.getState()) {
        case NetworkState::CONNECTED:
            return SystemInfo::Wifi::Connected;
        case NetworkState::CONNECTING:
            return SystemInfo::Wifi::Connecting;
        case NetworkState::CONNECTION_FAILED:
            return SystemInfo::Wifi::Failed;
        default:
            return SystemInfo::Wifi::Unconfigured;
    }
}

// Publishes network state to the display; it only redraws on changes.
void syncDisplay() {
    SystemInfo info;
    info.bleName = mcpService.bleName().c_str();
    info.wifi = networkManager.isConnected() ? SystemInfo::Wifi::Connected : wifiState();
    info.ssid = networkManager.getSSID().c_str();
    info.mcpUrl = mcpService.getHttpUrl().c_str();
    display.updateSystem(info);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.println("\n==== ESP32 MCP Display ====");

    if (!display.begin()) Serial.println("[Main] display failed to start");

    // BLE MCP (provisioning) starts immediately; HTTP MCP once WiFi is up.
    mcpService.begin();
    networkManager.begin();
    mcpService.ensureWifiTransport();
    syncDisplay();
}

void loop() {
    networkManager.loop();
    mcpService.loop();

    if (!ntpStarted && networkManager.isConnected()) {
        configTzTime(DISPLAY_TZ, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
        ntpStarted = true;
    }
    syncDisplay();
    delay(100);
}
