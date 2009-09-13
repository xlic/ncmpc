/* ncmpc (Ncurses MPD Client)
 * (c) 2004-2009 The Music Player Daemon Project
 * Project homepage: http://musicpd.org

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "config.h"
#include "i18n.h"
#include "options.h"
#include "charset.h"
#include "mpdclient.h"
#include "command.h"
#include "screen.h"
#include "screen_utils.h"
#include "screen_browser.h"
#include "screen_play.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

static struct screen_browser browser;
static char *current_path;

static void
browse_paint(void);

static void
file_repaint(void)
{
	browse_paint();
	wrefresh(browser.lw->w);
}

static void
file_repaint_if_active(void)
{
	if (screen_is_visible(&screen_browse))
		file_repaint();
}

static void
file_reload(struct mpdclient *c)
{
	if (browser.filelist != NULL)
		filelist_free(browser.filelist);

	browser.filelist = mpdclient_filelist_get(c, current_path);
}

/* the db has changed -> update the filelist */
static void
file_changed_callback(mpdclient_t *c, G_GNUC_UNUSED int event,
		      G_GNUC_UNUSED gpointer data)
{
	file_reload(c);

#ifndef NCMPC_MINI
	sync_highlights(c, browser.filelist);
#endif
	list_window_check_selected(browser.lw, filelist_length(browser.filelist));

	file_repaint_if_active();
}

#ifndef NCMPC_MINI
/* the playlist has been updated -> fix highlights */
static void
playlist_changed_callback(mpdclient_t *c, int event, gpointer data)
{
	browser_playlist_changed(&browser, c, event, data);

	file_repaint_if_active();
}
#endif

static bool
file_change_directory(mpdclient_t *c, filelist_entry_t *entry,
		      const char *new_path)
{
	mpd_InfoEntity *entity = NULL;
	gchar *path = NULL;
	char *old_path;
	int idx;

	if( entry!=NULL )
		entity = entry->entity;
	else if( new_path==NULL )
		return false;

	if( entity==NULL ) {
		if( entry || 0==strcmp(new_path, "..") ) {
			/* return to parent */
			char *parent = g_path_get_dirname(current_path);
			if( strcmp(parent, ".") == 0 )
				parent[0] = '\0';
			path = g_strdup(parent);
			g_free(parent);
		} else {
			/* entry==NULL, then new_path ("" is root) */
			path = g_strdup(new_path);
		}
	} else if( entity->type==MPD_INFO_ENTITY_TYPE_DIRECTORY) {
		/* enter sub */
		mpd_Directory *dir = entity->info.directory;
		path = g_strdup(dir->path);
	} else
		return false;

	old_path = current_path;
	current_path = g_strdup(path);

	file_reload(c);

#ifndef NCMPC_MINI
	sync_highlights(c, browser.filelist);
#endif

	idx = old_path != NULL
		? filelist_find_directory(browser.filelist, old_path)
		: -1;
	g_free(old_path);

	list_window_reset(browser.lw);
	if (idx >= 0) {
		list_window_set_selected(browser.lw, idx);
		list_window_center(browser.lw,
				   filelist_length(browser.filelist), idx);
	}

	g_free(path);
	return true;
}

static bool
file_handle_enter(struct mpdclient *c)
{
	struct filelist_entry *entry = browser_get_selected_entry(&browser);
	struct mpd_InfoEntity *entity;

	if (entry == NULL)
		return false;

	entity = entry->entity;
	if (entity == NULL || entity->type == MPD_INFO_ENTITY_TYPE_DIRECTORY)
		return file_change_directory(c, entry, NULL);
	else
		return false;
}

static int
handle_save(mpdclient_t *c)
{
	filelist_entry_t *entry;
	char *defaultname = NULL;
	int ret;
	unsigned selected;

	if (browser.lw->selected >= filelist_length(browser.filelist))
		return -1;

	for(selected = browser.lw->selected_start; selected <= browser.lw->selected_end; ++selected)
	{
		entry = filelist_get(browser.filelist, selected);
		if( entry && entry->entity ) {
			mpd_InfoEntity *entity = entry->entity;
			if( entity->type==MPD_INFO_ENTITY_TYPE_PLAYLISTFILE ) {
				mpd_PlaylistFile *plf = entity->info.playlistFile;
				defaultname = plf->path;
			}
		}
	}

	if(defaultname)
		defaultname = utf8_to_locale(defaultname);
	ret = playlist_save(c, NULL, defaultname);
	g_free(defaultname);

	return ret;
}

static int
handle_delete(mpdclient_t *c)
{
	filelist_entry_t *entry;
	mpd_InfoEntity *entity;
	mpd_PlaylistFile *plf;
	char *str, *buf;
	int key;
	unsigned selected;

	for(selected = browser.lw->selected_start; selected <= browser.lw->selected_end; ++selected)
	{
		if (selected >= filelist_length(browser.filelist))
			return -1;

		entry = filelist_get(browser.filelist, selected);
		if( entry==NULL || entry->entity==NULL )
			continue;

		entity = entry->entity;

		if( entity->type!=MPD_INFO_ENTITY_TYPE_PLAYLISTFILE ) {
			/* translators: the "delete" command is only possible
			   for playlists; the user attempted to delete a song
			   or a directory or something else */
			screen_status_printf(_("Deleting this item is not possible"));
			screen_bell();
			continue;
		}

		plf = entity->info.playlistFile;
		str = utf8_to_locale(g_basename(plf->path));
		buf = g_strdup_printf(_("Delete playlist %s [%s/%s] ? "), str, YES, NO);
		g_free(str);
		key = tolower(screen_getch(screen.status_window.w, buf));
		g_free(buf);
		if( key != YES[0] ) {
			/* translators: a dialog was aborted by the user */
			screen_status_printf(_("Aborted"));
			return 0;
		}

		if( mpdclient_cmd_delete_playlist(c, plf->path) )
			continue;

		/* translators: MPD deleted the playlist, as requested by the
		   user */
		screen_status_printf(_("Playlist deleted"));
	}
	return 0;
}

static void
browse_init(WINDOW *w, int cols, int rows)
{
	current_path = g_strdup("");

	browser.lw = list_window_init(w, cols, rows);
}

static void
browse_resize(int cols, int rows)
{
	browser.lw->cols = cols;
	browser.lw->rows = rows;
}

static void
browse_exit(void)
{
	if (browser.filelist)
		filelist_free(browser.filelist);
	list_window_free(browser.lw);

	g_free(current_path);
}

static void
browse_open(G_GNUC_UNUSED mpdclient_t *c)
{
	if (browser.filelist == NULL) {
		browser.filelist = mpdclient_filelist_get(c, "");
#ifndef NCMPC_MINI
		mpdclient_install_playlist_callback(c, playlist_changed_callback);
#endif
		mpdclient_install_browse_callback(c, file_changed_callback);
	}
}

static const char *
browse_title(char *str, size_t size)
{
	const char *path = NULL, *prev = NULL, *slash = current_path;
	char *path_locale;

	/* determine the last 2 parts of the path */
	while ((slash = strchr(slash, '/')) != NULL) {
		path = prev;
		prev = ++slash;
	}

	if (path == NULL)
		/* fall back to full path */
		path = current_path;

	path_locale = utf8_to_locale(path);
	g_snprintf(str, size, "%s: %s",
		   /* translators: caption of the browser screen */
		   _("Browse"), path_locale);
	g_free(path_locale);
	return str;
}

static void
browse_paint(void)
{
	list_window_paint(browser.lw, browser_lw_callback, browser.filelist);
}

static bool
browse_cmd(mpdclient_t *c, command_t cmd)
{
	switch(cmd) {
	case CMD_PLAY:
		if (file_handle_enter(c)) {
			file_repaint();
			return true;
		}

		break;

	case CMD_GO_ROOT_DIRECTORY:
		file_change_directory(c, NULL, "");
		file_repaint();
		return true;
	case CMD_GO_PARENT_DIRECTORY:
		file_change_directory(c, NULL, "..");
		file_repaint();
		return true;

	case CMD_LOCATE:
		/* don't let browser_cmd() evaluate the locate command
		   - it's a no-op, and by the way, leads to a
		   segmentation fault in the current implementation */
		return false;

	case CMD_DELETE:
		handle_delete(c);
		file_repaint();
		break;
	case CMD_SAVE_PLAYLIST:
		handle_save(c);
		break;
	case CMD_SCREEN_UPDATE:
		file_reload(c);
#ifndef NCMPC_MINI
		sync_highlights(c, browser.filelist);
#endif
		list_window_check_selected(browser.lw,
					   filelist_length(browser.filelist));
		file_repaint();
		return false;

	case CMD_DB_UPDATE:
		if (c->status == NULL)
			return true;

		if (!c->status->updatingDb) {
			if (mpdclient_cmd_db_update(c, current_path) == 0) {
				if (strcmp(current_path, "") != 0) {
					char *path_locale =
						utf8_to_locale(current_path);
					screen_status_printf(_("Database update of %s started"),
							     path_locale);
					g_free(path_locale);
				} else
					screen_status_printf(_("Database update started"));
			}
		} else
			screen_status_printf(_("Database update running..."));
		return true;

	default:
		break;
	}

	if (browser_cmd(&browser, c, cmd)) {
		if (screen_is_visible(&screen_browse))
			file_repaint();
		return true;
	}

	return false;
}

const struct screen_functions screen_browse = {
	.init = browse_init,
	.exit = browse_exit,
	.open = browse_open,
	.resize = browse_resize,
	.paint = browse_paint,
	.cmd = browse_cmd,
	.get_title = browse_title,
};

bool
screen_file_goto_song(struct mpdclient *c, const struct mpd_song *song)
{
	const char *slash, *parent;
	char *allocated = NULL;
	bool ret;
	int i;

	assert(song != NULL);
	assert(song->file != NULL);

	if (strstr(song->file, "//") != NULL)
		/* an URL? */
		return false;

	/* determine the song's parent directory and go there */

	slash = strrchr(song->file, '/');
	if (slash != NULL)
		parent = allocated = g_strndup(song->file, slash - song->file);
	else
		parent = "";

	ret = file_change_directory(c, NULL, parent);
	g_free(allocated);
	if (!ret)
		return false;

	/* select the specified song */

	i = filelist_find_song(browser.filelist, song);
	if (i < 0)
		i = 0;

	list_window_set_selected(browser.lw, i);

	/* finally, switch to the file screen */
	screen_switch(&screen_browse, c);
	return true;
}
