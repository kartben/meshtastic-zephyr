/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic_packet.h"
#include "meshtastic_phoneapi.h"

LOG_MODULE_DECLARE(meshtastic, CONFIG_MESHTASTIC_LOG_LEVEL);

#define MESHTASTIC_PHONEAPI_MAX_TRANSPORTS 2

static struct {
	struct meshtastic_phoneapi *transports[MESHTASTIC_PHONEAPI_MAX_TRANSPORTS];
	uint8_t count;
} phoneapi;

static K_MUTEX_DEFINE(phoneapi_lock);

int meshtastic_phoneapi_encode_fromradio_frame(const meshtastic_FromRadio *from,
					       struct meshtastic_phoneapi_frame *frame)
{
	pb_ostream_t stream;

	*frame = (struct meshtastic_phoneapi_frame){0};

	stream = pb_ostream_from_buffer(frame->data, sizeof(frame->data));
	if (!pb_encode(&stream, meshtastic_FromRadio_fields, from)) {
		LOG_ERR("FromRadio encode failed: %s", PB_GET_ERROR(&stream));
		return -ENOMEM;
	}

	frame->len = (uint16_t)stream.bytes_written;
	return 0;
}

static bool config_active(const struct meshtastic_phoneapi *api)
{
	return api->config_state != MESHTASTIC_PHONEAPI_CONFIG_IDLE;
}

void meshtastic_phoneapi_notify_data_ready(struct meshtastic_phoneapi *api)
{
	if (api->data_ready != NULL) {
		api->data_ready(api);
	}
}

void meshtastic_phoneapi_init(struct meshtastic_phoneapi *api, const char *name,
			      struct meshtastic_phoneapi_frame *queue, uint8_t queue_size,
			      meshtastic_phoneapi_data_ready_cb_t data_ready,
			      meshtastic_phoneapi_disconnect_cb_t disconnect,
			      meshtastic_phoneapi_invalidate_cb_t invalidate_delivery, void *user_data)
{
	*api = (struct meshtastic_phoneapi){
		.name = name,
		.queue = queue,
		.queue_size = queue_size,
		.data_ready = data_ready,
		.disconnect = disconnect,
		.invalidate_delivery = invalidate_delivery,
		.user_data = user_data,
	};
	k_mutex_init(&api->lock);
}

void meshtastic_phoneapi_register(struct meshtastic_phoneapi *api)
{
	k_mutex_lock(&phoneapi_lock, K_FOREVER);
	for (uint8_t i = 0; i < phoneapi.count; i++) {
		if (phoneapi.transports[i] == api) {
			k_mutex_unlock(&phoneapi_lock);
			return;
		}
	}

	if (phoneapi.count < MESHTASTIC_PHONEAPI_MAX_TRANSPORTS) {
		phoneapi.transports[phoneapi.count++] = api;
		LOG_DBG("PhoneAPI registered transport %s (queue depth %u)", api->name,
			api->queue_size);
	} else {
		LOG_WRN("PhoneAPI transport limit reached, ignoring %s", api->name);
	}
	k_mutex_unlock(&phoneapi_lock);
}

void meshtastic_phoneapi_reset(struct meshtastic_phoneapi *api)
{
	k_mutex_lock(&api->lock, K_FOREVER);
	api->head = 0U;
	api->tail = 0U;
	api->count = 0U;
	api->current_valid = false;
	api->config_state = MESHTASTIC_PHONEAPI_CONFIG_IDLE;
	api->config_index = 0U;
	api->config_request_id = 0U;
	api->from_num = 0U;
	k_mutex_unlock(&api->lock);
}

uint32_t meshtastic_phoneapi_from_num(struct meshtastic_phoneapi *api)
{
	uint32_t num;

	k_mutex_lock(&api->lock, K_FOREVER);
	num = api->from_num;
	k_mutex_unlock(&api->lock);

	return num;
}

uint32_t meshtastic_phoneapi_pending_count(struct meshtastic_phoneapi *api)
{
	uint32_t count;

	k_mutex_lock(&api->lock, K_FOREVER);
	count = api->count + (api->current_valid ? 1U : 0U) + (config_active(api) ? 1U : 0U);
	k_mutex_unlock(&api->lock);

	return count;
}

bool meshtastic_phoneapi_pop_frame(struct meshtastic_phoneapi *api,
				   struct meshtastic_phoneapi_frame *frame)
{
	k_mutex_lock(&api->lock, K_FOREVER);
	if (api->count == 0U) {
		if (config_active(api) && meshtastic_phoneapi_next_config_frame(api, frame) == 0) {
			k_mutex_unlock(&api->lock);
			return true;
		}

		k_mutex_unlock(&api->lock);
		return false;
	}

	*frame = api->queue[api->tail];
	api->tail = (uint8_t)((api->tail + 1U) % api->queue_size);
	api->count--;
	k_mutex_unlock(&api->lock);

	return true;
}

void meshtastic_phoneapi_push_frame_front(struct meshtastic_phoneapi *api,
					  const struct meshtastic_phoneapi_frame *frame)
{
	k_mutex_lock(&api->lock, K_FOREVER);

	if (api->count == api->queue_size) {
		api->head = (uint8_t)((api->head + api->queue_size - 1U) % api->queue_size);
		api->count--;
	}

	api->tail = (uint8_t)((api->tail + api->queue_size - 1U) % api->queue_size);
	api->queue[api->tail] = *frame;
	api->count++;

	k_mutex_unlock(&api->lock);
}

bool meshtastic_phoneapi_current_frame(struct meshtastic_phoneapi *api,
				       struct meshtastic_phoneapi_frame *frame)
{
	k_mutex_lock(&api->lock, K_FOREVER);
	if (!api->current_valid && api->count > 0U) {
		api->current = api->queue[api->tail];
		api->tail = (uint8_t)((api->tail + 1U) % api->queue_size);
		api->count--;
		api->current_valid = true;
	} else if (!api->current_valid && config_active(api) &&
		   meshtastic_phoneapi_next_config_frame(api, &api->current) == 0) {
		api->current_valid = true;
	}

	if (!api->current_valid) {
		k_mutex_unlock(&api->lock);
		return false;
	}

	*frame = api->current;
	k_mutex_unlock(&api->lock);
	return true;
}

void meshtastic_phoneapi_release_current_frame(struct meshtastic_phoneapi *api)
{
	k_mutex_lock(&api->lock, K_FOREVER);
	api->current_valid = false;
	k_mutex_unlock(&api->lock);
}

void meshtastic_phoneapi_current_frame_complete(struct meshtastic_phoneapi *api)
{
	meshtastic_phoneapi_release_current_frame(api);
}

void meshtastic_phoneapi_current_frame_reset(struct meshtastic_phoneapi *api)
{
	k_mutex_lock(&api->lock, K_FOREVER);
	api->current_valid = false;
	k_mutex_unlock(&api->lock);
}

int meshtastic_phoneapi_enqueue_fromradio(struct meshtastic_phoneapi *api,
					  const meshtastic_FromRadio *from)
{
	struct meshtastic_phoneapi_frame frame;
	int ret;

	ret = meshtastic_phoneapi_encode_fromradio_frame(from, &frame);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&api->lock, K_FOREVER);
	if (api->count == api->queue_size) {
		api->tail = (uint8_t)((api->tail + 1U) % api->queue_size);
		api->count--;
		LOG_WRN("%s FromRadio queue full, dropping oldest frame", api->name);
	}

	api->queue[api->head] = frame;
	api->head = (uint8_t)((api->head + 1U) % api->queue_size);
	api->count++;
	LOG_DBG("%s FromRadio enqueue variant=%u len=%u pending=%u/%u", api->name,
		(unsigned int)from->which_payload_variant, frame.len, api->count, api->queue_size);
	k_mutex_unlock(&api->lock);

	meshtastic_phoneapi_notify_data_ready(api);

	return 0;
}

void meshtastic_phoneapi_enqueue_queue_status(struct meshtastic_phoneapi *api, int res,
					      uint32_t mesh_packet_id)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	uint32_t pending = meshtastic_phoneapi_pending_count(api);

	from.id = meshtastic_next_fromradio_id();
	from.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
	from.queueStatus.res = (int8_t)CLAMP(res, INT8_MIN, INT8_MAX);
	from.queueStatus.free = (pending >= api->queue_size) ? 0U : (api->queue_size - pending);
	from.queueStatus.maxlen = api->queue_size;
	from.queueStatus.mesh_packet_id = mesh_packet_id;

	(void)meshtastic_phoneapi_enqueue_fromradio(api, &from);
}

void meshtastic_phoneapi_enqueue_my_info(struct meshtastic_phoneapi *api, uint32_t request_id)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	uint8_t node_id[4];

	ARG_UNUSED(request_id);

	from.id = meshtastic_next_fromradio_id();
	from.which_payload_variant = meshtastic_FromRadio_my_info_tag;
	from.my_info.my_node_num = meshtastic_get_node_id();
	from.my_info.min_app_version = 0U;
	from.my_info.nodedb_count = 1U;
	from.my_info.device_id.size = sizeof(node_id);
	sys_put_le32(meshtastic_get_node_id(), node_id);
	memcpy(from.my_info.device_id.bytes, node_id, sizeof(node_id));
	strncpy(from.my_info.pio_env, "zephyr", sizeof(from.my_info.pio_env) - 1U);

	(void)meshtastic_phoneapi_enqueue_fromradio(api, &from);
}

void meshtastic_phoneapi_enqueue_rebooted(struct meshtastic_phoneapi *api)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;

	from.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
	from.rebooted = true;

	(void)meshtastic_phoneapi_enqueue_fromradio(api, &from);
}

void meshtastic_phoneapi_handle_toradio(struct meshtastic_phoneapi *api, const uint8_t *buf,
					size_t len)
{
	meshtastic_ToRadio to = meshtastic_ToRadio_init_zero;
	pb_istream_t stream;
	int ret = 0;

	stream = pb_istream_from_buffer(buf, len);
	if (!pb_decode(&stream, meshtastic_ToRadio_fields, &to)) {
		/*
		 * StreamAPI resync can deliver one extra byte when a false length
		 * includes the next frame's START1 (0x94) as payload.
		 */
		if (len > 1U && buf[len - 1U] == 0x94U) {
			stream = pb_istream_from_buffer(buf, len - 1U);
			if (pb_decode(&stream, meshtastic_ToRadio_fields, &to)) {
				LOG_DBG("%s ToRadio recovered after dropping trailing "
					"START1 (%u -> %u bytes)",
					api->name, (unsigned int)len, (unsigned int)(len - 1U));
				len -= 1U;
				goto toradio_decoded;
			}
		}

		LOG_WRN("%s ToRadio decode failed (len=%u): %s", api->name, (unsigned int)len,
			PB_GET_ERROR(&stream));
		LOG_HEXDUMP_DBG(buf, MIN(len, 16U), api->name);
		return;
	}

toradio_decoded:

	if (stream.bytes_left > 0U) {
		LOG_DBG("%s ToRadio trailing bytes (%u)", api->name,
			(unsigned int)stream.bytes_left);
	}

	LOG_DBG("%s ToRadio variant=%u len=%u", api->name, (unsigned int)to.which_payload_variant,
		(unsigned int)len);

	switch (to.which_payload_variant) {
	case meshtastic_ToRadio_packet_tag:
		LOG_DBG("%s ToRadio packet id=%u", api->name, to.packet.id);
		ret = meshtastic_send_mesh_pb(&to.packet);
		meshtastic_phoneapi_enqueue_queue_status(api, ret, to.packet.id);
		break;
	case meshtastic_ToRadio_want_config_id_tag:
		LOG_DBG("%s ToRadio want_config_id=%u", api->name, to.want_config_id);
		meshtastic_phoneapi_enqueue_phone_config(api, to.want_config_id);
		break;
	case meshtastic_ToRadio_disconnect_tag:
		LOG_DBG("%s ToRadio disconnect", api->name);
		if (api->disconnect != NULL) {
			api->disconnect(api);
		}
		break;
	case meshtastic_ToRadio_heartbeat_tag:
		LOG_DBG("%s ToRadio heartbeat", api->name);
		meshtastic_phoneapi_enqueue_queue_status(api, 0, 0U);
		break;
	default:
		LOG_WRN("%s unsupported ToRadio variant %u", api->name,
			(unsigned int)to.which_payload_variant);
		break;
	}
}

void meshtastic_phoneapi_on_packet(const struct meshtastic_packet *packet)
{
	meshtastic_FromRadio from = meshtastic_FromRadio_init_zero;
	struct meshtastic_phoneapi *transports[MESHTASTIC_PHONEAPI_MAX_TRANSPORTS];
	uint8_t count;
	int ret;

	from.id = meshtastic_next_fromradio_id();
	from.which_payload_variant = meshtastic_FromRadio_packet_tag;

	ret = meshtastic_packet_to_mesh_pb(packet, &from.packet);
	if (ret < 0) {
		LOG_DBG("FromRadio packet encode skipped (%d)", ret);
		return;
	}

	k_mutex_lock(&phoneapi_lock, K_FOREVER);
	count = phoneapi.count;
	memcpy(transports, phoneapi.transports, count * sizeof(transports[0]));
	k_mutex_unlock(&phoneapi_lock);

	LOG_DBG("FromRadio packet fan-out to %u transport(s), mesh id=%u", count, from.packet.id);

	for (uint8_t i = 0; i < count; i++) {
		(void)meshtastic_phoneapi_enqueue_fromradio(transports[i], &from);
	}
}
