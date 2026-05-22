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

#include <ctype.h> /* isdigit() */
#include <stdint.h> /* intptr_t */
#include <stdio.h> /* FILE fclose() fprintf() */
#include <stdlib.h> /* free() */
#include <string.h> /* strcmp() strdup() strlen() */
#include <wchar.h> /* wchar_t */

#include "../compat/fs_limits.h"
#include "../compat/os.h"
#include "../compat/reallocarray.h"
#include "../cfg/config.h"
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
#include "menus.h"

/* Minimal length of command name column. */
#define CMDNAME_COLUMN_MIN_WIDTH 10
#define KIND_COLUMN_WIDTH 7
#define KEY_COLUMN_MIN_WIDTH 10

typedef enum
{
	PA_COMMAND = 1,
	PA_KEYS = 2,
	PA_HEADER = 4,
	PA_NEEDS_ARGS = 0x100,
}
PaletteAction;

static int execute_cmd_palette_action(view_t *view, const char action[],
		PaletteAction type);
static int build_cmd_palette_source(void);
static int write_fzf_input(FILE *fp);
static int pick_fzf_item(FILE *input, const char query[], int *picked);
static int pick_fzf_file(const char root[], const char query[], char **next_query,
		char **picked);
static int stash_find_fzf_results(view_t *view, const char root[],
		const char query[], const char selected[]);
static char * make_find_fzf_filter_cmd(const char root[], const char query[]);
static int execute_find_fzf_cb(view_t *view, menu_data_t *m);
static void goto_picked_file(view_t *view, const char path[]);
static int prepare_grep_results(view_t *view, const char pattern[],
		char results_path[], size_t results_path_len, char meta_path[],
		size_t meta_path_len);
static char * make_grep_cmd(view_t *view, const char pattern[]);
static int create_grep_opener(const char meta_path[], char script_path[],
		size_t script_path_len);
static int run_grep_fzf(const char results_path[], const char script_path[]);
static char * make_grep_fzf_cmd(const char results_path[],
		const char script_path[]);
static int extract_grep_match(const char line[], char path[], size_t path_size,
		int *line_num);
static int count_custom_commands(char *list[]);
static int append_source_item(char item[], char action[], PaletteAction type);
static int append_section_header(const char title[]);
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
show_cmd_palette_fzf(view_t *view, const char query[])
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
	const int err = pick_fzf_item(input, query != NULL ? query : "", &picked);
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
show_find_fzf(view_t *view, const char query[])
{
	char *next_query = NULL;
	char *picked = NULL;
	const char *const root = flist_get_dir(view);
	const char *const initial_query = (query != NULL) ? query : "";

	if(find_cmd_in_path("fzf", /*path_len=*/0UL, /*path=*/NULL) != 0)
	{
		ui_sb_err("fzf executable not found");
		return 1;
	}

	const int err = pick_fzf_file(root, initial_query, &next_query, &picked);
	if(err == 0)
	{
		(void)stash_find_fzf_results(view, root,
				next_query != NULL ? next_query : initial_query, picked);
	}
	if(err == 0 && picked != NULL)
	{
		goto_picked_file(view, picked);
	}

	free(next_query);
	free(picked);
	return err;
}

int
show_grep_fzf(view_t *view, const char pattern[])
{
	char results_path[PATH_MAX + 1];
	char meta_path[PATH_MAX + 1];
	char script_path[PATH_MAX + 1];

	if(find_cmd_in_path("fzf", /*path_len=*/0UL, /*path=*/NULL) != 0)
	{
		ui_sb_err("fzf executable not found");
		return 1;
	}
	if(find_cmd_in_path("rg", /*path_len=*/0UL, /*path=*/NULL) != 0 &&
			find_cmd_in_path("grep", /*path_len=*/0UL, /*path=*/NULL) != 0)
	{
		ui_sb_err("Neither rg nor grep executable found");
		return 1;
	}
	if(prepare_grep_results(view, pattern, results_path, sizeof(results_path),
				meta_path, sizeof(meta_path)) != 0)
	{
		return 1;
	}
	if(create_grep_opener(meta_path, script_path, sizeof(script_path)) != 0)
	{
		(void)remove(results_path);
		(void)remove(meta_path);
		return 1;
	}

	const int err = run_grep_fzf(results_path, script_path);
	(void)remove(results_path);
	(void)remove(meta_path);
	(void)remove(script_path);
	return err;
}

/* Executes a command palette action. */
static int
execute_cmd_palette_action(view_t *view, const char action[], PaletteAction type)
{
	if(type & PA_HEADER)
	{
		return 0;
	}

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

	const int err = (custom_count > 0 &&
	                  append_section_header("User-defined commands") != 0) ||
	                append_custom_commands(custom_cmds, cmdname_width) != 0 ||
	                append_section_header("Builtin commands") != 0 ||
	                append_builtin_commands(cmdname_width) != 0 ||
	                append_section_header("Normal mode keys") != 0;
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
pick_fzf_item(FILE *input, const char query[], int *picked)
{
	FILE *output;
	char **lines;
	int nlines;

	char *const escaped_query = shell_arg_escape(query, curr_stats.shell_type);
	if(escaped_query == NULL)
	{
		ui_sb_err("Failed to prepare fzf command");
		return 1;
	}

	char *const cmd = format_str("fzf --query=%s --prompt=Command: "
			"--layout=reverse --height=100%%", escaped_query);
	free(escaped_query);
	if(cmd == NULL)
	{
		ui_sb_err("Failed to prepare fzf command");
		return 1;
	}

	ui_shutdown();
	pid_t pid = bg_run_and_capture(cmd, /*user_sh=*/0, input, &output,
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
pick_fzf_file(const char root[], const char query[], char **next_query,
		char **picked)
{
	FILE *output;
	char **lines;
	int nlines;

	char *const escaped_dir = shell_arg_escape(root, curr_stats.shell_type);
	char *const escaped_query = shell_arg_escape(query, curr_stats.shell_type);
	if(escaped_dir == NULL || escaped_query == NULL)
	{
		free(escaped_dir);
		free(escaped_query);
		ui_sb_err("Failed to prepare fzf command");
		return 1;
	}

	char *const cmd = format_str("find %s -type f -print0 | "
			"fzf --read0 --print-query --query=%s "
			"--prompt=File: --layout=reverse --height=100%%",
			escaped_dir, escaped_query);
	free(escaped_dir);
	free(escaped_query);
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
		*next_query = strdup(lines[0]);
	}

	if(nlines > 1)
	{
		*picked = strdup(lines[1]);
	}
	free_string_array(lines, nlines);

	return 0;
}

/* Saves current fzf find results as a regular Vifm menu. */
static int
stash_find_fzf_results(view_t *view, const char root[], const char query[],
		const char selected[])
{
	FILE *output;
	char **lines;
	int nlines;
	int i;
	static menu_data_t m;

	char *const cmd = make_find_fzf_filter_cmd(root, query);
	if(cmd == NULL)
	{
		ui_sb_err("Failed to prepare fzf find results");
		return 1;
	}

	pid_t pid = bg_run_and_capture(cmd, /*user_sh=*/0, /*in=*/NULL, &output,
			/*err=*/NULL);
	free(cmd);
	if(pid == (pid_t)-1)
	{
		return 1;
	}

	lines = read_stream_lines(output, &nlines, /*null_sep_heuristic=*/1,
			/*cb=*/NULL, /*arg=*/NULL);
	fclose(output);

	menus_init_data(&m, view,
			format_str("FindFZF %s @ %s",
			           query, root),
			strdup("No files found"));
	m.stashable = 1;
	m.execute_handler = &execute_find_fzf_cb;
	m.key_handler = &menus_def_khandler;

	for(i = 0; i < nlines; ++i)
	{
		if(add_to_string_array(&m.items, m.len, lines[i]) != m.len)
		{
			if(selected != NULL && strcmp(lines[i], selected) == 0)
			{
				m.pos = m.len;
			}
			++m.len;
		}
	}

	free_string_array(lines, nlines);
	menus_stash(&m);
	return 0;
}

/* Makes a shell command that reproduces current fzf find result list. */
static char *
make_find_fzf_filter_cmd(const char root[], const char query[])
{
	char *const escaped_root = shell_arg_escape(root, curr_stats.shell_type);
	char *const escaped_query = shell_arg_escape(query, curr_stats.shell_type);
	char *cmd;

	if(escaped_root == NULL || escaped_query == NULL)
	{
		free(escaped_root);
		free(escaped_query);
		return NULL;
	}

	cmd = format_str("find %s -type f -print0 | fzf --read0 --filter=%s",
			escaped_root, escaped_query);

	free(escaped_root);
	free(escaped_query);
	return cmd;
}

/* Navigates to a file picked from the fzf find results menu. */
static int
execute_find_fzf_cb(view_t *view, menu_data_t *m)
{
	const char *const spec = m->get_spec(m, m->pos);
	(void)menus_goto_file(m, view, spec, /*try_open=*/0);
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

/* Captures grep results to a temporary file. */
static int
prepare_grep_results(view_t *view, const char pattern[], char results_path[],
		size_t results_path_len, char meta_path[], size_t meta_path_len)
{
	FILE *results;
	FILE *meta;
	FILE *output;
	char **lines;
	int nlines;
	int i;
	int id = 1;

	results = make_file_in_tmp("vifm-fzf-grep", 0600, /*auto_delete=*/0,
			results_path, results_path_len);
	if(results == NULL)
	{
		ui_sb_err("Failed to create temporary file for grep results");
		return 1;
	}
	meta = make_file_in_tmp("vifm-fzf-grep-meta", 0600, /*auto_delete=*/0,
			meta_path, meta_path_len);
	if(meta == NULL)
	{
		fclose(results);
		(void)remove(results_path);
		ui_sb_err("Failed to create temporary file for grep metadata");
		return 1;
	}

	char *const cmd = make_grep_cmd(view, pattern);
	if(cmd == NULL)
	{
		fclose(results);
		fclose(meta);
		(void)remove(results_path);
		(void)remove(meta_path);
		ui_sb_err("Failed to prepare grep command");
		return 1;
	}

	pid_t pid = bg_run_and_capture(cmd, /*user_sh=*/0, /*in=*/NULL, &output,
			/*err=*/NULL);
	free(cmd);
	if(pid == (pid_t)-1)
	{
		fclose(results);
		fclose(meta);
		(void)remove(results_path);
		(void)remove(meta_path);
		return 1;
	}

	lines = read_stream_lines(output, &nlines, /*null_sep_heuristic=*/1,
			/*cb=*/NULL, /*arg=*/NULL);
	fclose(output);
	for(i = 0; i < nlines; ++i)
	{
		char path[PATH_MAX + 1];
		int line_num;
		if(extract_grep_match(lines[i], path, sizeof(path), &line_num) == 0)
		{
			fprintf(results, "%d\t%s\n", id, lines[i]);
			fprintf(meta, "%d\t%s\n", line_num, path);
			++id;
		}
	}
	free_string_array(lines, nlines);
	fclose(results);
	fclose(meta);

	return 0;
}

/* Creates a helper script that opens grep result by id. */
static int
create_grep_opener(const char meta_path[], char script_path[],
		size_t script_path_len)
{
	FILE *script = make_file_in_tmp("vifm-fzf-grep-open", 0700,
			/*auto_delete=*/0, script_path, script_path_len);
	if(script == NULL)
	{
		ui_sb_err("Failed to create temporary opener for grep results");
		return 1;
	}

	char *const escaped_meta = shell_arg_escape(meta_path, curr_stats.shell_type);
	if(escaped_meta == NULL)
	{
		fclose(script);
		(void)remove(script_path);
		return 1;
	}

	int bg;
	const char *const vicmd = cfg_get_vicmd(&bg);
	fprintf(script,
			"#!/bin/sh\n"
			"id=$1\n"
			"entry=$(sed -n \"${id}p\" %s)\n"
			"line=${entry%%	*}\n"
			"path=${entry#*	}\n"
			"[ -n \"$path\" ] || exit 0\n"
			"exec < /dev/tty > /dev/tty 2>&1\n"
			"%s -f +\"$line\" \"$path\"\n",
			escaped_meta, vicmd);
	free(escaped_meta);
	fclose(script);

	return 0;
}

/* Makes a shell command that searches file contents. */
static char *
make_grep_cmd(view_t *view, const char pattern[])
{
	char *const escaped_pattern = shell_arg_escape(pattern, curr_stats.shell_type);
	char *const escaped_dir = shell_arg_escape(flist_get_dir(view),
			curr_stats.shell_type);

	if(escaped_pattern == NULL || escaped_dir == NULL)
	{
		free(escaped_pattern);
		free(escaped_dir);
		return NULL;
	}

	const int have_rg = find_cmd_in_path("rg", /*path_len=*/0UL,
			/*path=*/NULL) == 0;
	char *cmd;
	if(have_rg)
	{
		cmd = format_str("rg --line-number --column --with-filename "
				"--color=never --smart-case -- %s %s",
				escaped_pattern, escaped_dir);
	}
	else
	{
		cmd = format_str("grep -RInH -I -e %s %s", escaped_pattern, escaped_dir);
	}

	free(escaped_pattern);
	free(escaped_dir);
	return cmd;
}

/* Runs fzf with grep results. */
static int
run_grep_fzf(const char results_path[], const char script_path[])
{
	FILE *output;
	char **lines;
	int nlines;

	char *const cmd = make_grep_fzf_cmd(results_path, script_path);
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
	free_string_array(lines, nlines);
	recover_after_shellout();
	update_screen(UT_FULL);

	return 0;
}

/* Makes a shell command that lets user browse cached grep results. */
static char *
make_grep_fzf_cmd(const char results_path[], const char script_path[])
{
	char *const escaped_results = shell_arg_escape(results_path,
			curr_stats.shell_type);
	char *const escaped_script = shell_arg_escape(script_path, curr_stats.shell_type);
	char *cmd;

	if(escaped_results == NULL || escaped_script == NULL)
	{
		free(escaped_results);
		free(escaped_script);
		return NULL;
	}

	cmd = format_str("fzf --with-nth=2.. --delimiter='\t' --prompt=Grep: "
			"--layout=reverse --height=100%% "
			"--bind='enter:execute(%s {1})' < %s",
			escaped_script, escaped_results);

	free(escaped_results);
	free(escaped_script);
	return cmd;
}

/* Extracts existing path and line from a grep-like "path:line:..." result line. */
static int
extract_grep_match(const char line[], char path[], size_t path_size,
		int *line_num)
{
	const char *sep = line;

	while((sep = strchr(sep, ':')) != NULL)
	{
		int parsed_line = 0;
		const char *num = sep + 1;
		if(isdigit((unsigned char)*num))
		{
			while(isdigit((unsigned char)*num))
			{
				parsed_line = parsed_line*10 + (*num - '0');
				++num;
			}

			if(*num == ':')
			{
				const size_t len = sep - line;
				if(len < path_size)
				{
					snprintf(path, path_size, "%.*s", (int)len, line);
					if(path_exists(path, NODEREF))
					{
						*line_num = parsed_line;
						return 0;
					}
				}
			}
		}

		++sep;
	}

	return 1;
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

/* Adds visual separator to the source list. */
static int
append_section_header(const char title[])
{
	char *const item = format_str("-- %s --", title);
	char *const action = strdup("");
	return append_source_item(item, action, PA_HEADER);
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

		char *const item = format_str("%-*s :%-*s %s", KIND_COLUMN_WIDTH,
				"builtin", (int)cmdname_width, cmds_list[i].name,
				cmds_list[i].descr);
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
		char *const item = format_str("%-*s :%-*s %s", KIND_COLUMN_WIDTH,
				"usercmd", (int)cmdname_width, list[i], list[i + 1]);
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

	char *const item = format_str("%-*s %-*s %s", KIND_COLUMN_WIDTH, "key",
			KEY_COLUMN_MIN_WIDTH, keys, descr);
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
