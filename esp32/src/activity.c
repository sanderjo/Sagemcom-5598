#include "activity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "matrix.h"

#define LEVEL 5
#define TICK_MS 20
#define MCP_ROW 3
#define MCP_COL 0
#define ROUTER_ROW 3
#define ROUTER_COL 7
#define BLINK_MS 125          // blue: 125 ms on / 125 ms off
#define FLASH_ON_MS 120       // green: one 120 ms flash per connection,
#define FLASH_OFF_MS 100      // with a gap so back-to-back ones stay apart
#define MAX_PENDING 20

static volatile int s_mcp_active;
static volatile int64_t s_mcp_since;      // start of the current blinking, us
static volatile int64_t s_mcp_min_until;  // keep blinking at least until (one full flash)
static volatile int s_router_pending;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void activity_mcp(int begin)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (begin) {
        if (!s_mcp_active && now >= s_mcp_min_until) s_mcp_since = now;
        s_mcp_active++;
        if (s_mcp_min_until < s_mcp_since + BLINK_MS * 1000) s_mcp_min_until = s_mcp_since + BLINK_MS * 1000;
    } else if (s_mcp_active > 0) {
        s_mcp_active--;
    }
    portEXIT_CRITICAL(&s_lock);
}

void activity_router(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_router_pending < MAX_PENDING) s_router_pending++;
    portEXIT_CRITICAL(&s_lock);
}

static void task(void *arg)
{
    int shown_blue = 0, shown_green = 0, green = 0;
    int64_t green_until = 0, green_gap_until = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();

        portENTER_CRITICAL(&s_lock);
        int mcp = s_mcp_active || now < s_mcp_min_until;
        int64_t since = s_mcp_since;
        int start_flash = !green && now >= green_gap_until && s_router_pending > 0;
        if (start_flash) s_router_pending--;
        portEXIT_CRITICAL(&s_lock);

        int blue = mcp && ((now - since) / 1000 / BLINK_MS) % 2 == 0;
        if (start_flash) {
            green = 1;
            green_until = now + FLASH_ON_MS * 1000;
        } else if (green && now >= green_until) {
            green = 0;
            green_gap_until = now + FLASH_OFF_MS * 1000;
        }

        if (blue != shown_blue || green != shown_green) {
            matrix_set(MCP_ROW, MCP_COL, 0, 0, blue ? LEVEL : 0);
            matrix_set(ROUTER_ROW, ROUTER_COL, 0, green ? LEVEL : 0, 0);
            matrix_show();
            shown_blue = blue;
            shown_green = green;
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void activity_start(void)
{
    xTaskCreate(task, "activity", 3072, NULL, 3, NULL);
}
