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

#include <ctype.h> /* isspace() toupper() */
#include <stdint.h> /* intptr_t */
#include <stdio.h> /* FILE fclose() fgets() fopen() snprintf() */
#include <stdlib.h> /* free() */
#include <string.h> /* strdup() strlen() */
#include <wchar.h> /* wchar_t wcscmp() */

#include "../cfg/config.h"
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
#include "../utils/path.h"
#include "../utils/str.h"
#include "../utils/string_array.h"
#include "../utils/utf8.h"
#include "../utils/utils.h"
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
static int append_source_item(char item[], char attrs[], char action[],
		PaletteAction type);
static int append_builtin_commands(menu_data_t *m, size_t cmdname_width);
static int append_custom_commands(menu_data_t *m, char *list[],
		size_t cmdname_width);
static size_t calc_cmdname_width(char *custom_cmds[]);
static char * make_palette_descr(const char tag[], const char fallback[]);
static char * make_help_attrs(const char item[]);
static char * make_command_descr(const char name[], const char fallback[]);
static void append_key_action(const wchar_t lhs[], const wchar_t rhs[],
		const char descr[]);
static char * make_key_descr(const char keys[], const char fallback[]);
static char * make_key_help_tag(const char keys[]);
static int append_key_tag_part(char **tag, size_t *tag_len,
		const char part[], int separator);
static char * read_help_descr(const char tag[]);
static FILE * open_vim_help(void);
static char * extract_same_line_descr(const char line[], const char marker[]);
static char * clean_help_text(const char text[]);
static char * strndup_local(const char str[], size_t len);
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
static char **source_item_attrs;
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

/* Makes a palette description by placing static code metadata first and a
 * matching help excerpt after it. */
static char *
make_palette_descr(const char tag[], const char fallback[])
{
	char *const help = read_help_descr(tag);
	if(help == NULL || help[0] == '\0' || strcmp(help, fallback) == 0)
	{
		free(help);
		return strdup(fallback);
	}

	char *const descr = format_str("%s; %s", fallback, help);
	free(help);
	return descr == NULL ? strdup(fallback) : descr;
}

/* Makes attributes that mark help-derived part of an item as muted. */
static char *
make_help_attrs(const char item[])
{
	const char *const help = (item == NULL) ? NULL : strstr(item, "; ");
	if(help == NULL)
	{
		return NULL;
	}

	const size_t item_width = utf8_strsw(item);
	const size_t help_width = utf8_strsw(help + 2);
	char *const attrs = malloc(item_width + 1U);
	if(attrs == NULL)
	{
		return NULL;
	}

	const size_t help_start = item_width - help_width;
	memset(attrs, ' ', help_start);
	memset(attrs + help_start, 'a', help_width);
	attrs[item_width] = '\0';
	return attrs;
}

/* Adds item to the menu.  Takes ownership of item, attrs and action on
 * success. */
static int
append_source_item(char item[], char attrs[], char action[], PaletteAction type)
{
	char **items;
	char **item_attrs;
	char **data;
	void **void_data;

	items = reallocarray(source_items, source_len + 1, sizeof(*source_items));
	if(items == NULL)
	{
		free(item);
		free(attrs);
		free(action);
		return 1;
	}
	source_items = items;

	item_attrs = reallocarray(source_item_attrs, source_len + 1,
			sizeof(*source_item_attrs));
	if(item_attrs == NULL)
	{
		free(item);
		free(attrs);
		free(action);
		return 1;
	}
	source_item_attrs = item_attrs;

	data = reallocarray(source_data, source_len + 1, sizeof(*source_data));
	if(data == NULL)
	{
		free(item);
		free(attrs);
		free(action);
		return 1;
	}
	source_data = data;

	void_data = reallocarray(source_void_data, source_len + 1,
			sizeof(*source_void_data));
	if(void_data == NULL)
	{
		free(item);
		free(attrs);
		free(action);
		return 1;
	}
	source_void_data = void_data;

	source_items[source_len] = item;
	source_item_attrs[source_len] = attrs;
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

		char *const descr = make_command_descr(cmds_list[i].name,
				cmds_list[i].descr);
		char *const item = format_str(":%-*s %s", (int)cmdname_width,
				cmds_list[i].name, descr);
		char *const attrs = make_help_attrs(item);
		char *const action = strdup(cmds_list[i].name);
		free(descr);
		PaletteAction type = PA_COMMAND;
		if(cmds_list[i].min_args > 0)
		{
			type |= PA_NEEDS_ARGS;
		}

		if(append_source_item(item, attrs, action, type) != 0)
		{
			return 1;
		}
	}
	return 0;
}

/* Makes description of a builtin command from help, falling back to static
 * command metadata if help isn't available. */
static char *
make_command_descr(const char name[], const char fallback[])
{
	char *const tag = format_str("vifm-:%s", name);
	if(tag == NULL)
	{
		return strdup(fallback);
	}

	char *const descr = make_palette_descr(tag, fallback);
	free(tag);
	return descr;
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

		if(append_source_item(item, NULL, action, PA_COMMAND) != 0)
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

	char *const help_descr = make_key_descr(keys, descr);
	char *const item = format_str("%-*s %s", KEY_COLUMN_MIN_WIDTH, keys,
			help_descr);
	char *const attrs = make_help_attrs(item);
	free(help_descr);

	if(append_source_item(item, attrs, keys, PA_KEYS) != 0)
	{
		return;
	}
}

/* Makes description of a key action from help, falling back to key metadata if
 * help isn't available. */
static char *
make_key_descr(const char keys[], const char fallback[])
{
	char *const tag = make_key_help_tag(keys);
	if(tag == NULL)
	{
		return strdup(fallback);
	}

	char *const descr = make_palette_descr(tag, fallback);
	free(tag);
	return descr;
}

/* Converts bracket notation to the corresponding Vim-help tag.  This covers
 * the common builtin forms and intentionally falls back to static descriptions
 * for anything more exotic. */
static char *
make_key_help_tag(const char keys[])
{
	char *tag = strdup("vifm-");
	size_t tag_len = strlen(tag);
	int first = 1;
	int previous_named = 0;
	const char *p = keys;

	while(*p != '\0')
	{
		char *part;
		int err;
		int named = 0;

		if(*p == '<')
		{
			const char *const end = strchr(p, '>');
			if(end == NULL)
			{
				free(tag);
				return NULL;
			}

			named = 1;
			const char *name = p + 1;
			const size_t name_len = end - name;
			if(name_len == 3 && strncasecmp(name, "c-", 2) == 0)
			{
				part = format_str("CTRL-%c", toupper((unsigned char)name[2]));
			}
			else if(name_len == 5 && strncasecmp(name, "s-tab", 5) == 0)
			{
				part = strdup("SHIFT-Tab");
			}
			else if(name_len == 2 && strncasecmp(name, "cr", 2) == 0)
			{
				part = strdup("Enter");
			}
			else if(name_len == 3 && strncasecmp(name, "esc", 3) == 0)
			{
				part = strdup("Escape");
			}
			else if(name_len == 5 && strncasecmp(name, "space", 5) == 0)
			{
				part = strdup("Space");
			}
			else if(name_len == 3 && strncasecmp(name, "tab", 3) == 0)
			{
				part = strdup("Tab");
			}
			else if(name_len == 6 && strncasecmp(name, "pageup", 6) == 0)
			{
				part = strdup("PageUp");
			}
			else if(name_len == 8 && strncasecmp(name, "pagedown", 8) == 0)
			{
				part = strdup("PageDown");
			}
			else if(name_len == 2 && strncasecmp(name, "lt", 2) == 0)
			{
				part = strdup("<");
			}
			else
			{
				part = NULL;
			}
			p = end + 1;
		}
		else
		{
			char buf[2] = { *p++, '\0' };
			named = (buf[0] == '|');
			part = strdup(named ? "bar" : buf);
		}

		if(part == NULL)
		{
			free(tag);
			return NULL;
		}

		err = append_key_tag_part(&tag, &tag_len, part,
				!first && (previous_named || named));
		free(part);
		if(err != 0)
		{
			free(tag);
			return NULL;
		}
		first = 0;
		previous_named = named;
	}

	return tag;
}

/* Appends a key component to a Vim-help tag. */
static int
append_key_tag_part(char **tag, size_t *tag_len, const char part[],
		int separator)
{
	if(separator && strappendch(tag, tag_len, '_') != 0)
	{
		return 1;
	}
	return strappend(tag, tag_len, part);
}

/* Reads a short description of a help topic. */
static char *
read_help_descr(const char tag[])
{
	char marker[256];
	char line[1024];
	FILE *fp;
	int found = 0;
	int collecting = 0;
	char *descr = NULL;
	size_t descr_len = 0U;

	snprintf(marker, sizeof(marker), "*%s*", tag);

	fp = open_vim_help();
	if(fp == NULL)
	{
		return NULL;
	}

	while(fgets(line, sizeof(line), fp) != NULL)
	{
		chomp(line);

		if(!found)
		{
			if(strstr(line, marker) != NULL)
			{
				char *const same_line = extract_same_line_descr(line, marker);
				if(same_line != NULL)
				{
					fclose(fp);
					return same_line;
				}
				found = 1;
			}
			continue;
		}

		const char *text = skip_whitespace(line);
		if(text[0] == '\0')
		{
			if(collecting)
			{
				break;
			}
			continue;
		}
		if(strstr(text, "*vifm-") != NULL)
		{
			break;
		}
		if(!collecting && (text[0] == ':' || text[0] == '[' || text[0] == '<'))
		{
			continue;
		}

		char *const clean = clean_help_text(text);
		if(clean == NULL || clean[0] == '\0')
		{
			free(clean);
			continue;
		}

		if(collecting && strappendch(&descr, &descr_len, ' ') != 0)
		{
			free(clean);
			break;
		}
		if(strappend(&descr, &descr_len, clean) != 0)
		{
			free(clean);
			break;
		}
		free(clean);
		collecting = 1;
	}

	fclose(fp);

	if(descr == NULL)
	{
		return NULL;
	}
	return descr;
}

/* Opens Vim-formatted application help file. */
static FILE *
open_vim_help(void)
{
	char path[PATH_MAX + 1];

	build_path(path, sizeof(path), get_installed_data_dir(),
			"vim-doc/doc/" VIFM_VIM_HELP);
	FILE *fp = fopen(path, "r");
	if(fp != NULL)
	{
		return fp;
	}

	build_path(path, sizeof(path), get_installed_data_dir(),
			"vim/doc/app/" VIFM_VIM_HELP);
	fp = fopen(path, "r");
	if(fp != NULL)
	{
		return fp;
	}

#ifdef VIFM_SOURCE_DATA_DIR
	build_path(path, sizeof(path), VIFM_SOURCE_DATA_DIR,
			"vim/doc/app/" VIFM_VIM_HELP);
	return fopen(path, "r");
#else
	return NULL;
#endif
}

/* Extracts descriptions that are placed before a tag on the same line. */
static char *
extract_same_line_descr(const char line[], const char marker[])
{
	const char *const marker_pos = strstr(line, marker);
	if(marker_pos == NULL)
	{
		return NULL;
	}

	const char *desc = NULL;
	const char *p = line;
	while((p = strstr(p, " - ")) != NULL && p < marker_pos)
	{
		desc = p + 3;
		p += 3;
	}
	if(desc == NULL)
	{
		return NULL;
	}

	char *const raw = strndup_local(desc, marker_pos - desc);
	if(raw == NULL)
	{
		return NULL;
	}

	char *const clean = clean_help_text(raw);
	free(raw);
	return clean;
}

/* Normalizes a snippet of Vim help text for single-line menu display. */
static char *
clean_help_text(const char text[])
{
	char *result = NULL;
	size_t result_len = 0U;
	int pending_space = 0;
	const char *p;
	const char *end;

	p = skip_whitespace(text);
	end = p + strlen(p);
	while(end > p && isspace((unsigned char)end[-1]))
	{
		--end;
	}

	for(; p < end; ++p)
	{
		if(isspace((unsigned char)*p))
		{
			pending_space = (result_len != 0U);
			continue;
		}

		if(pending_space)
		{
			if(strappendch(&result, &result_len, ' ') != 0)
			{
				free(result);
				return NULL;
			}
			pending_space = 0;
		}

		if(strappendch(&result, &result_len, *p) != 0)
		{
			free(result);
			return NULL;
		}
	}

	return (result == NULL) ? strdup("") : result;
}

/* Local replacement for strndup(), which isn't available everywhere. */
static char *
strndup_local(const char str[], size_t len)
{
	char *const result = malloc(len + 1U);
	if(result == NULL)
	{
		return NULL;
	}

	memcpy(result, str, len);
	result[len] = '\0';
	return result;
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
	char **item_attrs;
	char **data;
	void **void_data;

	items = reallocarray(m->items, m->len + 1, sizeof(*m->items));
	if(items == NULL)
	{
		return 1;
	}
	m->items = items;

	item_attrs = reallocarray(m->item_attrs, m->len + 1,
			sizeof(*m->item_attrs));
	if(item_attrs == NULL)
	{
		return 1;
	}
	m->item_attrs = item_attrs;

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
	m->item_attrs[m->len] = (source_item_attrs[index] == NULL)
	                       ? NULL
	                       : strdup(source_item_attrs[index]);
	m->data[m->len] = strdup(source_data[index]);
	m->void_data[m->len] = source_void_data[index];
	if(m->items[m->len] == NULL || m->data[m->len] == NULL ||
			(source_item_attrs[index] != NULL && m->item_attrs[m->len] == NULL))
	{
		free(m->items[m->len]);
		free(m->item_attrs[m->len]);
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
	free_string_array(m->item_attrs, m->len);
	free_string_array(m->data, m->len);
	free(m->void_data);
	m->items = NULL;
	m->item_attrs = NULL;
	m->data = NULL;
	m->void_data = NULL;
	m->len = 0;
}

/* Releases source items. */
static void
reset_source_items(void)
{
	free_string_array(source_items, source_len);
	free_string_array(source_item_attrs, source_len);
	free_string_array(source_data, source_len);
	free(source_void_data);
	source_items = NULL;
	source_item_attrs = NULL;
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
