// Activity lights on the LED matrix: left-middle blinks blue while an MCP
// request is handled, right-middle flashes green once per router connection.
#pragma once

void activity_start(void);
void activity_mcp(int begin);   // 1 = request started, 0 = finished
void activity_router(void);     // a connection to the router is being made
