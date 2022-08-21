#pragma bank 2

/**
 * Copyright (c) 2020, 2021, 2022 Adrian Siekierka
 *
 * TinyZoo/GB is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * TinyZoo/GB is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with TinyZoo/GB. If not, see <https://www.gnu.org/licenses/>. 
 *
 * TinyZoo/GB includes code derived from the Reconstruction of ZZT, used under
 * the following license terms:
 *
 * Copyright (c) 2020 Adrian Siekierka
 *
 * Based on a reconstruction of code from ZZT,
 * Copyright 1991 Epic MegaGames, used with permission.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <string.h>
#include <gbdk/platform.h>
#include <gbdk/emu_debug.h>

#include "bank_switch.h"
#include "elements.h"
#include "elements_utils.h"
#include "game.h"
#include "gamevars.h"
#include "config.h"
#include "math.h"
#include "oop.h"
#include "renderer_sidebar.h"
#include "sram_alloc.h"
#include "txtwind.h"

#include "element_defs_cycles.inc"

static uint8_t get_color_for_tile_match(uint8_t element, uint8_t color) {
	uint8_t def_color = zoo_element_defs_color[element];
	if (def_color < COLOR_SPECIAL_MIN) {
		return def_color & 0x07;
	} else if (def_color == COLOR_WHITE_ON_CHOICE) {
		return ((color >> 4) & 0x0F) | 8;
	} else {
		return color & 0x0F;
	}
}

#ifdef __GG__
static uint8_t temp7, temp8;
#endif

// TODO: This method is *SLOW*. Really, really *SLOW*.
// TODO (SDCC 4.2.0 upgrade): This is broken, at least as of SDCC r13388 nightly
bool find_tile_on_board(uint8_t *x, uint8_t *y, uint8_t element, uint8_t color) BANKED {
	zoo_tile_t tile;

	temp7 = *x;
	temp8 = *y;

	while (true) {
		if ((++temp7) > BOARD_WIDTH) {
			temp7 = 1;
			if ((++temp8) > BOARD_HEIGHT) {
				return false;
			}
		}

		ZOO_TILE_ASSIGN(tile, temp7, temp8);
		if (tile.element == element) {
			if (color == 0 ||
			    get_color_for_tile_match(tile.element, tile.color) == color) {
				*x = temp7;
				*y = temp8;
				return true;
			}
		}
	}
}

void oop_place_tile(uint8_t x, uint8_t y, uint8_t element, uint8_t color) BANKED {
	if (!ZOO_TILE_WRITEBOUNDS(x, y)) return;

	zoo_tile_t *src_tile = &ZOO_TILE(x, y);

	if (src_tile->element != E_PLAYER) {
		uint8_t new_color = color;
		uint8_t def_color = zoo_element_defs_color[element];
		if (def_color < COLOR_SPECIAL_MIN) {
			new_color = def_color;
		} else {
			if (new_color == 0) {
				new_color = src_tile->color;
			}

			if (new_color == 0) {
				new_color = 0x0F;
			}

			if (def_color == COLOR_WHITE_ON_CHOICE) {
				new_color = ((new_color - 8) << 4) | 0x0F;
			}
		}

		if (src_tile->element == element) {
			src_tile->color = new_color;
		} else {
			board_damage_tile(x, y);
			if (zoo_element_defs_flags[element] & ELEMENT_TYPICALLY_STATTED) {
				add_stat(x, y, element, new_color, zoo_element_defs_cycles[element], &stat_template_default);
			} else {
				src_tile->element = element;
				src_tile->color = new_color;
			}
		}

		board_draw_tile(x, y);
	}
}

uint16_t oop_dataofs_clone(uint16_t loc) BANKED {
	uint8_t len = zoo_stat_data[loc + 3] & 0x7F;
#ifdef DEBUG_PRINTFS
	EMU_printf("cloning dataofs @ %u, len %u, size %u", loc, len, zoo_stat_data_size);
#endif
	memcpy(zoo_stat_data + zoo_stat_data_size, zoo_stat_data + loc, len);
	uint16_t new_pos = zoo_stat_data_size;
	zoo_stat_data_size += len;
	return new_pos;
}

static void oop_dataofs_free(uint16_t loc) {
	uint8_t len = zoo_stat_data[loc + 3] & 0x7F;
#ifdef DEBUG_PRINTFS
	EMU_printf("freeing dataofs @ %u, len %u, size %u", loc, len, zoo_stat_data_size);
#endif
	memmove(zoo_stat_data + loc, zoo_stat_data + loc + len, zoo_stat_data_size - loc - len);
	zoo_stat_data_size -= len;

	zoo_stat_t *stat = &ZOO_STAT(0);
	uint8_t stat_id = 0;
	for (; stat_id <= zoo_stat_count; stat_id++, stat++) {
		if (stat->data_ofs != 0xFFFF) {
			if (stat->data_ofs > loc) {
#ifdef DEBUG_PRINTFS
				EMU_printf("shifting stat %d from %u", stat_id, stat->data_ofs);
#endif
				stat->data_ofs -= len;
			}
		}
	}
}

void oop_dataofs_free_if_unused(uint16_t loc, uint8_t except_id) BANKED {
	zoo_stat_t *stat = &ZOO_STAT(0);
	uint8_t stat_id = 0;
	for (; stat_id <= zoo_stat_count; stat_id++, stat++) {
		if (stat_id != except_id && stat->data_ofs == loc) {
			return;
		}
	}
	oop_dataofs_free(loc);
}

extern uint16_t oop_window_zzt_lines;

#define TWL_OFS (TXTWIND_LINE_HEADER_LEN - 1)

bool oop_handle_txtwind(void) BANKED {
	if (oop_window_zzt_lines > 1) {
		return txtwind_run(RENDER_MODE_TXTWIND) != 255;
	} else if (oop_window_zzt_lines == 1) {
		uint8_t sram_ptr_data[9];
		sram_ptr_t sram_ptr;

		ZOO_ENABLE_RAM;
		sram_ptr.bank = 0;
		sram_ptr.position = SRAM_TEXT_WINDOW_POS;
		sram_read(&sram_ptr, sram_ptr_data, sizeof(sram_ptr_data));
		ZOO_DISABLE_RAM;

		// TODO: This doesn't handle 1-liners prefixed with $ or !.
		// Let's hope they don't actually occur much?

		init_display_message(200, true);
		switch (txtwind_lines) {
			case 1:
				sidebar_show_message(NULL, 3, NULL, 3,
					(const char*) (*((uint16_t**) (sram_ptr_data))) + TWL_OFS, sram_ptr_data[2]);
				break;
			case 2:
				sidebar_show_message(NULL, 3,
					(const char*) (*((uint16_t**) (sram_ptr_data))) + TWL_OFS, sram_ptr_data[2],
					(const char*) (*((uint16_t**) (sram_ptr_data + 3))) + TWL_OFS, sram_ptr_data[5]);
				break;
			default:
				sidebar_show_message(
					(const char*) (*((uint16_t**) (sram_ptr_data))) + TWL_OFS, sram_ptr_data[2],
					(const char*) (*((uint16_t**) (sram_ptr_data + 3))) + TWL_OFS, sram_ptr_data[5],
					(const char*) (*((uint16_t**) (sram_ptr_data + 6))) + TWL_OFS, sram_ptr_data[8]);
				break;
		}

		return false;
	} else {
		return false;
	}
}
