/* ncmpc (Ncurses MPD Client)
 * (c) 2004-2018 The Music Player Daemon Project
 * Project homepage: http://musicpd.org
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "AlbumListPage.hxx"
#include "screen_status.hxx"
#include "screen_find.hxx"
#include "FileListPage.hxx"
#include "Command.hxx"
#include "screen.hxx"
#include "i18n.h"
#include "charset.hxx"
#include "mpdclient.hxx"
#include "util/StringUTF8.hxx"

#include <glib.h>

#include <algorithm>

#include <assert.h>
#include <string.h>

#define BUFSIZE 1024

gcc_pure
static bool
CompareUTF8(const std::string &a, const std::string &b)
{
	return CollateUTF8(a.c_str(), b.c_str()) < 0;
}

const char *
AlbumListPage::GetListItemText(char *buffer, size_t size,
			       unsigned idx) const
{
	if (idx == 0)
		return "..";
	else if (idx == album_list.size() + 1)
		return _("All tracks");

	--idx;

	assert(idx < album_list.size());

	const char *str_utf8 = album_list[idx].c_str();

	g_strlcpy(buffer, Utf8ToLocale(str_utf8).c_str(), size);
	return buffer;
}

static void
recv_tag_values(struct mpd_connection *connection, enum mpd_tag_type tag,
		std::vector<std::string> &list)
{
	struct mpd_pair *pair;

	while ((pair = mpd_recv_pair_tag(connection, tag)) != nullptr) {
		list.emplace_back(pair->value);
		mpd_return_pair(connection, pair);
	}
}

void
AlbumListPage::LoadAlbumList(struct mpdclient &c)
{
	auto *connection = c.GetConnection();

	album_list.clear();

	if (connection != nullptr) {
		mpd_search_db_tags(connection, MPD_TAG_ALBUM);
		mpd_search_add_tag_constraint(connection,
					      MPD_OPERATOR_DEFAULT,
					      MPD_TAG_ARTIST,
					      artist.c_str());
		mpd_search_commit(connection);

		recv_tag_values(connection, MPD_TAG_ALBUM, album_list);

		c.FinishCommand();
	}

	/* sort list */
	std::sort(album_list.begin(), album_list.end(), CompareUTF8);
	lw.SetLength(album_list.size() + 2);
}

void
AlbumListPage::Reload(struct mpdclient &c)
{
	LoadAlbumList(c);
}

/**
 * Paint one item in the album list.  There are two virtual items
 * inserted: at the beginning, there's the special item ".." to go to
 * the parent directory, and at the end, there's the item "All tracks"
 * to view the tracks of all albums.
 */
void
AlbumListPage::PaintListItem(WINDOW *w, unsigned i,
			     gcc_unused unsigned y, unsigned width,
			     bool selected) const
{
	const char *p;
	char *q = nullptr;

	if (i == 0)
		p = "..";
	else if (i == album_list.size() + 1)
		p = _("All tracks");
	else
		p = q = utf8_to_locale(album_list[i - 1].c_str());

	screen_browser_paint_directory(w, width, selected, p);
	g_free(q);
}

void
AlbumListPage::Paint() const
{
	lw.Paint(*this);
}

const char *
AlbumListPage::GetTitle(char *str, size_t size) const
{
	if (artist.empty())
		return _("Albums");

	snprintf(str, size, _("Albums of artist: %s"),
		 Utf8ToLocale(artist.c_str()).c_str());
	return str;
}

void
AlbumListPage::Update(struct mpdclient &c, unsigned events)
{
	if (events & MPD_IDLE_DATABASE) {
		/* the db has changed -> update the list */
		Reload(c);
		SetDirty();
	}
}

/* add_query - Add all songs satisfying specified criteria.
   _artist is actually only used in the ALBUM case to distinguish albums with
   the same name from different artists. */
static void
add_query(struct mpdclient *c, enum mpd_tag_type table, const char *_filter,
	  const char *_artist)
{
	assert(_filter != nullptr);

	auto *connection = c->GetConnection();
	if (connection == nullptr)
		return;

	screen_status_printf(_("Adding \'%s\' to queue"),
			     Utf8ToLocale(_filter).c_str());

	mpd_search_add_db_songs(connection, true);
	mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
				      table, _filter);
	if (table == MPD_TAG_ALBUM)
		mpd_search_add_tag_constraint(connection, MPD_OPERATOR_DEFAULT,
					      MPD_TAG_ARTIST, _artist);
	mpd_search_commit(connection);
	c->FinishCommand();
}

bool
AlbumListPage::OnCommand(struct mpdclient &c, Command cmd)
{
	switch(cmd) {
		const char *selected;

	case Command::PLAY:
		if (lw.selected == 0) {
			/* handle ".." */
			screen.OnCommand(c, Command::GO_PARENT_DIRECTORY);
			return true;
		}

		break;

	case Command::SELECT:
	case Command::ADD:
		for (const unsigned i : lw.GetRange()) {
			if(i == album_list.size() + 1)
				add_query(&c, MPD_TAG_ARTIST, artist.c_str(),
					  nullptr);
			else if (i > 0) {
				selected = album_list[lw.selected - 1].c_str();
				add_query(&c, MPD_TAG_ALBUM, selected,
					  artist.c_str());
				cmd = Command::LIST_NEXT; /* continue and select next item... */
			}
		}
		break;

		/* continue and update... */
	case Command::SCREEN_UPDATE:
		Reload(c);
		return false;

	case Command::LIST_FIND:
	case Command::LIST_RFIND:
	case Command::LIST_FIND_NEXT:
	case Command::LIST_RFIND_NEXT:
		screen_find(screen, &lw, cmd, *this);
		SetDirty();
		return true;

	case Command::LIST_JUMP:
		screen_jump(screen, &lw, *this, *this);
		SetDirty();
		return true;

	default:
		break;
	}

	if (lw.HandleCommand(cmd)) {
		SetDirty();
		return true;
	}

	return false;
}
