/*
 *  (C) Copyright 2002-2003 Wojtek Kaniewski <wojtekka@irc.pl>
 *			    Wojtek Bojdo� <wojboj@htcon.pl>
 *			    Pawe� Maziarz <drg@infomex.pl>
 *		  2008-2010 Wies�aw Ochmi�ski <wiechu@wiechu.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Window header and statusbar routines
 */

#include "ekg2.h"

#include "ecurses.h"

#include <arpa/inet.h>
#include <string.h>

#include "input.h"
#include "nc-stuff.h"

/* vars */
int config_header_size;
int config_statusbar_size;

WINDOW *ncurses_status	= NULL;		/* okno stanu */
WINDOW *ncurses_header	= NULL;		/* okno nag��wka */

struct format_data {
	char *name;			/* %{nazwa} */
	char *text;			/* tre�� */
	int percent_ok;
};


/*
 * window_printat()
 *
 * wy�wietla dany tekst w danym miejscu okna.
 *	(w == ncurses_header || ncurses_status)
 *  - format - co mamy wy�wietli�
 *  - data - dane do podstawienia w formatach
 *  - fgcolor - domy�lny kolor tekstu
 *  - bold - domy�lne pogrubienie
 *  - bgcolor - domy�lny kolor t�a
 *
 * zwraca ilo�� dopisanych znak�w.
 */

//static void window_printat(WINDOW *w, const /*locale*/ char *format, /*locale*/ struct format_data *data, int fgcolor, int bold, int bgcolor) {
static void window_printat(WINDOW *w, const /*locale*/ char *format, /*locale*/ struct format_data *data, int attr) {
	const char *p;			/* temporary format value */
	int fgcolor = PAIR_NUMBER(attr) & 0x7;
	int bgcolor = (PAIR_NUMBER(attr) >> 3) & 0x7;
#define __fgcolor(x,y,z) \
		case x: fgcolor = z; attr &= ~A_BOLD; break; \
		case y: fgcolor = z; attr |= A_BOLD; break;
#define __bgcolor(x,y) \
		case x: bgcolor = y; break;

	void __set_attr(char control) {
		switch (control) {
			case '%': waddch(w, control); return;	/* it's character, not color attribute */
			__fgcolor('k', 'K', COLOR_BLACK);
			__fgcolor('r', 'R', COLOR_RED);
			__fgcolor('g', 'G', COLOR_GREEN);
			__fgcolor('y', 'Y', COLOR_YELLOW);
			__fgcolor('b', 'B', COLOR_BLUE);
			__fgcolor('m', 'M', COLOR_MAGENTA);
			__fgcolor('p', 'P', COLOR_MAGENTA);
			__fgcolor('c', 'C', COLOR_CYAN);
			__fgcolor('w', 'W', COLOR_WHITE);
			__bgcolor('l', COLOR_BLACK);
			__bgcolor('s', COLOR_RED);
			__bgcolor('h', COLOR_GREEN);
			__bgcolor('z', COLOR_YELLOW);
			__bgcolor('e', COLOR_BLUE);
			__bgcolor('q', COLOR_MAGENTA);
			__bgcolor('d', COLOR_CYAN);
			__bgcolor('x', COLOR_WHITE);
			case 'T':	attr ^= A_BOLD;		break;
			case 'U':	attr |= A_UNDERLINE;	break;
			case 'V':	attr |= A_REVERSE;	break;
			case 'i':	attr |= A_BLINK;	break;
			case 'A':	attr |= A_ALTCHARSET;	break;
			case 'a':	attr &= ~A_ALTCHARSET;	break;
			case 'n':
				bgcolor = COLOR_BLUE;
				fgcolor = COLOR_WHITE;
				attr &= A_COLOR;
				break;
		}
		attr = color_pair(fgcolor, bgcolor) | (attr & ~A_COLOR);
		wattrset(w, attr);
	}

#undef __fgcolor
#undef __bgcolor

	p = format;

	while (*p && *p != '}' && getcurx(w) <= w->_maxx) {
		int i, nest;

		if (*p != '%') {
			waddch(w, (unsigned char) *p++);
			continue;
		}
// %
		if (!*++p)
			break;

		if (*p != '{') {
			__set_attr(*p++);
			continue;
		}
// %{
		if (!*++p)
			break;

		for (i = 0; *p!='?' && data[i].name; i++) {
			int len;

			if (!data[i].text)
				continue;

			len = xstrlen(data[i].name);

			if (!strncmp(p, data[i].name, len) && p[len] == '}') {
				char *text = data[i].text;

				while (*text && getcurx(w) <= w->_maxx) {
					if (*text == '%' && data[i].percent_ok) {
						if (!*++text)
							break;

						__set_attr(*text++);
					} else {
						waddch(w, (unsigned char) *text++);
					}
				}

				p += len;
				goto next;
			}
		}

		if (*p == '?') {
// %{?
			int neg = 0;

			if (!*++p)
				break;

			if (*p == '!') {
				neg = 1;
				p++;
			}

			for (i = 0; data[i].name; i++) {
				int len, matched = ((data[i].text) ? 1 : 0);

				if (neg)
					matched = !matched;

				len = xstrlen(data[i].name);

				if (!strncmp(p, data[i].name, len) && p[len] == ' ') {
					p += len + 1;

					if (matched)
						window_printat(w, p, data, attr);
					break; /* goto next; */
				}
			}
			/* goto next; */
		}

next:
		/* uciekamy z naszego poziomu zagnie�d�enia */

		nest = 1;

		while (*p && nest) {
			if (*p == '}')
				nest--;
			if (*p == '{')
				nest++;
			p++;
		}
	}
}

static char *ncurses_window_activity(void) {
	string_t s = string_init("");
	int act = 0;
	window_t *w;

	for (w = windows; w; w = w->next) {
		char tmp[36];

		if ((!w->act && !w->in_typing) || !w->id || (w == window_current))
			continue;

		if (act)
			string_append_c(s, ',');

		switch (w->act) {
			case EKG_WINACT_NONE:
			case EKG_WINACT_JUNK:
				strcpy(tmp, "statusbar_act");
				break;
			case EKG_WINACT_MSG:
				strcpy(tmp, "statusbar_act_important");
				break;
			case EKG_WINACT_IMPORTANT:
			default:
				strcpy(tmp, "statusbar_act_important2us");
				break;
		}

		if (w->in_typing)
			strcat(tmp, "_typing");

		string_append(s, format_find(tmp));
		string_append(s, ekg_itoa(w->id));
		act = 1;
	}

	if (!act) {
		string_free(s, 1);
		return NULL;
	} else
		return string_free(s, 0);
}

static void reprint_statusbar(WINDOW *w, int y, const gchar *format, /*locale*/ struct format_data *data) {
	int backup_display_color = config_display_color;
	char *tmp;

	if (!w)
		return;

	if (config_display_color == 2)
		config_display_color = 0;

	wattrset(w, color_pair(COLOR_WHITE, COLOR_BLUE));

	wmove(w, y, 0);
	tmp = ekg_recode_to_locale(format);
	window_printat(w, tmp, data, color_pair(COLOR_WHITE, COLOR_BLUE));
	g_free(tmp);

	mvwhline(w, y, getcurx(w), ' ', w->_maxx);

	config_display_color = backup_display_color;
}

/*
 * update_statusbar()
 *
 * uaktualnia pasek stanu i wy�wietla go ponownie.
 *
 *  - commit - czy wy�wietli� od razu?
 */
void update_statusbar(int commit)
{
	static const char empty_format[] = "";
	static int connecting_counter = 0;

	struct format_data formats[32];	/* if someone add his own format increment it. */
	int formats_count = 0, i = 0, y;
	session_t *sess = window_current->session;
	userlist_t *q = userlist_find(sess, window_current->target);

	char *query_tmp;
	char *irctopic, *irctopicby, *ircmode;
	int mail_count;

	wattrset(ncurses_status, color_pair(COLOR_WHITE, COLOR_BLUE));
	if (ncurses_header)
		wattrset(ncurses_header, color_pair(COLOR_WHITE, COLOR_BLUE));

	/* inicjalizujemy wszystkie opisowe bzdurki */
#define __add_format_(x, z, p) \
	{ \
		formats[formats_count].name = x; \
		formats[formats_count].text = z; \
		formats[formats_count].percent_ok = p; \
		formats_count++; \
	}

#define __add_format(x, y, z) \
	{ \
		char *tmp = y; \
		__add_format_(x, ekg_recode_to_locale(tmp), z); \
		g_free(tmp); \
	}

#define __add_format_emp(x, y)		__add_format_(x, y ? (char*) empty_format : NULL, 0)
#define __add_format_dup(x, y, z)	__add_format_(x, y ? ekg_recode_to_locale(z) : NULL, 0)

	__add_format("time", xstrdup(timestamp(format_find("statusbar_timestamp"))), 1);

	__add_format_dup("window", window_current->id, ekg_itoa(window_current->id));
	__add_format_dup("session", (sess), (sess->alias) ? sess->alias : sess->uid);
	__add_format_dup("descr", (sess && sess->descr && sess->connected), sess->descr);

	query_tmp = (sess && q && q->nickname) ? saprintf("%s/%s", q->nickname, q->uid) : xstrdup(window_current->alias ? window_current->alias : window_current->target);
	__add_format("query", query_tmp, 0);
	__add_format("query_nickname", (sess && q && q->nickname) ? xstrdup(q->nickname) : xstrdup(window_current->alias ? window_current->alias : window_current->target), 0);
	if (window_current->more)
		__add_format("more", saprintf("more:%%T%d%%n", ncurses_current->more_lines), 1);

	__add_format_emp("debug", (!window_current->id));

	mail_count = -1;
	if (query_emit(NULL, "mail-count", &mail_count) != -2)
		__add_format_dup("mail", (mail_count > 0), ekg_itoa(mail_count));

	irctopic = irctopicby = ircmode = NULL;
	if (query_emit(NULL, "irc-topic", &irctopic, &irctopicby, &ircmode) != -2) {
		__add_format("irctopic", irctopic, 1);
		__add_format("irctopicby", irctopicby, 0);
		__add_format("ircmode", ircmode, 0);
	}

	__add_format("activity", ncurses_window_activity(), 1);

	if (sess && (sess->connected || (sess->connecting && connecting_counter))) {
#define __add_format_emp_st(x, y) case y: __add_format_(x, (char *) empty_format, 0) break
		switch (sess->status) {
				/* XXX: rewrite? */
			__add_format_emp_st("away", EKG_STATUS_AWAY);
			__add_format_emp_st("avail", EKG_STATUS_AVAIL);
			__add_format_emp_st("dnd", EKG_STATUS_DND);
			__add_format_emp_st("chat", EKG_STATUS_FFC);
			__add_format_emp_st("xa", EKG_STATUS_XA);
			__add_format_emp_st("gone", EKG_STATUS_GONE);
			__add_format_emp_st("invisible", EKG_STATUS_INVISIBLE);

			__add_format_emp_st("notavail", EKG_STATUS_NA);		/* XXX, session shouldn't be connected here */
			default: ;
		}
#undef __add_format_emp_st
	} else
		__add_format_emp("notavail", 1);

	if (sess && sess->connecting) /* statusbar update shall be called at least once per second */
		connecting_counter ^= 1;

	if (q) {
		int __ip = user_private_item_get_int(q, "ip");
		char *ip = __ip ? inet_ntoa(*((struct in_addr*) &__ip)) : NULL;;
#define __add_format_emp_st(x, y) case y: __add_format_("query_" x, (char *) empty_format, 0); break
		switch (q->status) {
				/* XXX: rewrite? */
			__add_format_emp_st("away", EKG_STATUS_AWAY);
			__add_format_emp_st("avail", EKG_STATUS_AVAIL);
			__add_format_emp_st("invisible", EKG_STATUS_INVISIBLE);
			__add_format_emp_st("notavail", EKG_STATUS_NA);
			__add_format_emp_st("dnd", EKG_STATUS_DND);
			__add_format_emp_st("chat", EKG_STATUS_FFC);
			__add_format_emp_st("xa", EKG_STATUS_XA);
			__add_format_emp_st("gone", EKG_STATUS_GONE);
			__add_format_emp_st("blocking", EKG_STATUS_BLOCKED);
			__add_format_emp_st("error", EKG_STATUS_ERROR);
			__add_format_emp_st("unknown", EKG_STATUS_UNKNOWN);
			default: ;
		}
#undef __add_format_emp_st

		__add_format_emp("typing", q->typing);

		__add_format_dup("query_descr", (q->descr1line), q->descr1line);

		__add_format_dup("query_ip", 1, ip);
	}

	__add_format_dup("url", 1, "http://www.ekg2.org/");
	__add_format_dup("version", 1, VERSION);

	__add_format(NULL, NULL, 0);	/* NULL-terminator */

#undef __add_format_emp
#undef __add_format_dup
#undef __add_format
#undef __add_format_

	for (y = 0; y < config_header_size; y++) {
		const char *p;

		if (!y) {
			p = format_find("header1");

			if (!format_ok(p))
				p = format_find("header");
		} else {
			char *tmp = saprintf("header%d", y + 1);
			p = format_find(tmp);
			xfree(tmp);
		}

		reprint_statusbar(ncurses_header, y, p, formats);
	}

	for (y = 0; y < config_statusbar_size; y++) {
		const char *p;

		if (!y) {
			p = format_find("statusbar1");

			if (!format_ok(p))
				p = format_find("statusbar");
		} else {
			char *tmp = saprintf("statusbar%d", y + 1);
			p = format_find(tmp);
			xfree(tmp);
		}

		switch (ncurses_debug) {
			char *tmp;
			case 0:
				reprint_statusbar(ncurses_status, y, p, formats);
				break;

			case 1:
/* XXX */
				tmp = saprintf(" debug: lines=%d start=(%d,%d) height=%d cleared=%d screen_width=%d",
					ncurses_current->backlog->len, ncurses_current->index, ncurses_current->first_row,
					window_current->height, ncurses_current->cleared, ncurses_screen_width);
				reprint_statusbar(ncurses_status, y, tmp, formats);
				xfree(tmp);
				break;

			case 2:
				tmp = saprintf(" debug: lines(count=%d,start=%d,index=%d), line(start=%d,index=%d)", ncurses_lines ? g_strv_length((char **) ncurses_lines) : 0, lines_start, lines_index, line_start, line_index);
				reprint_statusbar(ncurses_status, y, tmp, formats);
				xfree(tmp);
				break;

			case 3:
				tmp = saprintf(" debug: session=%p uid=%s alias=%s / target=%s session_current->uid=%s", sess, (sess && sess->uid) ? sess->uid : "", (sess && sess->alias) ? sess->alias : "", (window_current->target) ? window_current->target : "", (session_current && session_current->uid) ? session_current->uid : "");
				reprint_statusbar(ncurses_status, y, tmp, formats);
				xfree(tmp);
				break;
		}
	}

	for (i = 0; i < formats_count; i++) {
		if (formats[i].text != empty_format)
			xfree(formats[i].text);
	}

	if (commit)
		ncurses_commit();
}

/*
 * header_statusbar_resize()
 *
 * zmienia rozmiar paska stanu i/lub nag��wka okna.
 */
void header_statusbar_resize(const char *dummy)
{
/*	if (in_autoexec) return; */
	if (!ncurses_status)
		return;

	if (config_header_size < 0)
		config_header_size = 0;

	if (config_header_size > 5)
		config_header_size = 5;

	if (config_statusbar_size < 1)
		config_statusbar_size = 1;

	if (config_statusbar_size > 5)
		config_statusbar_size = 5;

	if (config_header_size) {
		if (!ncurses_header)
			ncurses_header = newwin(config_header_size, stdscr->_maxx + 1, 0, 0);
		else
			wresize(ncurses_header, config_header_size, stdscr->_maxx + 1);
	}

	if (!config_header_size && ncurses_header) {
		delwin(ncurses_header);
		ncurses_header = NULL;
	}

	ncurses_resize();

	wresize(ncurses_status, config_statusbar_size, stdscr->_maxx + 1);
	mvwin(ncurses_status, stdscr->_maxy + 1 - ncurses_input_size - config_statusbar_size, 0);

	update_statusbar(0);

	ncurses_commit();
}
