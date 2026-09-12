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
#define LOW_TEST_MS 15000
static const uint32_t other_columns[] = {46, 45, 44, 10};

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
        } else if (ch == 'l' || ch == 'L') {
            atomic_cas(&command, 0, 'l');
        }
    }
}

static void stop_column(void)
{
    /* Release the column: do not actively drive it LOW. */
    nrf_gpio_cfg_default(COL_D10);
    for (size_t i = 0; i < ARRAY_SIZE(other_columns); i++) {
        nrf_gpio_cfg_default(other_columns[i]);
    }
}

static void start_column(bool others_low)
{
    stop_column();
    if (others_low) {
        for (size_t i = 0; i < ARRAY_SIZE(other_columns); i++) {
            nrf_gpio_pin_clear(other_columns[i]);
            nrf_gpio_cfg(other_columns[i], NRF_GPIO_PIN_DIR_OUTPUT,
                         NRF_GPIO_PIN_INPUT_CONNECT, NRF_GPIO_PIN_NOPULL,
                         NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
        }
    }
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
    bool others_low = false;
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
            printk("\nMONA2-RAW-GPIO-v2 READY; s=others HIZ l=others LOW x=stop\n");
            printk("No HID/BLE/trackball. D10=P1.15; H_D2=P0.28; N_D3=P0.29\n");
            printk("NFC_GPIO=%u (1 required for l; no UICR writes)\n",
                   (unsigned int)((NRF_UICR->NFCPINS & 1u) == 0));
            connected = true;
        }
        atomic_val_t cmd = atomic_set(&command, 0);
        if (cmd == 'x' || (active && now >= deadline)) {
            stop_column();
            active = false;
            printk("STOPPED: all columns high impedance\n");
        } else if ((cmd == 's' || cmd == 'l') && !active) {
            if (cmd == 'l' && (NRF_UICR->NFCPINS & 1u)) {
                printk("REFUSED: NFC_GPIO=0; send this log; no pin changes\n");
                continue;
            }
            others_low = cmd == 'l';
            start_column(others_low);
            active = true;
            deadline = now + (others_low ? LOW_TEST_MS : TEST_MS);
            printk("ACTIVE MODE=%s D10=HIGH timeout=%us; x stops\n",
                   others_low ? "OTHERS_LOW" : "OTHERS_HIZ",
                   others_low ? 15u : 60u);
            printk("PIN_CNF28=%08x PIN_CNF29=%08x (expected 00000004)\n",
                   (unsigned int)NRF_P0->PIN_CNF[28],
                   (unsigned int)NRF_P0->PIN_CNF[29]);
            k_msleep(5);
        }
        if (active) {
            /* Readback catches some output conflicts, not all overloads. */
            bool fault = !nrf_gpio_pin_read(COL_D10);
            if (others_low) {
                for (size_t i = 0; i < ARRAY_SIZE(other_columns); i++) {
                    fault |= nrf_gpio_pin_read(other_columns[i]) != 0;
                }
            }
            if (fault) {
                stop_column();
                active = false;
                printk("FAULT: output readback mismatch; all columns released; stop testing\n");
            }
        }
        if (now >= next_print) {
            if (active) {
                printk("RAW MODE=%s COL_D10=%u H_D2=%u N_D3=%u ROW0_D1=%u ROW3_D6=%u\n",
                       others_low ? "OTHERS_LOW" : "OTHERS_HIZ",
                       (unsigned int)nrf_gpio_pin_read(COL_D10),
                       (unsigned int)nrf_gpio_pin_read(H_D2),
                       (unsigned int)nrf_gpio_pin_read(N_D3),
                       (unsigned int)nrf_gpio_pin_read(3),
                       (unsigned int)nrf_gpio_pin_read(43));
                next_print = now + 250;
            } else {
                printk("MONA2-RAW-GPIO-v2 IDLE; s=others HIZ l=others LOW x=stop\n");
                next_print = now + 2000;
            }
        }
        k_msleep(20);
    }
}
