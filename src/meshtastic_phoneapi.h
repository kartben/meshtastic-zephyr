/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_PHONEAPI_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_PHONEAPI_H_

#include "meshtastic/channel.pb.h"
#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum protobuf message bytes used by the Meshtastic phone API. */
#define MESHTASTIC_API_FRAME_MAX 512U

struct meshtastic_phoneapi;

struct meshtastic_phoneapi_frame {
	uint8_t data[MESHTASTIC_API_FRAME_MAX];
	uint16_t len;
};

typedef void (*meshtastic_phoneapi_data_ready_cb_t)(struct meshtastic_phoneapi *api);
typedef void (*meshtastic_phoneapi_disconnect_cb_t)(struct meshtastic_phoneapi *api);
typedef void (*meshtastic_phoneapi_invalidate_cb_t)(struct meshtastic_phoneapi *api);

enum meshtastic_phoneapi_config_state {
	MESHTASTIC_PHONEAPI_CONFIG_IDLE,
	MESHTASTIC_PHONEAPI_CONFIG_MY_INFO,
	MESHTASTIC_PHONEAPI_CONFIG_DEVICE_UI,
	MESHTASTIC_PHONEAPI_CONFIG_NODE_INFO,
	MESHTASTIC_PHONEAPI_CONFIG_METADATA,
	MESHTASTIC_PHONEAPI_CONFIG_CHANNELS,
	MESHTASTIC_PHONEAPI_CONFIG_CONFIGS,
	MESHTASTIC_PHONEAPI_CONFIG_MODULES,
	MESHTASTIC_PHONEAPI_CONFIG_QUEUE_STATUS,
	MESHTASTIC_PHONEAPI_CONFIG_COMPLETE,
};

struct meshtastic_phoneapi {
	const char *name;
	struct k_mutex lock;
	struct meshtastic_phoneapi_frame *queue;
	struct meshtastic_phoneapi_frame current;
	uint8_t queue_size;
	uint8_t head;
	uint8_t tail;
	uint8_t count;
	bool current_valid;
	uint32_t from_num;
	enum meshtastic_phoneapi_config_state config_state;
	uint8_t config_index;
	uint32_t config_request_id;
	meshtastic_phoneapi_data_ready_cb_t data_ready;
	meshtastic_phoneapi_disconnect_cb_t disconnect;
	meshtastic_phoneapi_invalidate_cb_t invalidate_delivery;
	void *user_data;
};

#if defined(CONFIG_MESHTASTIC_PHONEAPI)
void meshtastic_phoneapi_init(struct meshtastic_phoneapi *api, const char *name,
			      struct meshtastic_phoneapi_frame *queue, uint8_t queue_size,
			      meshtastic_phoneapi_data_ready_cb_t data_ready,
			      meshtastic_phoneapi_disconnect_cb_t disconnect,
			      meshtastic_phoneapi_invalidate_cb_t invalidate_delivery, void *user_data);
void meshtastic_phoneapi_release_current_frame(struct meshtastic_phoneapi *api);
void meshtastic_phoneapi_register(struct meshtastic_phoneapi *api);
void meshtastic_phoneapi_reset(struct meshtastic_phoneapi *api);
void meshtastic_phoneapi_notify_data_ready(struct meshtastic_phoneapi *api);
uint32_t meshtastic_phoneapi_from_num(struct meshtastic_phoneapi *api);
uint32_t meshtastic_phoneapi_pending_count(struct meshtastic_phoneapi *api);
bool meshtastic_phoneapi_pop_frame(struct meshtastic_phoneapi *api,
				   struct meshtastic_phoneapi_frame *frame);
void meshtastic_phoneapi_push_frame_front(struct meshtastic_phoneapi *api,
					  const struct meshtastic_phoneapi_frame *frame);
bool meshtastic_phoneapi_current_frame(struct meshtastic_phoneapi *api,
				       struct meshtastic_phoneapi_frame *frame);
void meshtastic_phoneapi_current_frame_complete(struct meshtastic_phoneapi *api);
void meshtastic_phoneapi_current_frame_reset(struct meshtastic_phoneapi *api);
int meshtastic_phoneapi_encode_fromradio_frame(const meshtastic_FromRadio *from,
					       struct meshtastic_phoneapi_frame *frame);
int meshtastic_phoneapi_next_config_frame(struct meshtastic_phoneapi *api,
					  struct meshtastic_phoneapi_frame *frame);
int meshtastic_phoneapi_enqueue_fromradio(struct meshtastic_phoneapi *api,
					  const meshtastic_FromRadio *from);
void meshtastic_phoneapi_enqueue_my_info(struct meshtastic_phoneapi *api, uint32_t request_id);
void meshtastic_phoneapi_enqueue_rebooted(struct meshtastic_phoneapi *api);
void meshtastic_phoneapi_enqueue_phone_config(struct meshtastic_phoneapi *api, uint32_t request_id);
int meshtastic_phoneapi_set_channel(uint8_t index, const meshtastic_Channel *channel);
void meshtastic_phoneapi_enqueue_queue_status(struct meshtastic_phoneapi *api, int res,
					      uint32_t mesh_packet_id);
void meshtastic_phoneapi_handle_toradio(struct meshtastic_phoneapi *api, const uint8_t *buf,
					size_t len);
void meshtastic_phoneapi_on_packet(const struct meshtastic_packet *packet);
#else
static inline void meshtastic_phoneapi_on_packet(const struct meshtastic_packet *packet)
{
	ARG_UNUSED(packet);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_PHONEAPI_H_ */
