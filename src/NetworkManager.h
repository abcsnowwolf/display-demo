#pragma once

#include <WiFi.h>

#include <atomic>

enum class NetworkState {
    INIT,
    CONNECTING,
    CONNECTED,
    CONNECTION_FAILED,
    AP_MODE
};

class NetworkManager {
public:
    NetworkManager();
    void begin();
    void loop();
    bool connect(const String& ssid, const String& password);
    bool isConnected() const;
    String getSSID() const;
    String getIPAddress() const;
    NetworkState getState() const;

private:
    static constexpr uint32_t CONNECT_TIMEOUT = 15000; // 15 seconds
    static constexpr uint8_t MAX_CONNECT_ATTEMPTS = 3;
    static constexpr uint16_t RECONNECT_INTERVAL = 5000; // 5 seconds
    static constexpr size_t MAX_SSID_LENGTH = 32;     // 802.11 SSID limit
    static constexpr size_t MAX_PASSWORD_LENGTH = 64; // WPA2 passphrase limit

    /* 以下成员只由主循环任务读写。 */
    uint8_t connectAttempts;
    uint32_t lastConnectAttempt;
    String requestedSSID;
    String requestedPassword;
    String savedSSID;  // from the driver's NVS config, for display only
    bool statusPrinted;

    // get_wifi_status 在 BLE 任务上读它, 主循环写它。
    std::atomic<NetworkState> state;

    /* config_wifi 跑在 ESP-MCP 的 mcp_ble_rx 任务上, WiFi 状态机跑在主循环
     * 任务上。BLE 侧只把凭据拷进这个定长槽位并置位标志, String 赋值和
     * WiFi.begin() 全部留给主循环 —— 与 LCD 刷新同一套"跨任务只提交请求"的
     * 做法。用定长缓冲 + 自旋锁而不是 String, 是为了让临界区里不发生分配。 */
    portMUX_TYPE pendingLock;
    char pendingSSID[MAX_SSID_LENGTH + 1];
    char pendingPassword[MAX_PASSWORD_LENGTH + 1];
    std::atomic<bool> pendingRequest;

    void applyPendingRequest();
    void startConnection(const char* ssid = nullptr, const char* password = nullptr);
    void printConnectionStatus();
};
