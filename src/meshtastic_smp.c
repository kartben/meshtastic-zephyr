/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include "meshtastic_modules.h"

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

struct meshtastic_smp_reply_context {
	uint8_t *buf;
	size_t len;
	size_t capacity;
	uint8_t response_count;
};

static struct smp_transport meshtastic_smp_transport = {
	.functions.output = NULL,
	.functions.get_mtu = NULL,
};

static struct meshtastic_smp_reply_context *active_reply_ctx;
static K_MUTEX_DEFINE(meshtastic_smp_lock);

static int smp_err_to_errno(int err)
{
	switch (err) {
	case MGMT_ERR_EOK:
		return 0;
	case MGMT_ERR_ENOMEM:
		return -ENOMEM;
	case MGMT_ERR_EINVAL:
		return -EINVAL;
	case MGMT_ERR_ENOENT:
		return -ENOENT;
	case MGMT_ERR_EMSGSIZE:
		return -EMSGSIZE;
	case MGMT_ERR_ENOTSUP:
		return -ENOTSUP;
	case MGMT_ERR_EBUSY:
		return -EBUSY;
	default:
		return -EIO;
	}
}

static uint16_t meshtastic_smp_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);

	return MESHTASTIC_MAX_PAYLOAD_LEN;
}

static int meshtastic_smp_output(struct net_buf *nb)
{
	struct meshtastic_smp_reply_context *ctx = active_reply_ctx;
	int ret = MGMT_ERR_EOK;

	if (ctx == NULL) {
		ret = MGMT_ERR_EUNKNOWN;
		goto out;
	}

	if (ctx->response_count != 0U) {
		LOG_WRN("SMP request produced more than one response packet");
		ret = MGMT_ERR_EMSGSIZE;
		goto out;
	}

	if (nb->len > ctx->capacity) {
		LOG_WRN("SMP response too large for Meshtastic payload (%u > %u)",
			(unsigned int)nb->len, (unsigned int)ctx->capacity);
		ret = MGMT_ERR_EMSGSIZE;
		goto out;
	}

	memcpy(ctx->buf, nb->data, nb->len);
	ctx->len = nb->len;
	ctx->response_count = 1U;

out:
	smp_packet_free(nb);
	return ret;
}

static int meshtastic_smp_process_request(const uint8_t *payload, size_t payload_len, uint8_t *reply_buf,
					  size_t reply_buf_len, size_t *reply_len)
{
	struct meshtastic_smp_reply_context ctx = {
		.buf = reply_buf,
		.capacity = reply_buf_len,
	};
	struct cbor_nb_reader reader;
	struct cbor_nb_writer writer;
	struct smp_streamer streamer = {
		.reader = &reader,
		.writer = &writer,
		.smpt = &meshtastic_smp_transport,
	};
	struct net_buf *nb;
	int ret;

	if (payload == NULL || reply_buf == NULL || reply_len == NULL || payload_len < MGMT_HDR_SIZE) {
		return -EINVAL;
	}

	nb = smp_packet_alloc();
	if (nb == NULL) {
		return -ENOMEM;
	}

	if (payload_len > net_buf_tailroom(nb)) {
		smp_packet_free(nb);
		return -EMSGSIZE;
	}

	net_buf_add_mem(nb, payload, payload_len);

	k_mutex_lock(&meshtastic_smp_lock, K_FOREVER);
	active_reply_ctx = &ctx;
	ret = smp_process_request_packet(&streamer, nb);
	active_reply_ctx = NULL;
	k_mutex_unlock(&meshtastic_smp_lock);

	if (ret != MGMT_ERR_EOK) {
		return smp_err_to_errno(ret);
	}

	if (ctx.response_count == 0U) {
		return -ENOMSG;
	}

	*reply_len = ctx.len;
	return 0;
}

static int meshtastic_module_smp_alloc_reply(const struct meshtastic_packet *req,
					     struct meshtastic_packet *reply)
{
	static uint8_t payload[MESHTASTIC_MAX_PAYLOAD_LEN];
	size_t payload_len;
	int ret;

	if (req == NULL || reply == NULL || req->payload == NULL || req->payload_len == 0U) {
		return -EINVAL;
	}

	ret = meshtastic_smp_process_request(req->payload, req->payload_len, payload, sizeof(payload),
					     &payload_len);
	if (ret < 0) {
		LOG_WRN("SMP request from 0x%08x failed (%d)", req->from, ret);
		return ret;
	}

	*reply = (struct meshtastic_packet){
		.portnum = CONFIG_MESHTASTIC_SMP_PORTNUM,
		.payload = payload,
		.payload_len = payload_len,
	};

	return 0;
}

MESHTASTIC_MODULE_DEFINE(smp, CONFIG_MESHTASTIC_SMP_PORTNUM, 0, NULL,
			 meshtastic_module_smp_alloc_reply);

int meshtastic_smp_init(void)
{
	meshtastic_smp_transport.functions.output = meshtastic_smp_output;
	meshtastic_smp_transport.functions.get_mtu = meshtastic_smp_get_mtu;

	return smp_transport_init(&meshtastic_smp_transport);
}
