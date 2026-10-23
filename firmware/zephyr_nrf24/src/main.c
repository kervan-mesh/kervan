/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Zephyr board + nRF24L01+ firmware for ksim live sessions (docs/hardware.md): the same logic
 * as the Pico firmware (firmware/common), on any Zephyr board. The radio node `kervan_nrf24`
 * (SPI bus, CS, CE, board name, wiring) comes from the overlay ksim writes; JSON lines go over
 * the console UART, which is the board's USB serial port (J-Link, ST-LINK, CP2102).
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include "kfw/app.h"

#define RADIO DT_NODELABEL(kervan_nrf24)
#if !DT_NODE_EXISTS(RADIO)
#error "no kervan_nrf24 node: build with the overlay ksim writes for the wiring (ksim firmware)"
#endif

#define SPI_HZ_MAX 4000000u /* the chip takes 10 MHz; 4 MHz is safe on jumper wires */
#define LED_ON_MS 20

static const struct spi_dt_spec radio_spi =
    SPI_DT_SPEC_GET(RADIO, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER);
static const struct gpio_dt_spec radio_ce = GPIO_DT_SPEC_GET(RADIO, ce_gpios);
static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

#if DT_NODE_EXISTS(DT_ALIAS(led0))
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_LED 1
#else
#define HAVE_LED 0
#endif

RING_BUF_DECLARE(rx_ring, 1024);
#if HAVE_LED
static int64_t led_off_at;
#endif

static void uart_rx(const struct device *dev, void *user_data)
{
    uint8_t buf[32];
    (void)user_data;
    if (!uart_irq_update(dev)) {
        return;
    }
    while (uart_irq_rx_ready(dev)) {
        const int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        ring_buf_put(&rx_ring, buf,
                     (uint32_t)n); /* a full buffer drops: the line is then refused */
    }
}

static void spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t n)
{
    struct spi_buf tb = {.buf = (void *)tx, .len = n};
    struct spi_buf rb = {.buf = rx, .len = n};
    const struct spi_buf_set txs = {.buffers = &tb, .count = 1};
    const struct spi_buf_set rxs = {.buffers = &rb, .count = 1};
    (void)ctx;
    (void)spi_transceive_dt(&radio_spi, &txs, rx != NULL ? &rxs : NULL);
}

static void ce(void *ctx, bool high)
{
    (void)ctx;
    (void)gpio_pin_set_dt(&radio_ce, high ? 1 : 0);
}

static void delay(void *ctx, uint32_t us)
{
    (void)ctx;
    k_busy_wait(us);
}

static void write_line(void *ctx, const char *line)
{
    (void)ctx;
    for (const char *p = line; *p != '\0'; p++) {
        uart_poll_out(console, (unsigned char)*p);
    }
    uart_poll_out(console, '\n');
}

static void activity(void *ctx)
{
    (void)ctx;
#if HAVE_LED
    (void)gpio_pin_set_dt(&led, 1);
    led_off_at = k_uptime_get() + LED_ON_MS;
#endif
}

static uint64_t now_us(void)
{
    return k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

int main(void)
{
    static kfw_app_t app;
    static char line[KFW_LINE_MAX];
    static char id[2 * 16 + 1];
    uint8_t raw[16];
    size_t len = 0;
    bool too_long = false;
    kfw_app_host_t host;
    kfw_nrf24_io_t io;
    const bool ready = spi_is_ready_dt(&radio_spi) && gpio_is_ready_dt(&radio_ce) &&
                       gpio_pin_configure_dt(&radio_ce, GPIO_OUTPUT_INACTIVE) == 0;

    uart_irq_callback_user_data_set(console, uart_rx, NULL);
    uart_irq_rx_enable(console);
#if HAVE_LED
    (void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
#endif

    /* a unique id: the chip's, in hex */
    const ssize_t n = hwinfo_get_device_id(raw, sizeof(raw));
    for (ssize_t i = 0; i < n && i < (ssize_t)sizeof(raw); i++) {
        static const char digits[] = "0123456789ABCDEF";
        id[2 * i] = digits[raw[i] >> 4];
        id[2 * i + 1] = digits[raw[i] & 0x0Fu];
    }

    k_msleep(110); /* the radio's power-on reset takes 100 ms */

    memset(&host, 0, sizeof(host));
    host.write_line = write_line;
    host.activity = activity;
    host.board = DT_PROP(RADIO, kervan_board);
    host.board_id = id;
    host.wiring = DT_PROP(RADIO, kervan_wiring);
    io.spi = spi_xfer;
    io.ce = ce;
    io.delay_us = delay;
    io.ctx = NULL;
    kfw_app_init(&app, &host, ready ? &io : NULL);

    while (true) {
        uint8_t c;
        while (ring_buf_get(&rx_ring, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                if (too_long) {
                    write_line(NULL, "{\"ev\":\"error\",\"msg\":\"line too long\"}");
                } else if (len > 0u) {
                    kfw_app_line(&app, line, len, now_us());
                }
                len = 0;
                too_long = false;
            } else if (len < sizeof(line)) {
                line[len++] = (char)c;
            } else {
                too_long = true;
            }
        }
        kfw_app_poll(&app, now_us());
#if HAVE_LED
        if (led_off_at != 0 && k_uptime_get() >= led_off_at) {
            (void)gpio_pin_set_dt(&led, 0);
            led_off_at = 0;
        }
#endif
        k_yield();
    }
    return 0;
}
