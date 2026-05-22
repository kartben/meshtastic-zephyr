/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include "meshtastic_serial.h"

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zephyr_meshtastic_uart)
#error "CONFIG_MESHTASTIC_SERIAL requires /chosen/zephyr,meshtastic-uart"
#endif

#define MESHTASTIC_SERIAL_UART_NODE DT_CHOSEN(zephyr_meshtastic_uart)
#define MESHTASTIC_SERIAL_START1    0x94U
#define MESHTASTIC_SERIAL_START2    0xc3U
#define MESHTASTIC_SERIAL_HEADER    4U
#define MESHTASTIC_SERIAL_WIRE_MAX  (MESHTASTIC_API_FRAME_MAX + MESHTASTIC_SERIAL_HEADER)

#if !DT_NODE_HAS_PROP(MESHTASTIC_SERIAL_UART_NODE, current_speed)
#define MESHTASTIC_SERIAL_CONFIGURE_DEFAULT_BAUD 1
#elif DT_PROP(MESHTASTIC_SERIAL_UART_NODE, current_speed) == 0
#define MESHTASTIC_SERIAL_CONFIGURE_DEFAULT_BAUD 1
#else
#define MESHTASTIC_SERIAL_CONFIGURE_DEFAULT_BAUD 0
#endif

enum meshtastic_serial_parse_state {
	MESHTASTIC_SERIAL_WAIT_START1,
	MESHTASTIC_SERIAL_WAIT_START2,
	MESHTASTIC_SERIAL_LEN_HI,
	MESHTASTIC_SERIAL_LEN_LO,
	MESHTASTIC_SERIAL_PAYLOAD,
};

RING_BUF_DECLARE(serial_rx_rb, CONFIG_MESHTASTIC_SERIAL_RX_BUF_SIZE);
RING_BUF_DECLARE(serial_tx_rb, CONFIG_MESHTASTIC_SERIAL_TX_BUF_SIZE);

static struct {
	const struct device *dev;
	struct meshtastic_phoneapi api;
	struct meshtastic_phoneapi_frame queue[CONFIG_MESHTASTIC_SERIAL_FROMRADIO_QUEUE_SIZE];
	struct k_work_q work_q;
	struct k_work rx_work;
	struct k_work tx_work;
	struct k_work tx_cont_work;
	enum meshtastic_serial_parse_state state;
	uint16_t rx_len;
	uint16_t rx_pos;
	uint8_t rx_payload[MESHTASTIC_API_FRAME_MAX];
	atomic_t rx_overflow;
} serial = {
	.dev = DEVICE_DT_GET(MESHTASTIC_SERIAL_UART_NODE),
};

K_THREAD_STACK_DEFINE(serial_work_stack, CONFIG_MESHTASTIC_SERIAL_WORK_STACK_SIZE);

size_t meshtastic_serial_encode_frame(const uint8_t *payload, size_t payload_len, uint8_t *out,
				      size_t out_len)
{
	if (payload_len > MESHTASTIC_API_FRAME_MAX ||
	    out_len < payload_len + MESHTASTIC_SERIAL_HEADER) {
		return 0U;
	}

	out[0] = MESHTASTIC_SERIAL_START1;
	out[1] = MESHTASTIC_SERIAL_START2;
	out[2] = (uint8_t)(payload_len >> 8);
	out[3] = (uint8_t)payload_len;
	memcpy(&out[MESHTASTIC_SERIAL_HEADER], payload, payload_len);

	return payload_len + MESHTASTIC_SERIAL_HEADER;
}

int meshtastic_serial_decode_byte(uint8_t byte, uint8_t *payload, size_t payload_len,
				  size_t *frame_len)
{
	*frame_len = 0U;

	switch (serial.state) {
	case MESHTASTIC_SERIAL_WAIT_START1:
		if (byte == MESHTASTIC_SERIAL_START1) {
			serial.state = MESHTASTIC_SERIAL_WAIT_START2;
		}
		break;
	case MESHTASTIC_SERIAL_WAIT_START2:
		if (byte == MESHTASTIC_SERIAL_START2) {
			serial.state = MESHTASTIC_SERIAL_LEN_HI;
		} else if (byte == MESHTASTIC_SERIAL_START1) {
			/* Stay in WAIT_START2 for the matching START2. */
		} else {
			serial.state = MESHTASTIC_SERIAL_WAIT_START1;
		}
		break;
	case MESHTASTIC_SERIAL_LEN_HI:
		serial.rx_len = ((uint16_t)byte) << 8;
		serial.state = MESHTASTIC_SERIAL_LEN_LO;
		break;
	case MESHTASTIC_SERIAL_LEN_LO:
		serial.rx_len |= byte;
		serial.rx_pos = 0U;
		if (serial.rx_len > MESHTASTIC_API_FRAME_MAX || serial.rx_len > payload_len) {
			serial.state = MESHTASTIC_SERIAL_WAIT_START1;
			return -EMSGSIZE;
		}
		if (serial.rx_len == 0U) {
			serial.state = MESHTASTIC_SERIAL_WAIT_START1;
			return 1;
		}
		serial.state = MESHTASTIC_SERIAL_PAYLOAD;
		break;
	case MESHTASTIC_SERIAL_PAYLOAD:
		payload[serial.rx_pos++] = byte;
		if (serial.rx_pos == serial.rx_len) {
			*frame_len = serial.rx_len;
			serial.state = MESHTASTIC_SERIAL_WAIT_START1;
			return 1;
		}
		break;
	default:
		serial.state = MESHTASTIC_SERIAL_WAIT_START1;
		break;
	}

	return 0;
}

static bool pop_fromradio(struct meshtastic_phoneapi_frame *frame)
{
	return meshtastic_phoneapi_pop_frame(&serial.api, frame);
}

static void push_fromradio(const struct meshtastic_phoneapi_frame *frame)
{
	meshtastic_phoneapi_push_frame_front(&serial.api, frame);
}

static void serial_tx_start(void)
{
	uart_irq_tx_enable(serial.dev);
}

static void serial_tx_finish(void)
{
	unsigned int key = irq_lock();

	if (!ring_buf_is_empty(&serial_tx_rb)) {
		irq_unlock(key);
		return;
	}

	uart_irq_tx_disable(serial.dev);
	irq_unlock(key);
	k_work_submit_to_queue(&serial.work_q, &serial.tx_cont_work);
}

static bool serial_tx_queue(const uint8_t *data, size_t len)
{
	unsigned int key = irq_lock();
	bool ok = false;

	if (ring_buf_space_get(&serial_tx_rb) >= len) {
		(void)ring_buf_put(&serial_tx_rb, data, len);
		ok = true;
	}

	serial_tx_start();
	irq_unlock(key);

	return ok;
}

static void tx_work_handler(struct k_work *work)
{
	struct meshtastic_phoneapi_frame frame;
	uint8_t wire[MESHTASTIC_SERIAL_WIRE_MAX];
	uint32_t bytes = 0U;
	uint32_t frames = 0U;

	ARG_UNUSED(work);

	while (pop_fromradio(&frame)) {
		size_t wire_len =
			meshtastic_serial_encode_frame(frame.data, frame.len, wire, sizeof(wire));

		if (wire_len == 0U) {
			continue;
		}

		if (ring_buf_space_get(&serial_tx_rb) < wire_len) {
			LOG_DBG("Serial PhoneAPI TX ring full (%u free, need %u), deferring",
				ring_buf_space_get(&serial_tx_rb), (unsigned int)wire_len);
			push_fromradio(&frame);
			break;
		}

		if (!serial_tx_queue(wire, wire_len)) {
			push_fromradio(&frame);
			break;
		}

		bytes += wire_len;
		frames++;
	}

	if (frames > 0U) {
		LOG_INF("Serial PhoneAPI TX %u frame(s), %u byte(s)", frames, bytes);
	}
}

static void tx_cont_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_work_submit_to_queue(&serial.work_q, &serial.tx_work);
}

/*
 * Extract one complete StreamAPI frame from the RX ring buffer.
 *
 * Returns 1 when a payload was delivered, 0 when more UART data is needed,
 * and -1 after discarding byte(s) while resynchronizing.
 */
static int serial_rx_pop_frame(size_t *payload_len)
{
	uint8_t hdr[MESHTASTIC_SERIAL_HEADER];
	uint8_t wire[MESHTASTIC_SERIAL_WIRE_MAX];
	uint32_t avail;
	uint16_t pkt_len;
	uint32_t frame_len;

	avail = ring_buf_size_get(&serial_rx_rb);
	if (avail < MESHTASTIC_SERIAL_HEADER) {
		return 0;
	}

	if (ring_buf_peek(&serial_rx_rb, hdr, MESHTASTIC_SERIAL_HEADER) !=
	    MESHTASTIC_SERIAL_HEADER) {
		return 0;
	}

	if (hdr[0] != MESHTASTIC_SERIAL_START1) {
		uint8_t discard;

		(void)ring_buf_get(&serial_rx_rb, &discard, 1U);
		return -1;
	}

	if (hdr[1] != MESHTASTIC_SERIAL_START2) {
		uint8_t discard;

		(void)ring_buf_get(&serial_rx_rb, &discard, 1U);
		return -1;
	}

	pkt_len = ((uint16_t)hdr[2] << 8) | hdr[3];
	if (pkt_len > MESHTASTIC_API_FRAME_MAX) {
		uint8_t discard;

		LOG_DBG("Serial PhoneAPI oversize length %u, resyncing", pkt_len);
		(void)ring_buf_get(&serial_rx_rb, &discard, 1U);
		return -1;
	}

	frame_len = MESHTASTIC_SERIAL_HEADER + pkt_len;
	if (avail < frame_len) {
		return 0;
	}

	if (pkt_len > 0U) {
		if (ring_buf_peek(&serial_rx_rb, wire, frame_len) != frame_len) {
			return 0;
		}

		memcpy(serial.rx_payload, &wire[MESHTASTIC_SERIAL_HEADER], pkt_len);
	}

	(void)ring_buf_get(&serial_rx_rb, wire, frame_len);
	*payload_len = pkt_len;

	return 1;
}

static void rx_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_cas(&serial.rx_overflow, 1, 0)) {
		LOG_WRN("Serial PhoneAPI RX buffer overrun, some bytes were dropped");
	}

	while (true) {
		size_t frame_len = 0U;
		int ret = serial_rx_pop_frame(&frame_len);

		if (ret == 0) {
			break;
		}

		if (ret < 0) {
			continue;
		}

		if (frame_len == 0U) {
			LOG_DBG("Serial PhoneAPI RX empty StreamAPI frame ignored");
			continue;
		}

		LOG_DBG("Serial PhoneAPI RX StreamAPI frame len=%u", frame_len);
		meshtastic_phoneapi_handle_toradio(&serial.api, serial.rx_payload, frame_len);
	}
}

static void serial_data_ready(struct meshtastic_phoneapi *api)
{
	ARG_UNUSED(api);

	k_work_submit_to_queue(&serial.work_q, &serial.tx_work);
}

static void serial_disconnect(struct meshtastic_phoneapi *api)
{
	ARG_UNUSED(api);
}

static void serial_isr_rx(void)
{
	uint8_t *data;
	uint32_t space;
	int len;

	space = ring_buf_put_claim(&serial_rx_rb, &data, UINT32_MAX);
	if (space == 0U) {
		uint8_t discard[16];

		atomic_set(&serial.rx_overflow, 1);
		do {
			len = uart_fifo_read(serial.dev, discard, sizeof(discard));
		} while (len == (int)sizeof(discard));
		return;
	}

	len = uart_fifo_read(serial.dev, data, space);
	if (len <= 0) {
		ring_buf_put_finish(&serial_rx_rb, 0U);
		return;
	}

	ring_buf_put_finish(&serial_rx_rb, (uint32_t)len);
	k_work_submit_to_queue(&serial.work_q, &serial.rx_work);
}

static void serial_isr_tx(void)
{
	uint8_t *data;
	uint32_t len;
	int sent;

	len = ring_buf_get_claim(&serial_tx_rb, &data, UINT32_MAX);
	if (len == 0U) {
		serial_tx_finish();
		return;
	}

	sent = uart_fifo_fill(serial.dev, data, len);
	if (sent <= 0) {
		ring_buf_get_finish(&serial_tx_rb, 0U);
		return;
	}

	ring_buf_get_finish(&serial_tx_rb, (uint32_t)sent);
}

static void serial_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);

	if (uart_irq_rx_ready(dev)) {
		serial_isr_rx();
	}

	if (uart_irq_tx_ready(dev)) {
		/*
		 * Hardware UARTs often have FIFO depth 1: keep draining while
		 * the driver reports TX ready and the ring buffer is shrinking.
		 */
		do {
			uint32_t pending = ring_buf_size_get(&serial_tx_rb);

			serial_isr_tx();
			if (ring_buf_size_get(&serial_tx_rb) == pending) {
				break;
			}
		} while (uart_irq_tx_ready(dev));
	}
}

static int configure_uart(void)
{
#if MESHTASTIC_SERIAL_CONFIGURE_DEFAULT_BAUD
	const struct uart_config cfg = {
		.baudrate = 115200,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int ret = uart_configure(serial.dev, &cfg);

	if (ret < 0 && ret != -ENOSYS && ret != -ENOTSUP) {
		return ret;
	}
#endif

	return 0;
}

int meshtastic_serial_init(void)
{
	int ret;

	if (!device_is_ready(serial.dev)) {
		LOG_ERR("Meshtastic serial UART is not ready");
		return -ENODEV;
	}

	ret = configure_uart();
	if (ret < 0) {
		LOG_ERR("Meshtastic serial UART configuration failed (%d)", ret);
		return ret;
	}

	meshtastic_phoneapi_init(&serial.api, "serial", serial.queue, ARRAY_SIZE(serial.queue),
				 serial_data_ready, serial_disconnect, NULL, NULL);
	meshtastic_phoneapi_register(&serial.api);

	k_work_queue_start(&serial.work_q, serial_work_stack,
			   K_THREAD_STACK_SIZEOF(serial_work_stack),
			   CONFIG_MESHTASTIC_SERIAL_WORK_PRIORITY, NULL);
	k_work_init(&serial.rx_work, rx_work_handler);
	k_work_init(&serial.tx_work, tx_work_handler);
	k_work_init(&serial.tx_cont_work, tx_cont_work_handler);

	ret = uart_irq_callback_user_data_set(serial.dev, serial_isr, NULL);
	if (ret < 0) {
		LOG_ERR("Meshtastic serial UART IRQ callback failed (%d)", ret);
		return ret;
	}

	uart_irq_tx_disable(serial.dev);
	uart_irq_rx_enable(serial.dev);
	meshtastic_phoneapi_enqueue_rebooted(&serial.api);

	LOG_INF("Meshtastic serial PhoneAPI ready on %s", serial.dev->name);
	return 0;
}
