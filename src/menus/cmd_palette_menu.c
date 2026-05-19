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
#include <wchar.h> /* wchar_t */

#include "../compat/reallocarray.h"
#include "../engine/keys.h"
#include "../engine/cmds.h"
#include "../engine/mode.h"
#include "../modes/cmdline.h"
#include "../modes/menu.h"
#include "../modes/modes.h"
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
static int append_item(menu_data_t *m, char item[], char action[],
		PaletteAction type);
static int append_builtin_commands(menu_data_t *m, size_t cmdname_width);
static int append_custom_commands(menu_data_t *m, char *list[],
		size_t cmdname_width);
static size_t calc_cmdname_width(char *custom_cmds[]);
static void append_key_action(const wchar_t lhs[], const wchar_t rhs[],
		const char descr[]);

/* Menu object is global to make it available in append_key_action(). */
static menu_data_t m;

int
show_cmd_palette_menu(view_t *view)
{
	char **custom_cmds = vle_cmds_list_udcs();
	const int custom_count = count_custom_commands(custom_cmds);
	size_t cmdname_width = calc_cmdname_width(custom_cmds);

	menus_init_data(&m, view, strdup("Command Palette"),
			strdup("No commands available"));
	m.execute_handler = &execute_cmd_palette_cb;

	if(append_builtin_commands(&m, cmdname_width) != 0 ||
			append_custom_commands(&m, custom_cmds, cmdname_width) != 0)
	{
		goto fail;
	}
	free_string_array(custom_cmds, custom_count*2);
	custom_cmds = NULL;

	vle_keys_list(NORMAL_MODE, &append_key_action, /*user_only=*/0);

	if(menus_enter(&m, view) != 0)
	{
		goto fail;
	}

	menu_data_t *const current = modmenu_get_current();
	menus_search_reset(current->state, /*backward=*/0, /*new_repeat_count=*/1);
	modcline_in_menu(CLS_MENU_FSEARCH, /*initial=*/"", current);
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
append_item(menu_data_t *m, char item[], char action[], PaletteAction type)
{
	char **items;
	char **data;
	void **void_data;

	items = reallocarray(m->items, m->len + 1, sizeof(*m->items));
	if(items == NULL)
	{
		free(item);
		free(action);
		return 1;
	}
	m->items = items;

	data = reallocarray(m->data, m->len + 1, sizeof(*m->data));
	if(data == NULL)
	{
		free(item);
		free(action);
		return 1;
	}
	m->data = data;

	void_data = reallocarray(m->void_data, m->len + 1, sizeof(*m->void_data));
	if(void_data == NULL)
	{
		free(item);
		free(action);
		return 1;
	}
	m->void_data = void_data;

	m->items[m->len] = item;
	m->data[m->len] = action;
	m->void_data[m->len] = (void *)(intptr_t)type;
	++m->len;
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

		if(append_item(m, item, action, type) != 0)
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

		if(append_item(m, item, action, PA_COMMAND) != 0)
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
	if(append_item(&m, item, keys, PA_KEYS) != 0)
	{
		return;
	}
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
