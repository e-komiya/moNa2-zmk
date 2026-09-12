/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>

/* Nordic pin numbers, not Arduino pin numbers or diode designators. */
#define COL_D10 NRF_GPIO_PIN_MAP(1, 15)
#define H_D2 NRF_GPIO_PIN_MAP(0, 28)
#define N_D3 NRF_GPIO_PIN_MAP(0, 29)
#define TEST_MS 60000

BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart));
static const struct device *const serial = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static atomic_t command;

static void receive(const struct device *dev, void *unused)
{
    uint8_t ch;
    ARG_UNUSED(unused);
    if (!uart_irq_update(dev)) {
        return;
    }
    while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &ch, 1) == 1) {
        if (ch == 'x' || ch == 'X') {
            atomic_set(&command, 'x');
        } else if (ch == 's' || ch == 'S') {
            /* A stop already queued wins over a start in the same batch. */
            atomic_cas(&command, 0, 's');
        }
    }
}

static void stop_column(void)
{
    /* Release the column: do not actively drive it LOW. */
    nrf_gpio_cfg_default(COL_D10);
}

static void start_column(void)
{
    nrf_gpio_pin_set(COL_D10);
    nrf_gpio_cfg(COL_D10, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
}

int main(void)
{
    stop_column();
    /* All other external columns and sensor pins remain high impedance.
     * No UICR/NFC configuration or settings storage is written. */
    const uint32_t unused_pins[] = {2, 4, 5, 9, 10, 44, 45, 46};
    for (size_t i = 0; i < ARRAY_SIZE(unused_pins); i++) {
        nrf_gpio_cfg_default(unused_pins[i]);
    }
    nrf_gpio_cfg_input(3, NRF_GPIO_PIN_PULLDOWN);  /* row0 = XIAO D1 */
    nrf_gpio_cfg_input(H_D2, NRF_GPIO_PIN_PULLDOWN);
    nrf_gpio_cfg_input(N_D3, NRF_GPIO_PIN_PULLDOWN);
    nrf_gpio_cfg_input(43, NRF_GPIO_PIN_PULLDOWN); /* row3 = XIAO D6 */

    if (!device_is_ready(serial) || usb_enable(NULL) != 0) {
        return 0;
    }
    if (uart_irq_callback_user_data_set(serial, receive, NULL) != 0) {
        return 0;
    }
    uart_irq_rx_enable(serial);

    bool active = false;
    bool connected = false;
    int64_t deadline = 0;
    int64_t next_print = 0;
    while (true) {
        uint32_t dtr = 0;
        uart_line_ctrl_get(serial, UART_LINE_CTRL_DTR, &dtr);
        int64_t now = k_uptime_get();
        if (!dtr) {
            stop_column();
            active = false;
            connected = false;
            atomic_set(&command, 0);
            k_msleep(50);
            continue;
        }
        if (!connected) {
            printk("\nMONA2-RAW-GPIO-v1 READY; right board only; s=start x=stop\n");
            printk("No HID/BLE/trackball. D10=P1.15; H_D2=P0.28; N_D3=P0.29\n");
            connected = true;
        }
        atomic_val_t cmd = atomic_set(&command, 0);
        if (cmd == 'x' || (active && now >= deadline)) {
            stop_column();
            active = false;
            printk("STOPPED: D10 high impedance\n");
        } else if (cmd == 's' && !active) {
            start_column();
            active = true;
            deadline = now + TEST_MS;
            printk("ACTIVE: D10 HIGH for max 60s; x stops; press physical N\n");
            printk("PIN_CNF28=%08x PIN_CNF29=%08x (expected 00000004)\n",
                   (unsigned int)NRF_P0->PIN_CNF[28],
                   (unsigned int)NRF_P0->PIN_CNF[29]);
            k_msleep(5);
        }
        if (now >= next_print) {
            if (active) {
                printk("RAW ACTIVE COL_D10=%u H_D2=%u N_D3=%u ROW0_D1=%u ROW3_D6=%u\n",
                       (unsigned int)nrf_gpio_pin_read(COL_D10),
                       (unsigned int)nrf_gpio_pin_read(H_D2),
                       (unsigned int)nrf_gpio_pin_read(N_D3),
                       (unsigned int)nrf_gpio_pin_read(3),
                       (unsigned int)nrf_gpio_pin_read(43));
                next_print = now + 250;
            } else {
                printk("MONA2-RAW-GPIO-v1 IDLE; send s to start (x to stop)\n");
                next_print = now + 2000;
            }
        }
        k_msleep(20);
    }
}
