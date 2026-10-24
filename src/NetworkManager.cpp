#include "NetworkManager.h"

#include <esp_wifi.h>

#include <cstring>

NetworkManager::NetworkManager() :
      connectAttempts(0),
      lastConnectAttempt(0),
      requestedSSID(""),
      requestedPassword(""),
      statusPrinted(false),
      state(NetworkState::INIT),
      pendingLock(portMUX_INITIALIZER_UNLOCKED),
      pendingRequest(false) {
    pendingSSID[0] = '\0';
    pendingPassword[0] = '\0';
}

void NetworkManager::begin() {
    Serial.println("[Network] Initializing WiFi manager");
    // WiFi.SSID() reports the currently associated AP, not the station
    // credentials saved in NVS, so ask the driver for its stored config.
    // Without one, stay idle and leave provisioning to BLE instead of
    // spending three connection timeouts on an empty SSID.
    WiFi.mode(WIFI_STA);
    wifi_config_t conf = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK && conf.sta.ssid[0] != 0) {
        savedSSID = reinterpret_cast<const char*>(conf.sta.ssid);
        startConnection();
    } else {
        Serial.println("[Network] No saved WiFi credentials; waiting for BLE provisioning");
    }
}

void NetworkManager::loop() {
    applyPendingRequest();

    if (state != NetworkState::CONNECTING) {
        if (WiFi.status() == WL_CONNECTED && !statusPrinted) {
            state = NetworkState::CONNECTED;
            printConnectionStatus();
            statusPrinted = true;
        }
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastConnectAttempt < CONNECT_TIMEOUT) {
            return;
        }

        connectAttempts++;
        Serial.printf("[Network] WiFi connection attempt %u failed\n", connectAttempts);
        if (connectAttempts >= MAX_CONNECT_ATTEMPTS) {
            state = NetworkState::CONNECTION_FAILED;
            Serial.println("[Network] WiFi connection failed; keeping BLE configuration available");
            return;
        }

        if (millis() - lastConnectAttempt >= RECONNECT_INTERVAL) {
            startConnection(requestedSSID.length() > 0 ? requestedSSID.c_str() : nullptr,
                            requestedSSID.length() > 0 ? requestedPassword.c_str() : nullptr);
        }
        return;
    }

    state = NetworkState::CONNECTED;
    printConnectionStatus();
    statusPrinted = true;
}

/* 由 BLE 任务调用: 只提交请求, 不碰任何主循环拥有的状态。 */
bool NetworkManager::connect(const String& ssid, const String& password) {
    if (ssid.length() == 0) {
        Serial.println("[Network] Refusing WiFi connection with empty SSID");
        return false;
    }
    if (ssid.length() > MAX_SSID_LENGTH || password.length() > MAX_PASSWORD_LENGTH) {
        Serial.println("[Network] Refusing WiFi credentials longer than the 802.11 limits");
        return false;
    }

    portENTER_CRITICAL(&pendingLock);
    strlcpy(pendingSSID, ssid.c_str(), sizeof(pendingSSID));
    strlcpy(pendingPassword, password.c_str(), sizeof(pendingPassword));
    portEXIT_CRITICAL(&pendingLock);

    pendingRequest.store(true, std::memory_order_release);
    /* 让紧接着的 get_wifi_status 立刻看到 connecting, 不必等主循环这一拍。 */
    state = NetworkState::CONNECTING;
    return true;
}

/* 只在主循环任务上运行: 把 BLE 提交的凭据落到 String 上并真正发起连接。 */
void NetworkManager::applyPendingRequest() {
    if (!pendingRequest.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    char ssid[MAX_SSID_LENGTH + 1];
    char password[MAX_PASSWORD_LENGTH + 1];
    portENTER_CRITICAL(&pendingLock);
    strlcpy(ssid, pendingSSID, sizeof(ssid));
    strlcpy(password, pendingPassword, sizeof(password));
    portEXIT_CRITICAL(&pendingLock);

    requestedSSID = ssid;
    requestedPassword = password;
    connectAttempts = 0;
    startConnection(requestedSSID.c_str(), requestedPassword.c_str());
}

bool NetworkManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

String NetworkManager::getSSID() const {
    if (isConnected()) return WiFi.SSID();
    return requestedSSID.length() > 0 ? requestedSSID : savedSSID;
}

String NetworkManager::getIPAddress() const {
    return isConnected() ? WiFi.localIP().toString() : String("");
}

NetworkState NetworkManager::getState() const {
    return state;
}

void NetworkManager::startConnection(const char* ssid, const char* password) {
    WiFi.persistent(true);
    WiFi.mode(WIFI_STA);

    statusPrinted = false;
    state = NetworkState::CONNECTING;
    lastConnectAttempt = millis();

    if (ssid && strlen(ssid) > 0) {
        Serial.printf("[Network] Connecting to WiFi SSID: %s\n", ssid);
        WiFi.begin(ssid, password ? password : "");
    } else {
        Serial.println("[Network] Connecting with saved WiFi credentials");
        WiFi.begin();
    }
}

void NetworkManager::printConnectionStatus() {
    Serial.println("======================================");
    Serial.println("           WiFi CONNECTION SUCCESS");
    Serial.println("======================================");
    
    Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("Subnet Mask: %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("DNS Server: %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
    Serial.printf("Signal Strength: %d dBm\n", WiFi.RSSI());
    Serial.printf("Channel: %d\n", WiFi.channel());
    
    // 连接质量评估
    int rssi = WiFi.RSSI();
    String quality;
    if (rssi > -50) {
        quality = "Excellent";
    } else if (rssi > -60) {
        quality = "Good";
    } else if (rssi > -70) {
        quality = "Fair";
    } else {
        quality = "Weak";
    }
    Serial.printf("Connection Quality: %s\n", quality.c_str());
    
    Serial.printf("Connection Time: %lu ms\n", millis() - lastConnectAttempt);
    Serial.printf("Connect Attempts: %d\n", connectAttempts + 1);
    
    Serial.println("======================================");
    Serial.println("Device is ready for MCP connections!");
    Serial.println("======================================");
}
