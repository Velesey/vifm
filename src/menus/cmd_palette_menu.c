/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "cmd_palette_menu.h"

#include <stdint.h> /* intptr_t */
#include <stdlib.h> /* free() */
#include <string.h> /* strdup() strlen() */
#include <wchar.h> /* wchar_t wcscmp() */

#include "../compat/reallocarray.h"
#include "../engine/keys.h"
#include "../engine/cmds.h"
#include "../engine/mode.h"
#include "../modes/cmdline.h"
#include "../modes/menu.h"
#include "../modes/modes.h"
#include "../modes/wk.h"
#include "../ui/ui.h"
#include "../bracket_notation.h"
#include "../utils/macros.h"
#include "../utils/str.h"
#include "../utils/string_array.h"
#include "../cmd_core.h"
#include "../cmd_handlers.h"
#include "menus.h"

/* Minimal length of command name column. */
#define CMDNAME_COLUMN_MIN_WIDTH 10
#define KEY_COLUMN_MIN_WIDTH 10

typedef enum
{
	PA_COMMAND = 1,
	PA_KEYS = 2,
	PA_NEEDS_ARGS = 0x100,
}
PaletteAction;

static int execute_cmd_palette_cb(view_t *view, menu_data_t *m);
static int count_custom_commands(char *list[]);
static int append_source_item(char item[], char action[],
		PaletteAction type);
static int append_builtin_commands(menu_data_t *m, size_t cmdname_width);
static int append_custom_commands(menu_data_t *m, char *list[],
		size_t cmdname_width);
static size_t calc_cmdname_width(char *custom_cmds[]);
static void append_key_action(const wchar_t lhs[], const wchar_t rhs[],
		const char descr[]);
static KHandlerResponse cmd_palette_khandler(view_t *view, menu_data_t *m,
		const wchar_t keys[]);
static int filter_cmd_palette(menu_data_t *m, const char pattern[]);
static int append_filtered_item(menu_data_t *m, int index);
static void clear_menu_items(menu_data_t *m);
static void reset_source_items(void);
static void cleanup_cmd_palette(menu_data_t *m);
static int item_matches(const char item[], const char pattern[]);

/* Menu object is global to make it available in append_key_action(). */
static menu_data_t m;
static char **source_items;
static char **source_data;
static void **source_void_data;
static int source_len;

int
show_cmd_palette_menu(view_t *view)
{
	char **custom_cmds = vle_cmds_list_udcs();
	const int custom_count = count_custom_commands(custom_cmds);
	size_t cmdname_width = calc_cmdname_width(custom_cmds);

	reset_source_items();

	menus_init_data(&m, view, strdup("Command Palette"),
			strdup("No commands available"));
	m.execute_handler = &execute_cmd_palette_cb;
	m.key_handler = &cmd_palette_khandler;
	m.filter_handler = &filter_cmd_palette;
	m.cleanup_handler = &cleanup_cmd_palette;

	if(append_builtin_commands(&m, cmdname_width) != 0 ||
			append_custom_commands(&m, custom_cmds, cmdname_width) != 0)
	{
		goto fail;
	}
	free_string_array(custom_cmds, custom_count*2);
	custom_cmds = NULL;

	vle_keys_list(NORMAL_MODE, &append_key_action, /*user_only=*/0);
	if(filter_cmd_palette(&m, "") != 0)
	{
		goto fail;
	}

	if(menus_enter(&m, view) != 0)
	{
		goto fail;
	}

	menu_data_t *const current = modmenu_get_current();
	modcline_in_menu(CLS_MENU_FILTER, /*initial=*/"", current);
	return 0;

fail:
	free_string_array(custom_cmds, custom_count*2);
	menus_reset_data(&m);
	return 1;
}

/* Callback that is called when menu item is selected.  Should return non-zero
 * to stay in menu mode. */
static int
execute_cmd_palette_cb(view_t *view, menu_data_t *m)
{
	if(m->len == 0)
	{
		return 1;
	}

	const char *const action = m->data[m->pos];
	const PaletteAction type = (intptr_t)m->void_data[m->pos];

	if(type & PA_KEYS)
	{
		wchar_t *const keys = substitute_specs(action);
		if(keys != NULL)
		{
			(void)vle_keys_exec(keys);
			free(keys);
		}
		return 0;
	}

	if(type & PA_NEEDS_ARGS)
	{
		char *const initial = format_str("%s ", action);
		if(initial != NULL)
		{
			modcline_enter(CLS_COMMAND, initial);
			free(initial);
		}
		return 0;
	}

	cmds_dispatch1(action, view, CIT_COMMAND);
	return 0;
}

/* Counts custom command name/description pairs. */
static int
count_custom_commands(char *list[])
{
	return (list == NULL) ? 0 : count_strings(list)/2;
}

/* Computes width of the command-name column. */
static size_t
calc_cmdname_width(char *custom_cmds[])
{
	size_t cmdname_width = CMDNAME_COLUMN_MIN_WIDTH;
	size_t i;

	for(i = 0U; i < cmds_list_size; ++i)
	{
		if(cmds_list[i].name[0] != '\0')
		{
			cmdname_width = MAX(cmdname_width, strlen(cmds_list[i].name));
		}
	}

	if(custom_cmds != NULL)
	{
		int j;
		for(j = 0; custom_cmds[j] != NULL; j += 2)
		{
			cmdname_width = MAX(cmdname_width, strlen(custom_cmds[j]));
		}
	}

	return cmdname_width;
}

/* Adds item to the menu.  Takes ownership of item and action on success. */
static int
append_source_item(char item[], char action[], PaletteAction type)
{
	char **items;
	char **data;
	void **void_data;

	items = reallocarray(source_items, source_len + 1, sizeof(*source_items));
	if(items == NULL)
	{
		free(item);
		free(action);
		return 1;
	}
	source_items = items;

	data = reallocarray(source_data, source_len + 1, sizeof(*source_data));
	if(data == NULL)
	{
		free(item);
		free(action);
		return 1;
	}
	source_data = data;

	void_data = reallocarray(source_void_data, source_len + 1,
			sizeof(*source_void_data));
	if(void_data == NULL)
	{
		free(item);
		free(action);
		return 1;
	}
	source_void_data = void_data;

	source_items[source_len] = item;
	source_data[source_len] = action;
	source_void_data[source_len] = (void *)(intptr_t)type;
	++source_len;
	return 0;
}

/* Adds builtin commands to the menu. */
static int
append_builtin_commands(menu_data_t *m, size_t cmdname_width)
{
	size_t i;
	for(i = 0U; i < cmds_list_size; ++i)
	{
		if(cmds_list[i].name[0] == '\0')
		{
			continue;
		}

		char *const item = format_str(":%-*s %s", (int)cmdname_width,
				cmds_list[i].name, cmds_list[i].descr);
		char *const action = strdup(cmds_list[i].name);
		PaletteAction type = PA_COMMAND;
		if(cmds_list[i].min_args > 0)
		{
			type |= PA_NEEDS_ARGS;
		}

		if(append_source_item(item, action, type) != 0)
		{
			return 1;
		}
	}
	return 0;
}

/* Adds user-defined and foreign commands to the menu. */
static int
append_custom_commands(menu_data_t *m, char *list[], size_t cmdname_width)
{
	int i;
	if(list == NULL)
	{
		return 0;
	}

	for(i = 0; list[i] != NULL; i += 2)
	{
		char *const item = format_str(":%-*s %s", (int)cmdname_width, list[i],
				list[i + 1]);
		char *const action = strdup(list[i]);

		if(append_source_item(item, action, PA_COMMAND) != 0)
		{
			return 1;
		}
	}
	return 0;
}

/* Adds a normal-mode key action to the menu. */
static void
append_key_action(const wchar_t lhs[], const wchar_t rhs[], const char descr[])
{
	if(lhs[0] == L'\0' || rhs[0] != L'\0' || descr[0] == '\0')
	{
		return;
	}

	char *const keys = wstr_to_spec(lhs);
	if(keys == NULL)
	{
		return;
	}

	char *const item = format_str("%-*s %s", KEY_COLUMN_MIN_WIDTH, keys, descr);
	if(append_source_item(item, keys, PA_KEYS) != 0)
	{
		return;
	}
}

/* Menu-specific shortcut handler. */
static KHandlerResponse
cmd_palette_khandler(view_t *view, menu_data_t *m, const wchar_t keys[])
{
	(void)view;

	if(wcscmp(keys, WK_DELETE) == 0 || wcscmp(keys, WK_C_h) == 0)
	{
		if(filter_cmd_palette(m, "") == 0)
		{
			menus_partial_redraw(m->state);
			menus_set_pos(m->state, 0);
			return KHR_REFRESH_WINDOW;
		}
	}

	return KHR_UNHANDLED;
}

/* Filters visible menu items by a case-insensitive substring. */
static int
filter_cmd_palette(menu_data_t *m, const char pattern[])
{
	int i;

	clear_menu_items(m);

	for(i = 0; i < source_len; ++i)
	{
		if(item_matches(source_items[i], pattern) &&
				append_filtered_item(m, i) != 0)
		{
			return 1;
		}
	}

	m->top = 0;
	m->pos = 0;
	m->hor_pos = 0;
	return 0;
}

/* Appends a copy of a source item to the visible menu. */
static int
append_filtered_item(menu_data_t *m, int index)
{
	char **items;
	char **data;
	void **void_data;

	items = reallocarray(m->items, m->len + 1, sizeof(*m->items));
	if(items == NULL)
	{
		return 1;
	}
	m->items = items;

	data = reallocarray(m->data, m->len + 1, sizeof(*m->data));
	if(data == NULL)
	{
		return 1;
	}
	m->data = data;

	void_data = reallocarray(m->void_data, m->len + 1,
			sizeof(*m->void_data));
	if(void_data == NULL)
	{
		return 1;
	}
	m->void_data = void_data;

	m->items[m->len] = strdup(source_items[index]);
	m->data[m->len] = strdup(source_data[index]);
	m->void_data[m->len] = source_void_data[index];
	if(m->items[m->len] == NULL || m->data[m->len] == NULL)
	{
		free(m->items[m->len]);
		free(m->data[m->len]);
		return 1;
	}

	++m->len;
	return 0;
}

/* Clears currently visible menu items. */
static void
clear_menu_items(menu_data_t *m)
{
	free_string_array(m->items, m->len);
	free_string_array(m->data, m->len);
	free(m->void_data);
	m->items = NULL;
	m->data = NULL;
	m->void_data = NULL;
	m->len = 0;
}

/* Releases source items. */
static void
reset_source_items(void)
{
	free_string_array(source_items, source_len);
	free_string_array(source_data, source_len);
	free(source_void_data);
	source_items = NULL;
	source_data = NULL;
	source_void_data = NULL;
	source_len = 0;
}

/* Releases menu-specific state. */
static void
cleanup_cmd_palette(menu_data_t *m)
{
	(void)m;

	reset_source_items();
}

/* Checks whether item satisfies current filter. */
static int
item_matches(const char item[], const char pattern[])
{
	return pattern[0] == '\0' || strcasestr(item, pattern) != NULL;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
