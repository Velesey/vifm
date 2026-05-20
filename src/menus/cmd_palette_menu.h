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

#ifndef VIFM__MENUS__CMD_PALETTE_MENU_H__
#define VIFM__MENUS__CMD_PALETTE_MENU_H__

struct view_t;

/* Shows command palette through fzf.  Returns non-zero on error. */
int show_cmd_palette_fzf(struct view_t *view);

/* Shows files under current directory through fzf and navigates to selected
 * file.  Returns non-zero on error. */
int show_find_fzf(struct view_t *view);

#endif /* VIFM__MENUS__CMD_PALETTE_MENU_H__ */

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
