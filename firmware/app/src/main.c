#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>

LOG_MODULE_REGISTER(micnote_firmware, LOG_LEVEL_INF);

// 1 second sleep time
#define SLEEP_TIME_MS 1000

// The device tree node identifier for the led0 alias.
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void) {
    LOG_INF("Micnote %s", APP_VERSION_STRING);

    int ret;
    bool led_state = true;

    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED0 Not Ready");
        return 1;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED0: %d", ret);
        return 1;
    }

    while (true) {
        ret = gpio_pin_toggle_dt(&led);
        if (ret < 0) {
            LOG_ERR("Failed to toggle LED0: %d", ret);
            return 1;
        }

        led_state = !led_state;
        LOG_INF("LED State: %d", led_state);
        k_msleep(SLEEP_TIME_MS);
    }
    return 0;
}
