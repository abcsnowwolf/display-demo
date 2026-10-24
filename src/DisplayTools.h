#pragma once

#include "MCPServer.h"

// All display tools, served over HTTP once WiFi is up.
void registerDisplayTools(MCPServerBase& server);

/* get_description alone: BLE only does provisioning, but the app's device
 * identity protocol needs get_description on every transport. */
void registerDescriptionTool(MCPServerBase& server);
