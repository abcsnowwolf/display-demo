#pragma once
// In-memory MCP registry for host tests. Tests compile the actual
// DisplayTools.cpp and invoke its handlers; HTTP/BLE transport is not emulated.
#include <ArduinoJson.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Arduino.h"

struct Properties {
    String type, description;
    std::map<String, Properties> properties;
    std::vector<String> required, enumValues;
    std::shared_ptr<Properties> items;
};
class ToolHandler {
public:
    virtual ~ToolHandler() = default;
    virtual JsonDocument call(JsonVariantConst) = 0;
    virtual JsonDocument call(JsonVariantConst params, bool& isError) { isError = false; return call(params); }
};
struct Tool {
    String name, description;
    Properties inputSchema, outputSchema;
    std::shared_ptr<ToolHandler> handler;
};
class MCPServerBase {
public:
    std::map<String, Tool> tools;
    void RegisterTool(Tool tool) { auto name = tool.name; tools.emplace(name, std::move(tool)); }
};
