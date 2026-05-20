/* SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
 * SPDX-License-Identifier: GPL-3.0
 */

#ifndef ZEPHYR_SUBSYS_MESHTASTIC_MODULES_H_
#define ZEPHYR_SUBSYS_MESHTASTIC_MODULES_H_

#include <zephyr/sys/iterable_sections.h>

#include "meshtastic_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Module receives every packet when @ref MESHTASTIC_MODULE_ALL_PACKETS is set. */
#define MESHTASTIC_MODULE_ALL_PACKETS BIT(0)

struct meshtastic_module {
	const char *name;
	uint32_t portnum;
	uint8_t flags;
	void (*on_packet)(const struct meshtastic_packet *packet);
	/**
	 * Build a reply to @p req. Return 0 when @p reply is ready to send,
	 * -ENOENT when this request should be ignored (no reply), or another
	 * negative errno on failure.
	 */
	int (*alloc_reply)(const struct meshtastic_packet *req, struct meshtastic_packet *reply);
};

/**
 * @brief Register a Meshtastic port handler in the iterable module section.
 */
#define MESHTASTIC_MODULE_DEFINE(_name, _portnum, _flags, _on_packet, _alloc_reply)                \
	static const STRUCT_SECTION_ITERABLE(meshtastic_module, _meshtastic_module_##_name) = {    \
		.name = STRINGIFY(_name), .portnum = (_portnum), .flags = (_flags),                \
				  .on_packet = _on_packet, .alloc_reply = _alloc_reply,            \
	}

void meshtastic_dispatch_modules(const struct meshtastic_packet *packet);

/**
 * @brief Fill standard reply header fields per Meshtastic setReplyTo().
 *
 * Correlates the reply with @p req via Data.request_id (not reply_id).
 */
static inline void meshtastic_packet_set_reply_to(struct meshtastic_packet *reply,
						  const struct meshtastic_packet *req)
{
	reply->to = req->from;
	if (req->channel_index != MESHTASTIC_CHANNEL_INDEX_INVALID) {
		reply->channel_index = req->channel_index;
	} else if (req->channel != 0U) {
		reply->channel = req->channel;
	}
	reply->request_id = req->id;
	reply->reply_id = 0U;
	reply->want_response = false;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_MESHTASTIC_MODULES_H_ */
