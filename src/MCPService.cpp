#include "MCPService.h"

#include <WiFi.h>
#include <cstring>
#include <initializer_list>
#include <new>

#include "DisplayTools.h"

namespace {
constexpr const char* BLE_SERVER_NAME = "ESP32-Display-MCP";
// Also the mDNS hostname, so it must be a valid DNS label.
constexpr const char* HTTP_SERVER_NAME = "esp32-mcp-display";
constexpr const char* SERVER_VERSION = "1.0.0";
constexpr const char* BLE_INSTRUCTIONS = "Provision WiFi for the ESP32 MCP display with config_wifi.";
constexpr const char* HTTP_INSTRUCTIONS =
    "A 320x240 color LCD for showing information to people nearby. Use show_text for messages, show_notice "
    "for alerts, show_list for to-dos/agendas, show_metrics for numbers, show_slides to rotate screens, "
    "notify for a transient toast, and show_layout for custom designs (call get_display_info first). "
    "Chinese and English text are fully supported; text auto-fits, so prefer omitting size.";
constexpr uint32_t WIFI_TRANSPORT_CHECK_INTERVAL_MS = 3000;
constexpr uint32_t HEAP_REPORT_INTERVAL_MS = 60000;

// Schema helpers — keep outputSchema declarations readable.
Properties primitive(const String& type, const String& description) {
    Properties p;
    p.type = type;
    p.description = description;
    return p;
}

Properties stringEnum(const String& description, std::initializer_list<const char*> values) {
    Properties p;
    p.type = "string";
    p.description = description;
    for (auto v : values) {
        p.enumValues.push_back(v);
    }
    return p;
}

class ConfigureWiFiHandler : public ToolHandler {
public:
    explicit ConfigureWiFiHandler(MCPService& service) : service(service) {}

    JsonDocument call(JsonVariantConst params) override {
        return service.configureWiFi(params);
    }

private:
    MCPService& service;
};

class WiFiStatusHandler : public ToolHandler {
public:
    explicit WiFiStatusHandler(const MCPService& service) : service(service) {}

    JsonDocument call(JsonVariantConst params) override {
        return service.getWiFiStatus(params);
    }

private:
    const MCPService& service;
};
}  // namespace

MCPService::MCPService(NetworkManager& networkManager, uint16_t httpPort)
    : networkManager(networkManager),
      httpPort(httpPort),
      bleServer(BLE_SERVER_NAME, SERVER_VERSION, BLE_INSTRUCTIONS),
      httpServer(nullptr),
      bleStarted(false),
      bleToolsRegistered(false),
      lastWifiTransportCheck(0),
      lastHeapReport(0) {
}

MCPService::~MCPService() {
    delete httpServer.exchange(nullptr, std::memory_order_acq_rel);
}

void MCPService::begin() {
    registerBleTools();

    // A MAC suffix tells several displays apart in the BLE scan list.
    uint64_t mac = ESP.getEfuseMac();
    char name[32];
    snprintf(name, sizeof(name), "MCP-Display-%02X%02X", unsigned((mac >> 32) & 0xFF), unsigned((mac >> 40) & 0xFF));
    bleDeviceName = name;

    BleServerConfig bleConfig;
    bleConfig.deviceName = bleDeviceName.c_str();
    bleConfig.txPower = ESP_PWR_LVL_P9;
    bleConfig.advTxPower = ESP_PWR_LVL_P9;
    /* 100–200 ms。原来是 20–30 ms: WiFi 与 BLE 共用一个 2.4G 射频(软件共存,
     * BT 控制器和 lwIP 都在 core 0), 广播每 20 ms 抢一次时隙会明显拖慢 WiFi
     * 关联 —— 而广播只在没有 BLE 连接时才跑, 也就是开机到 HTTP MCP 起来的
     * 那段窗口。手机扫描是持续若干秒的, 150 ms 的平均发现延迟感知不到。 */
    bleConfig.advMinInterval = 0xA0;
    bleConfig.advMaxInterval = 0x140;
    bleServer.setBleConfig(bleConfig);
    bleServer.begin();
    bleStarted = true;
}

void MCPService::loop() {
    if (bleStarted) {
        bleServer.loop();
    }

    unsigned long now = millis();
    if (now - lastWifiTransportCheck >= WIFI_TRANSPORT_CHECK_INTERVAL_MS) {
        lastWifiTransportCheck = now;
        ensureWifiTransport();
    }

    /* BLE + WiFi + 两套工具注册表同时占堆, 而堆见底的表现是 AsyncTCP 静默收
     * 不下连接。留一条每分钟的水位线, 好把"HTTP 没响应"和堆对上时间。 */
    if (now - lastHeapReport >= HEAP_REPORT_INTERVAL_MS) {
        lastHeapReport = now;
        Serial.printf("[MCP] heap %u bytes, largest free block %u, min free since boot %u\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    }
}

void MCPService::registerBleTools() {
    if (bleToolsRegistered) {
        return;
    }

    /* BLE 仅保留配网工具 + get_description(app 设备身份协议要求, 缺失会导致连接失败) */
    registerDescriptionTool(bleServer);

    Tool configWifiTool;
    configWifiTool.name = "config_wifi";
    configWifiTool.description = "Configure WiFi with ssid and password, then start WiFi MCP on success";
    configWifiTool.inputSchema.type = "object";
    configWifiTool.inputSchema.hasAdditionalProperties = true;
    configWifiTool.inputSchema.additionalProperties = false;

    Properties ssidProp;
    ssidProp.type = "string";
    ssidProp.description = "WiFi SSID";
    configWifiTool.inputSchema.properties["ssid"] = ssidProp;
    configWifiTool.inputSchema.required.push_back("ssid");

    Properties passwordProp;
    passwordProp.type = "string";
    passwordProp.description = "WiFi password";
    configWifiTool.inputSchema.properties["password"] = passwordProp;

    configWifiTool.outputSchema.type = "object";
    configWifiTool.outputSchema.properties["status"] = stringEnum(
        "Result of the configuration attempt", {"connecting", "invalid"});
    configWifiTool.outputSchema.properties["ssid"] = primitive(
        "string", "SSID the connection was attempted with (present on success)");
    configWifiTool.outputSchema.properties["ip"] = primitive(
        "string", "Local IP, empty until the connection completes");
    configWifiTool.outputSchema.properties["error"] = primitive(
        "string", "Error message when status is 'invalid'");
    configWifiTool.outputSchema.properties["http_url"] = primitive(
        "string", "HTTP MCP URL once WiFi is up, empty otherwise");
    configWifiTool.outputSchema.required.push_back("status");
    configWifiTool.outputSchema.required.push_back("http_url");

    configWifiTool.handler = std::make_shared<ConfigureWiFiHandler>(*this);
    bleServer.RegisterTool(configWifiTool);

    Tool statusTool;
    statusTool.name = "get_wifi_status";
    statusTool.description = "Get current WiFi status and WiFi MCP URL";
    statusTool.inputSchema.type = "object";

    statusTool.outputSchema.type = "object";
    statusTool.outputSchema.properties["status"] = stringEnum(
        "WiFi link state", {"connected", "disconnected"});
    statusTool.outputSchema.properties["ssid"] = primitive(
        "string", "Connected SSID, empty when disconnected");
    statusTool.outputSchema.properties["ip"] = primitive(
        "string", "Local IP address, empty when disconnected");
    statusTool.outputSchema.properties["network_state"] = stringEnum(
        "NetworkManager state",
        {"idle", "connecting", "connected", "failed", "ap_mode", "unknown"});
    statusTool.outputSchema.properties["http_mcp_started"] = primitive(
        "boolean", "True when the HTTP MCP transport is running");
    statusTool.outputSchema.properties["http_url"] = primitive(
        "string", "HTTP MCP URL, empty when not started");
    statusTool.outputSchema.properties["ble_mcp_started"] = primitive(
        "boolean", "True when the BLE MCP transport is running");
    statusTool.outputSchema.required.push_back("status");
    statusTool.outputSchema.required.push_back("ssid");
    statusTool.outputSchema.required.push_back("ip");
    statusTool.outputSchema.required.push_back("network_state");
    statusTool.outputSchema.required.push_back("http_mcp_started");
    statusTool.outputSchema.required.push_back("http_url");
    statusTool.outputSchema.required.push_back("ble_mcp_started");

    statusTool.handler = std::make_shared<WiFiStatusHandler>(*this);
    bleServer.RegisterTool(statusTool);

    bleToolsRegistered = true;
}

void MCPService::registerWifiTools(HttpMCPServer& server) {
    const uint32_t heapBefore = ESP.getFreeHeap();
    registerDisplayTools(server);
    const uint32_t heapAfter = ESP.getFreeHeap();
    /* 最大可分配块比剩余总量更能预测 AsyncTCP 收不下新连接: 碎片化之后总量
     * 还很宽裕, 单块却已经喂不饱一个 PCB + 接收缓冲。 */
    Serial.printf("[MCP] HTTP tool schemas: heap %u -> %u bytes (%u used), "
                  "largest free block %u, min free since boot %u\n",
                  heapBefore, heapAfter, heapBefore - heapAfter,
                  ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

bool MCPService::startWifiTransport() {
    if (httpServer.load(std::memory_order_acquire)) {
        return true;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    /* 整个启动过程都在这个局部指针上进行, 直到 /mcp 真正能服务才发布到成员,
     * 否则 BLE 任务上的 get_wifi_status 会在工具注册的几百毫秒里就报
     * http_mcp_started: true, 失败路径的 delete 也会与它并发。 */
    HttpMCPServer* server =
        new (std::nothrow) HttpMCPServer(httpPort, HTTP_SERVER_NAME, SERVER_VERSION, HTTP_INSTRUCTIONS);
    if (!server) {
        Serial.println("[MCP] Failed to allocate HTTP MCP server");
        return false;
    }

    registerWifiTools(*server);

    /* 0.4.0 起 HttpMCPServer 构造后不再自动监听: 工具注册完成后显式 begin()，
     * 避免请求处理与工具注册竞争。 */
    if (!server->begin()) {
        Serial.println("[MCP] Failed to start HTTP MCP server");
        delete server;
        return false;
    }

    httpServer.store(server, std::memory_order_release);

    Serial.printf("[MCP] WiFi MCP started at %s\n", getHttpUrl().c_str());
    return true;
}

void MCPService::ensureWifiTransport() {
    if (!httpServer.load(std::memory_order_acquire) && WiFi.status() == WL_CONNECTED) {
        startWifiTransport();
    }
}

JsonDocument MCPService::configureWiFi(JsonVariantConst params) {
    JsonDocument result;

    const char* ssid = params["ssid"].as<const char*>();
    const char* password = params["password"].isNull() ? "" : params["password"].as<const char*>();

    if (!ssid || strlen(ssid) == 0) {
        result["status"] = "invalid";
        result["error"] = "ssid is required";
        result["http_url"] = getHttpUrl();
        return result;
    }

    Serial.printf("[MCP] BLE WiFi configuration requested for SSID: %s\n", ssid);

    if (!networkManager.connect(ssid, password)) {
        result["status"] = "invalid";
        result["error"] = "failed to start WiFi connection";
        result["http_url"] = getHttpUrl();
        return result;
    }

    result["status"] = "connecting";
    result["ssid"] = ssid;
    result["ip"] = "";
    result["http_url"] = "";

    return result;
}

JsonDocument MCPService::getWiFiStatus(JsonVariantConst params) const {
    (void)params;

    JsonDocument result;
    const bool connected = WiFi.status() == WL_CONNECTED;

    result["status"] = connected ? "connected" : "disconnected";
    result["ssid"] = connected ? WiFi.SSID() : String("");
    result["ip"] = connected ? ipToString(WiFi.localIP()) : String("");
    result["network_state"] = networkStateToString(networkManager.getState());
    result["http_mcp_started"] = httpServer.load(std::memory_order_acquire) != nullptr;
    result["http_url"] = getHttpUrl();
    result["ble_mcp_started"] = bleStarted;

    return result;
}

String MCPService::getHttpUrl() const {
    if (!httpServer.load(std::memory_order_acquire) || WiFi.status() != WL_CONNECTED) {
        return String("");
    }

    return String("http://") + ipToString(WiFi.localIP()) + ":" + String(httpPort) + "/mcp";
}

String MCPService::bleName() const {
    return bleDeviceName;
}

String MCPService::networkStateToString(NetworkState state) const {
    switch (state) {
        case NetworkState::INIT:
            return "idle";
        case NetworkState::CONNECTING:
            return "connecting";
        case NetworkState::CONNECTED:
            return "connected";
        case NetworkState::CONNECTION_FAILED:
            return "failed";
        case NetworkState::AP_MODE:
            return "ap_mode";
    }

    return "unknown";
}

String MCPService::ipToString(const IPAddress& ip) const {
    return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}
