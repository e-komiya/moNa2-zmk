/* SPDX-License-Identifier: MIT
 * Standalone diagnostic, no HID/BLE/flash writes. Mode-3, half-duplex SDIO.
 * Register semantics: Zephyr input_pmw3610.c and badjeff/zmk-pmw3610-driver.
 * Output readback is diagnostic only, NOT overcurrent protection.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <string.h>

#define ID "MONA2-TRACKBALL-DIAG-v1"
#define CS 9
#define CLK 5
#define SDIO 4
#define IRQ 2
static const struct device *const serial = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static atomic_t command, cancelled;
static bool fault, active, verbose;
static unsigned half_us = 10;
static int64_t deadline, next_report;
static uint32_t samples, motions, irq_low, irq_edges;
static int32_t total_x, total_y;
static uint8_t last[7];
static unsigned previous_irq;

static void release_bus(void)
{
    nrf_gpio_cfg_input(CS, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(CLK, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(SDIO, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(IRQ, NRF_GPIO_PIN_NOPULL);
}

static bool connected(void)
{
    uint32_t dtr = 0;
    uart_line_ctrl_get(serial, UART_LINE_CTRL_DTR, &dtr);
    return dtr != 0;
}

static bool interrupted(void)
{
    if (atomic_get(&cancelled) || !connected()) {
        fault = true;
        release_bus();
        return true;
    }
    return fault;
}

static bool output(unsigned pin, unsigned value)
{
    if (fault) { return false; }
    nrf_gpio_pin_write(pin, value);
    nrf_gpio_cfg(pin, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_S0S1, NRF_GPIO_PIN_NOSENSE);
    k_busy_wait(half_us);
    if (nrf_gpio_pin_read(pin) != value) {
        release_bus();
        fault = true;
        printk("FAULT output_readback P0.%02u expected=%u; released; not proof of MCU damage\n", pin, value);
        return false;
    }
    return true;
}

static void send_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0 && !fault; bit--) {
        output(CLK, 0);
        output(SDIO, (value >> bit) & 1);
        output(CLK, 1);
    }
}

static uint8_t receive_byte(void)
{
    uint8_t value = 0;
    for (int bit = 0; bit < 8 && !fault; bit++) {
        output(CLK, 0);
        output(CLK, 1);
        value = (value << 1) | nrf_gpio_pin_read(SDIO);
    }
    return value;
}

static bool transfer(uint8_t address, uint8_t *data, unsigned count, bool write)
{
    if (interrupted()) { return false; }
    output(CLK, 1);
    output(CS, 0);
    send_byte(write ? address | 0x80 : address & 0x7f);
    if (write) {
        send_byte(data[0]);
    } else {
        nrf_gpio_cfg_input(SDIO, NRF_GPIO_PIN_NOPULL);
        k_busy_wait(200);
        for (unsigned i = 0; i < count && !fault; i++) { data[i] = receive_byte(); }
    }
    if (fault) { release_bus(); return false; }
    k_busy_wait(20);
    output(CS, 1);
    nrf_gpio_cfg_input(SDIO, NRF_GPIO_PIN_NOPULL);
    k_busy_wait(200);
    return !fault;
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t value = 0;
    transfer(reg, &value, 1, false);
    return value;
}

static void write_reg(uint8_t reg, uint8_t value)
{
    transfer(reg, &value, 1, true);
}

static void gpio_info(void)
{
    printk("GPIO NFC_GPIO=%u CS=%u SCLK=%u SDIO=%u MOTION=%u CNF_CS=%08x\n",
           (unsigned)((NRF_UICR->NFCPINS & 1u) == 0),
           (unsigned)nrf_gpio_pin_read(CS), (unsigned)nrf_gpio_pin_read(CLK),
           (unsigned)nrf_gpio_pin_read(SDIO), (unsigned)nrf_gpio_pin_read(IRQ),
           (unsigned)NRF_P0->PIN_CNF[CS]);
}

static bool prepare(void)
{
    fault = false;
    release_bus();
    if (interrupted()) { return false; }
    if (NRF_UICR->NFCPINS & 1u) {
        printk("REFUSED NFC_GPIO=0; P0.09 unavailable; no UICR writes performed\n");
        fault = true;
        return false;
    }
    /* Weak pulls only. A driven sensor input or external bias can also fail this
     * test; report the observation rather than claiming a short circuit. */
    const unsigned pins[] = {CS, CLK};
    for (unsigned i = 0; i < 2; i++) {
        nrf_gpio_cfg_input(pins[i], NRF_GPIO_PIN_PULLDOWN);
        k_msleep(5);
        unsigned low = nrf_gpio_pin_read(pins[i]);
        nrf_gpio_cfg_input(pins[i], NRF_GPIO_PIN_PULLUP);
        k_msleep(5);
        unsigned high = nrf_gpio_pin_read(pins[i]);
        printk("PULL P0.%02u down=%u up=%u (expected 0/1 if no external bias)\n", pins[i], low, high);
        if (low != 0 || high != 1) {
            fault = true;
            release_bus();
            printk("REFUSED pin held or biased; check wiring; no strong-drive test\n");
            return false;
        }
    }
    nrf_gpio_cfg_input(IRQ, NRF_GPIO_PIN_PULLUP);
    output(CS, 1);
    output(CLK, 1);
    return !fault;
}

static bool probe(void)
{
    unsigned valid = 0;
    for (int i = 0; i < 5 && !fault; i++) {
        uint8_t id = read_reg(0x00), rev = read_reg(0x01);
        printk("PROBE %d ID=%02x REV=%02x expected_ID=3e\n", i + 1, id, rev);
        if (!fault && id == 0x3e) { valid++; }
        k_msleep(10);
    }
    printk("PROBE_RESULT valid=%u/5\n", valid);
    return !fault && valid == 5;
}

static bool initialize_sensor(void)
{
    printk("INIT reset\n");
    write_reg(0x3a, 0x5a);
    k_msleep(200);
    if (interrupted() || !probe()) { return false; }
    write_reg(0x41, 0xba);
    k_busy_wait(300);
    write_reg(0x2d, 0x00);
    write_reg(0x41, 0xb5);
    k_msleep(50);
    uint8_t obs = read_reg(0x2d);
    printk("SELFTEST OBS=%02x expected_low_nibble=f\n", obs);
    if (fault || (obs & 0x0f) != 0x0f) { return false; }
    for (uint8_t reg = 2; reg <= 5; reg++) { (void)read_reg(reg); }
    write_reg(0x41, 0xba);
    k_busy_wait(300);
    write_reg(0x11, 0xfd); /* force awake, 4 ms run rate */
    write_reg(0x1b, 0x04);
    write_reg(0x1c, 0x04);
    write_reg(0x1d, 0x0f);
    write_reg(0x7f, 0xff);
    write_reg(0x05, 0x03); /* 600 CPI, raw axes */
    uint8_t resolution = read_reg(0x05);
    write_reg(0x7f, 0x00);
    write_reg(0x41, 0xb5);
    uint8_t performance = read_reg(0x11);
    printk("CONFIG CPI_REG=%02x expected=03 PERF=%02x expected=fd\n", resolution, performance);
    return !fault && resolution == 0x03 && performance == 0xfd;
}

static int16_t signed12(unsigned value)
{
    return value & 0x800 ? (int16_t)((int)value - 4096) : (int16_t)value;
}

static void report(const char *tag)
{
    printk("%s samples=%u motion_frames=%u irq_low_samples=%u irq_edges_sampled=%u sum_dx=%d sum_dy=%d\n",
           tag, samples, motions, irq_low, irq_edges, total_x, total_y);
    printk("BURST raw=%02x,%02x,%02x,%02x,%02x,%02x,%02x SQUAL=%u SHUTTER_RAW=%u\n",
           last[0], last[1], last[2], last[3], last[4], last[5], last[6],
           last[4], (last[5] << 8) | last[6]);
}

static void stop(const char *reason)
{
    release_bus();
    if (active) { report("END"); }
    active = false;
    printk("STOP %s; sensor pins input/no-pull; sensor remains powered\n", reason);
}

static void sample(void)
{
    unsigned irq = nrf_gpio_pin_read(IRQ);
    irq_low += irq == 0;
    irq_edges += irq != previous_irq;
    previous_irq = irq;
    if (!transfer(0x12, last, sizeof(last), false)) { stop("bus fault or cancel"); return; }
    samples++;
    int16_t dx = signed12(((last[3] & 0xf0) << 4) | last[1]);
    int16_t dy = signed12(((last[3] & 0x0f) << 8) | last[2]);
    if (last[0] & 0x80) {
        motions++;
        total_x += dx;
        total_y += dy;
        if (verbose) { printk("MOVE irq=%u status=%02x dx=%d dy=%d squal=%u\n", irq, last[0], dx, dy, last[4]); }
    }
}

static void menu(void)
{
    printk("\n%s READY\nUSB serial only; no keys/HID/BLE; no ADC/flash/UICR writes\n", ID);
    printk("CS=P0.09 SCLK=P0.05 SDIO=P0.04 MOTION=P0.02; mode3 half_us=%u\n", half_us);
    printk("s=start full test 60s; p=ID only; i=GPIO; r=registers while active; v=verbose; 1=slow 2=faster (idle); x=STOP; ?=menu; NO ENTER\n");
    printk("IDLE until s/p. Wrong wiring may damage hardware despite readback checks.\n");
}

static void rx(const struct device *dev, void *unused)
{
    ARG_UNUSED(unused);
    uint8_t c;
    if (!uart_irq_update(dev)) { return; }
    while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &c, 1) == 1) {
        if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; }
        if (c == 'x') { atomic_set(&cancelled, 1); }
        else if (c == 's' || c == 'p' || c == 'i' || c == 'r' || c == 'v' || c == '?' || c == '1' || c == '2') {
            atomic_cas(&command, 0, c);
        }
    }
}

int main(void)
{
    release_bus();
    if (!device_is_ready(serial) || usb_enable(NULL) != 0) { return 0; }
    uart_irq_callback_user_data_set(serial, rx, NULL);
    uart_irq_rx_enable(serial);
    bool had_connection = false;
    for (;;) {
        if (!connected()) {
            release_bus();
            active = false;
            had_connection = false;
            atomic_set(&command, 0);
            atomic_set(&cancelled, 0);
            k_msleep(20);
            continue;
        }
        if (!had_connection) { menu(); gpio_info(); had_connection = true; }
        if (atomic_get(&cancelled)) {
            stop("command");
            atomic_set(&command, 0);
            atomic_set(&cancelled, 0);
        }
        int c = atomic_set(&command, 0);
        if (c == '?') { menu(); }
        else if (c == 'i') { gpio_info(); }
        else if (c == 'v') { verbose = !verbose; printk("VERBOSE=%u\n", verbose); }
        else if (!active && (c == '1' || c == '2')) {
            half_us = c == '1' ? 10 : 2;
            printk("TIMING half_us=%u; actual clock includes GPIO overhead\n", half_us);
        } else if (!active && (c == 's' || c == 'p')) {
            printk("BEGIN %s\n", c == 's' ? "FULL" : "PROBE");
            if (!prepare()) { stop("preflight refused"); }
            else if (!probe()) { stop("ID not stable 3e; init writes skipped"); }
            else if (c == 'p') { stop("probe complete"); }
            else if (!initialize_sensor()) { stop("initialization failed or cancelled"); }
            else {
                samples = motions = irq_low = irq_edges = 0;
                total_x = total_y = 0;
                memset(last, 0, sizeof(last));
                previous_irq = nrf_gpio_pin_read(IRQ);
                active = true;
                deadline = k_uptime_get() + 60000;
                next_report = k_uptime_get() + 1000;
                printk("INIT OK; POLL 60s regardless of MOTION line; rotate ball\n");
            }
        } else if (active && c == 'r') {
            uint8_t id = read_reg(0), rev = read_reg(1), obs = read_reg(0x2d), perf = read_reg(0x11);
            printk("REG ID=%02x REV=%02x OBS=%02x PERF=%02x\n", id, rev, obs, perf);
        }
        if (active) {
            if (interrupted()) { stop("cancel/disconnect/fault"); }
            else if (k_uptime_get() >= deadline) { stop("timeout"); }
            else {
                sample();
                if (active && k_uptime_get() >= next_report) {
                    report("REPORT");
                    next_report = k_uptime_get() + 1000;
                }
            }
        }
        k_msleep(10);
    }
}
