/* vifm
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
#include <stdio.h> /* FILE fclose() fprintf() */
#include <stdlib.h> /* free() */
#include <string.h> /* strcmp() strdup() strlen() */
#include <wchar.h> /* wchar_t */

#include "../compat/fs_limits.h"
#include "../compat/os.h"
#include "../compat/reallocarray.h"
#include "../engine/cmds.h"
#include "../engine/keys.h"
#include "../modes/cmdline.h"
#include "../modes/modes.h"
#include "../status.h"
#include "../ui/statusbar.h"
#include "../ui/ui.h"
#include "../bracket_notation.h"
#include "../background.h"
#include "../utils/macros.h"
#include "../utils/fs.h"
#include "../utils/path.h"
#include "../utils/str.h"
#include "../utils/string_array.h"
#include "../utils/utils.h"
#include "../cmd_core.h"
#include "../cmd_handlers.h"
#include "../filelist.h"

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

static int execute_cmd_palette_action(view_t *view, const char action[],
		PaletteAction type);
static int build_cmd_palette_source(void);
static int write_fzf_input(FILE *fp);
static int pick_fzf_item(FILE *input, int *picked);
static int pick_fzf_file(view_t *view, char **picked);
static void goto_picked_file(view_t *view, const char path[]);
static int count_custom_commands(char *list[]);
static int append_source_item(char item[], char action[], PaletteAction type);
static int append_builtin_commands(size_t cmdname_width);
static int append_custom_commands(char *list[], size_t cmdname_width);
static size_t calc_cmdname_width(char *custom_cmds[]);
static void append_key_action(const wchar_t lhs[], const wchar_t rhs[],
		const char descr[]);
static int find_source_item(const char item[]);
static void reset_source_items(void);

static char **source_items;
static char **source_data;
static void **source_void_data;
static int source_len;

int
show_cmd_palette_fzf(view_t *view)
{
	if(find_cmd_in_path("fzf", /*path_len=*/0UL, /*path=*/NULL) != 0)
	{
		ui_sb_err("fzf executable not found");
		return 1;
	}

	if(build_cmd_palette_source() != 0)
	{
		reset_source_items();
		return 1;
	}

	FILE *input = os_tmpfile();
	if(input == NULL)
	{
		reset_source_items();
		ui_sb_err("Failed to create temporary file for fzf");
		return 1;
	}

	if(write_fzf_input(input) != 0)
	{
		fclose(input);
		reset_source_items();
		ui_sb_err("Failed to prepare fzf input");
		return 1;
	}

	int picked = -1;
	const int err = pick_fzf_item(input, &picked);
	fclose(input);

	if(err == 0 && picked >= 0 && picked < source_len)
	{
		const PaletteAction type = (intptr_t)source_void_data[picked];
		(void)execute_cmd_palette_action(view, source_data[picked], type);
	}

	reset_source_items();
	return err;
}

int
show_find_fzf(view_t *view)
{
	char *picked = NULL;

	if(find_cmd_in_path("fzf", /*path_len=*/0UL, /*path=*/NULL) != 0)
	{
		ui_sb_err("fzf executable not found");
		return 1;
	}

	const int err = pick_fzf_file(view, &picked);
	if(err == 0 && picked != NULL)
	{
		goto_picked_file(view, picked);
	}

	free(picked);
	return err;
}

/* Executes a command palette action. */
static int
execute_cmd_palette_action(view_t *view, const char action[], PaletteAction type)
{
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

/* Builds source entries for command palette. */
static int
build_cmd_palette_source(void)
{
	char **custom_cmds = vle_cmds_list_udcs();
	const int custom_count = count_custom_commands(custom_cmds);
	const size_t cmdname_width = calc_cmdname_width(custom_cmds);

	reset_source_items();

	const int err = append_builtin_commands(cmdname_width) != 0 ||
	                append_custom_commands(custom_cmds, cmdname_width) != 0;
	free_string_array(custom_cmds, custom_count*2);
	if(err)
	{
		return 1;
	}

	vle_keys_list(NORMAL_MODE, &append_key_action, /*user_only=*/0);
	return 0;
}

/* Writes fzf input as plain visible lines. */
static int
write_fzf_input(FILE *fp)
{
	int i;
	for(i = 0; i < source_len; ++i)
	{
		if(fprintf(fp, "%s\n", source_items[i]) < 0)
		{
			return 1;
		}
	}
	return 0;
}

/* Lets user pick item through fzf. */
static int
pick_fzf_item(FILE *input, int *picked)
{
	FILE *output;
	char **lines;
	int nlines;

	char cmd[] = "fzf --prompt=Command: --layout=reverse --height=100%";

	ui_shutdown();
	pid_t pid = bg_run_and_capture(cmd, /*user_sh=*/0, input, &output,
			/*err=*/NULL);
	if(pid == (pid_t)-1)
	{
		recover_after_shellout();
		update_screen(UT_FULL);
		return 1;
	}

	lines = read_stream_lines(output, &nlines, /*null_sep_heuristic=*/1,
			/*cb=*/NULL, /*arg=*/NULL);
	fclose(output);
	recover_after_shellout();
	update_screen(UT_FULL);

	if(nlines == 0)
	{
		free_string_array(lines, nlines);
		return 0;
	}

	*picked = find_source_item(lines[0]);
	free_string_array(lines, nlines);
	return 0;
}

/* Lets user pick a file under view's current directory through fzf. */
static int
pick_fzf_file(view_t *view, char **picked)
{
	FILE *output;
	char **lines;
	int nlines;

	char *const escaped_dir = shell_arg_escape(flist_get_dir(view),
			curr_stats.shell_type);
	if(escaped_dir == NULL)
	{
		ui_sb_err("Failed to prepare fzf command");
		return 1;
	}

	char *const cmd = format_str("find %s -type f -print0 | "
			"fzf --read0 --print0 --prompt=File: --layout=reverse --height=100%%",
			escaped_dir);
	free(escaped_dir);
	if(cmd == NULL)
	{
		ui_sb_err("Failed to prepare fzf command");
		return 1;
	}

	ui_shutdown();
	pid_t pid = bg_run_and_capture(cmd, /*user_sh=*/0, /*in=*/NULL, &output,
			/*err=*/NULL);
	free(cmd);
	if(pid == (pid_t)-1)
	{
		recover_after_shellout();
		update_screen(UT_FULL);
		return 1;
	}

	lines = read_stream_lines(output, &nlines, /*null_sep_heuristic=*/1,
			/*cb=*/NULL, /*arg=*/NULL);
	fclose(output);
	recover_after_shellout();
	update_screen(UT_FULL);

	if(nlines > 0)
	{
		*picked = strdup(lines[0]);
	}
	free_string_array(lines, nlines);

	return 0;
}

/* Navigates the view to a file picked by fzf. */
static void
goto_picked_file(view_t *view, const char path[])
{
	char full_path[PATH_MAX + 1];
	char *fname;

	to_canonic_path(path, flist_get_dir(view), full_path, sizeof(full_path));
	if(!path_exists(full_path, NODEREF))
	{
		ui_sb_errf("Path doesn't exist: %s", full_path);
		return;
	}

	fname = strdup(get_last_path_component(full_path));
	remove_last_path_component(full_path);
	navigate_to_file(view, full_path, fname, 1);
	free(fname);
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

/* Adds item to the source list.  Takes ownership of item and action on
 * success. */
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

/* Adds builtin commands to the source list. */
static int
append_builtin_commands(size_t cmdname_width)
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

/* Adds user-defined and foreign commands to the source list. */
static int
append_custom_commands(char *list[], size_t cmdname_width)
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

/* Adds a normal-mode key action to the source list. */
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

/* Finds source item by its text. */
static int
find_source_item(const char item[])
{
	int i;
	for(i = 0; i < source_len; ++i)
	{
		if(strcmp(source_items[i], item) == 0)
		{
			return i;
		}
	}
	return -1;
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

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
