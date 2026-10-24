#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEMCPServer.h>
#include <HttpMCPServer.h>

#include <atomic>

#include "NetworkManager.h"

class MCPService {
public:
    explicit MCPService(NetworkManager& networkManager, uint16_t httpPort = 9000);
    ~MCPService();

    MCPService(const MCPService&) = delete;
    MCPService& operator=(const MCPService&) = delete;

    void begin();
    void loop();
    bool startWifiTransport();
    void ensureWifiTransport();
    String getHttpUrl() const;
    String bleName() const;

    JsonDocument configureWiFi(JsonVariantConst params);
    JsonDocument getWiFiStatus(JsonVariantConst params) const;

private:
    void registerBleTools();
    void registerWifiTools(HttpMCPServer& server);
    String networkStateToString(NetworkState state) const;
    String ipToString(const IPAddress& ip) const;

    NetworkManager& networkManager;
    uint16_t httpPort;
    BLEMCPServer bleServer;
    String bleDeviceName;  // advertised name, set in begin()

    /* BLE 工具处理器跑在 ESP-MCP 的 mcp_ble_rx 任务上, 而 HTTP 传输由主循环
     * 任务启动, 所以 get_wifi_status 读这个指针和 startWifiTransport() 写它是
     * 并发的。原子指针 + "begin() 成功后才发布" 保证 http_mcp_started 报 true
     * 时 /mcp 已经能服务, 也避免启动失败路径的 delete 与 BLE 侧的读撞上。 */
    std::atomic<HttpMCPServer*> httpServer;
    bool bleStarted;
    bool bleToolsRegistered;
    unsigned long lastWifiTransportCheck;
    unsigned long lastHeapReport;
};
