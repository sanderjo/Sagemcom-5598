// Activity lights on the LED matrix: left-middle blinks blue while an MCP
// request is handled, right-middle flashes green once per router connection,
// and the top-right LED flashes green every 2 s as a heartbeat.
#pragma once

void activity_start(void);
void activity_mcp(int begin);   // 1 = request started, 0 = finished
void activity_router(void);     // a connection to the router is being made
