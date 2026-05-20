/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: GPL-3.0
 */

#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(meshtastic_module, Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(meshtastic_settings_apply, Z_LINK_ITERABLE_SUBALIGN)
