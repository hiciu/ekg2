/*
 *  (C) Copyright 2004-2005 Michal 'GiM' Spadlinski <gim at skrzynka dot pl>
 *			Wies�aw Ochmi�ski <wiechu@wiechu.com>
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
#include "ekg2.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#ifndef NO_POSIX_SYSTEM
#include <arpa/inet.h>
#endif

#define GDEBUG
#define MARLENE

#include "irc.h"
#include "misc.h"
#include "people.h"
#include "input.h"
#include "autoacts.h"

char *sopt_keys[SERVOPTS] = { NULL, NULL, "PREFIX", "CHANTYPES", "CHANMODES", "MODES", "CHANLIMIT", "NICKLEN", "IDCHAN" };
char sopt_casemapping[] = "CASEMAPPING";
char *sopt_casemapping_values[IRC_CASEMAPPING_COUNT] = { "ascii", "rfc1459", "strict-rfc1459" };

#define OMITCOLON(x) ((*x)==':'?(x+1):(x))

static void irc_parse_ident_host(char *identhost, char **ident, char **host)  {
	char	*tmp;

	xfree(*ident);
	xfree(*host);
	if ((tmp = xstrchr(identhost, '@'))) {
		*ident = xstrndup(identhost, tmp-identhost);
		*host  = xstrdup(tmp+1);
	} else {
		*ident = xstrdup(identhost);
		*host = NULL;
	}
}

static void irc_convert_in(irc_private_t *j, GString *line) {
	gchar **ep;

	if (j->auto_guess_encoding) {
		for (ep = j->auto_guess_encoding; *ep; ep++) {
			if (ekg_try_recode_gstring_from(*ep, line))
				return;
		}
	}

	if (j->conv)
		ekg_recode_gstring_from(j->conv, line);
	else
		ekg_fix_utf8(line->str);
}

/* cos co blabla, zwraca liczbe pochlonietych znakow przez '*' XXX */
static int do_sample_wildcard_match(const char *str, const char *matchstr, const char stopon) {
	int i;

	for (i = 0; (str[i] && str[i] != stopon); i++);

	if (i == 0)
		return xstrlen(matchstr);

	debug_error("\nXXX do_sample_wildcard_match() XXX\n");
	return 0;
}

static void irc_access_parse(session_t *s, channel_t *chan, people_t *p, int flags) {
	userlist_t *ul;

	if (!s || !chan || !p)
		return;

#define dchar(x) debug("%c", x);

	for (ul = s->userlist; ul; ul = ul->next) {
		userlist_t *u = ul;
		ekg_resource_t *r = NULL, *rl;

		int i, j;

		if (!p->ident || !p->host) continue;

		if (xstrncmp(u->uid, "irc:", 4)) continue;	/* check for irc: */

		for (rl = u->resources; rl; rl = rl->next) {
			r = rl;

			if (r->priv_data == p) {
				const char *tmp = &(u->uid[4]);

				/* fast forward move.. */
				if (!(tmp = xstrchr(tmp, '!')) || !(tmp = xstrchr(tmp, '@')) || !(tmp = xstrchr(tmp, ':'))) {
					debug_error("%s:%d INTERNAL ERROR\n", __FILE__, __LINE__);
					goto next3;
				}
				tmp++;
				i = (tmp - u->uid);
				debug("irc, checkchan: %s\n", tmp);
				goto skip_identhost_check;
				break;	/* never here */
			}
		}
		r = NULL;
/* parse nick! */
		for (i = 4, j = 0; (u->uid[i] != '\0' && u->uid[i] != '!'); i++, j++) {
			dchar(u->uid[i]);

			if (u->uid[i] != p->ident[j]) {
				if (u->uid[i] == '*') j += do_sample_wildcard_match(&u->uid[i+1], &p->ident[j], '!');
				else if (u->uid[i] == '?') continue;
				else goto next;
			}
		} if (!u->uid[i]) goto next;
		dchar('!');
		i++;

/* parse ident@ */
		for (j = 0; (u->uid[i] != '\0' && u->uid[i] != '@'); i++, j++) {
			dchar(u->uid[i]);

			if (u->uid[i] != p->nick[j]) {
				if (u->uid[i] == '*') j += do_sample_wildcard_match(&u->uid[i+1], &p->ident[j], '@');
				else if (u->uid[i] == '?') continue;
				else goto next;
			}
		} if (!u->uid[i]) goto next;
		dchar('@');
		i++;

/* parse host: */
		for (j = 0; (u->uid[i] != '\0' && u->uid[i] != ':'); i++, j++) {
			dchar(u->uid[i]);

			if (u->uid[i] != p->host[j]) {
				if (u->uid[i] == '*') j += do_sample_wildcard_match(&u->uid[i+1], &p->ident[j], ':');
				else if (u->uid[i] == '?') continue;
				else goto next;
			}
		} if (!u->uid[i]) goto next;
		dchar('\n');
		i++;

		debug_error("irc_access_parse() %s!%s@%s MATCH with %s\n", p->ident, p->nick, p->host, u->uid+4);

skip_identhost_check:
/* let's rock with channels */
		{
			char **arr = array_make(&u->uid[i], ",", 0, 1, 0);

			int ismatch = 0;


			for (i=0; arr[i]; i++) {
				int k;
				debug_error("CHAN%d: %s: ", i, arr[i]);

				for (j = 0, k = 4 /* skip irc: */; arr[i][j]; j++, k++) {
					if (arr[i][j] != chan->name[k]) {
						if (arr[i][j] == '*') k += do_sample_wildcard_match(&arr[i][j+1], &chan->name[k], '\0');
						else if (arr[i][j] == '?') continue;
						else goto next2;
					}
				}
				if (chan->name[k] != '\0')
					goto next2;

				ismatch = 1;
				debug_error("MATCH\n");
				break;
next2:
				debug_error("NOT MATCH\n");
				continue;

			}
			g_strfreev(arr);

			if (!ismatch) continue;
		}
		if (!r) {
			char *tmp = irc_uid(p->nick);
			r = userlist_resource_add(u, tmp, 0);
			g_free(tmp);

			r->status	= EKG_STATUS_AVAIL;
			r->descr	= xstrdup(chan->name+4);
			r->priv_data	= p;

			if (u->status != EKG_STATUS_AVAIL) {
				xfree(u->descr);
				u->status	= EKG_STATUS_AVAIL;
				u->descr	= xstrdup("description... ?");
				query_emit(NULL, "userlist-changed", &(s->uid), &(u->uid));
			}
		} else {
			string_t str = string_init(r->descr);

			string_append_c(str, ',');
			string_append(str, chan->name+4);

			xfree(r->descr); r->descr = string_free(str, 0);
		}

		/* tutaj ladnie by wygladalo jakbysmy wywolali protocol-status.. ale jednak to jest b. kiepski pomysl */
		debug_error("USER: 0x%x PERMISION GRANTED ON CHAN: 0x%x\n", u, chan);
		continue;

next:
		dchar('\n');
		debug_error("irc_access_parse() %s!%s@%s NOT MATCH with %s\n", p->ident, p->nick, p->host, u->uid+4);
next3:
		continue;
	}
#undef dchar
}

/**
 * int gatoi(char *buf, int *returnvalue)
 *
 * Simple wrapper around strtol.
 * Convert string pointed by buf to base 10 number, and save it in int value pointed at
 * returnvalue.
 *
 * @return	0 - OK
 *		1 - conversion failed, memory content of *number is unknown
 */
static int gatoi(char *buf, int *a) {
	char	*x;
	long	t;
	if(!buf) return (1);
	t=strtol(buf, &x, 10);
	if (x == buf) return (1);
	*a=t;
	return (0);
}

/**
 * irc_tolower_int(char *buf, int casemapping)
 *
 * Converts buffer pointed at buf to lower case using one of casmapping's:
 * IRC_CASEMAPPING_ASCII, IRC_CASEMAPPING_RFC1459, IRC_CASEMAPPING_RFC1459_STRICT
 *
 * DO NOT pass strings that can be in unicode;
 *
 * @return	pointer to beginning of a string
 */

static char *irc_tolower_int(char *buf, int casemapping)
{
	char *p = buf;
	int upper_bound;
	/* please, do not change this code, to something like:
	 * 90 + (!!casemapping * (5 - casemapping))
	 */
	switch (casemapping)
	{
		case IRC_CASEMAPPING_ASCII:		upper_bound = 'Z'; break;
		case IRC_CASEMAPPING_RFC1459_STRICT:	upper_bound = ']'; break;
		case IRC_CASEMAPPING_RFC1459:		upper_bound = '^'; break;
		default: debug_error ("bad value in call to irc_tolower_int: %d\n", casemapping); return 0;
	}
	while (*p)
	{
		/* 65 ascii for 'A' */
		if (*p >= 'A' && *p <= upper_bound)
			*p += 32; /* add 32, conversion 'A' -> 'a' */
		p++;
	}
	return buf;
}

/**
 * IRC_TO_LOWER - macro around irc_upper_int, that passes currently
 * casemapping used by server
 */
#define IRC_TO_LOWER(x) irc_tolower_int(x, j->casemapping)

/*****************************************************************************/
/*
 */

int irc_parse_line(session_t *s, const char *l)
{
	static GString *strbuf = NULL;
	irc_private_t *j = s->priv;
	int	i, c=0, ecode, n_params;
	char	*p, *cmd, *colon2, **args = NULL, **pfxcmd = NULL;

	gchar *buf;
	int	len;

	if (G_UNLIKELY(!strbuf))
		strbuf = g_string_new(l);
	else
		g_string_assign(strbuf, l);

	irc_convert_in(j, strbuf);
	buf = strbuf->str;
	len = strbuf->len;

	query_emit(NULL, "irc-parse-line", &s->uid, &buf);

	p=buf;
	if(!p)
		return -1;
/*
Each IRC message may consist of up to three main parts: the prefix
(optional), the command, and the command parameters (of which there
may be up to 15).  The prefix, command, and all parameters are
separated by one (or more) ASCII space character(s) (0x20).

The presence of a prefix is indicated with a single leading ASCII
colon character (':', 0x3b), which must be the first character of the
message itself.  There must be no gap (whitespace) between the colon
and the prefix.
*/
	/* GiM: In fact this is probably not needed, but just in case...  */
	for (i=0; i<len; i++) if (buf[i]=='\n' || buf[i]=='\r') buf[i]='\0';

	if ((colon2=xstrstr(p, " :")))
		*colon2 = '\0';

	args = array_make(OMITCOLON(p), " ", 0, 1, 0);

	if (colon2) {
		*colon2 = ' ';
		array_add(&args, xstrdup(colon2+2));
	}

#define prefix pfxcmd[0]
#define pfx_nick pfxcmd[1]
#define pfx_ihost pfxcmd[2]
#define cmdname pfxcmd[3]

	/* prefix is optional... */
	if (':' != *buf) {
		array_add(&pfxcmd, g_strdup(""));	// prefix
		array_add(&pfxcmd, g_strdup(""));	// pfx_nick
		array_add(&pfxcmd, g_strdup(""));	// pfx_ihost
	} else {
		array_add(&pfxcmd, array_shift(&args));						// prefix
		p = xstrchr(pfxcmd[0], '!');
		array_add(&pfxcmd, p ? g_strndup(pfxcmd[0], p-pfxcmd[0]) : g_strdup(""));	// pfx_nick
		array_add(&pfxcmd, p ? g_strdup(p+1) : g_strdup(""));				// pfx_ihost
	}

	cmd = array_shift(&args);
	array_add(&pfxcmd, cmd);		// cmdname

	/* debug only nasty hack ;> */
#ifdef GDEBUG
	/* mg: well, it's not the exact data sent, but color is needed indeed */
	i=0;
	if (*pfxcmd[0]) debug_iorecv("[%s]", pfxcmd[0]);
	debug_iorecv("[%s]", cmd);
	while (args[i] != NULL) debug_iorecv("[%s]",args[i++]);
	debug_iorecv("\n");
#endif

	n_params = g_strv_length(args);
	if (xstrlen(cmd) > 1) {
		if(!gatoi(cmd, &ecode)) {
			/* for scripts */
			char *emitname = saprintf("irc-protocol-numeric %s", cmd);
			if ((query_emit(NULL, "irc-protocol-numeric", &s->uid, &ecode, &args) == -1) ||
			    (query_emit(NULL, emitname, &s->uid, &args) == -1))
			{
				xfree(emitname);
				g_strfreev(pfxcmd);
				g_strfreev(args);
				return -1;
			}
			xfree(emitname);

			c=0;
			while(irccommands[c].type != -1) {
				if (irccommands[c].type == 1 && irccommands[c].num == ecode) {
					if (irccommands[c].min_params > n_params) {
						debug_error("[irc] parse_line() Not enough parameters! cmd=%s, n=%d, min=%d\n",
								cmd, n_params, irccommands[c].min_params);
					} else
					/* I'm sending c not ecode!!!! */
					if ((*(irccommands[c].handler))(s, j, c, pfxcmd, args) == -1 ) {
						debug_error("[irc] parse_line() error while executing handler!\n");
					}
					/* GiM: XXX I don't expect more,
					 * then one handler on list... */
					break;
				}
				c++;
			}
#ifdef GDEBUG
			if (irccommands[c].type == -1) {
				debug("trying default handler\n");
				if ((*(irccommands[0].handler))(s, j, 0, pfxcmd, args) == -1 ) {
					debug("[irc] parse_line() error while executing handler!\n");
				}

			}
#endif
		} else {
			c=0;
			while(irccommands[c].type != -1) {
				if (irccommands[c].type == 0 && !xstrcmp(irccommands[c].comm, cmd)) {
					if (irccommands[c].min_params > n_params) {
						debug_error("[irc] parse_line() Not enough parameters! cmd=%s, n=%d, min=%d\n",
								cmd, n_params, irccommands[c].min_params);
					} else
					/* dj: instead of  ecode,    c; */
					if ((*(irccommands[c].handler))(s, j, c, pfxcmd, args) == -1 ) {
						debug_error("[irc] parse_line() error while executing handler!\n");
					}
					break;
				}
				c++;
			}
		}
	}

	g_strfreev(pfxcmd);
	g_strfreev(args);
	return 0;
}

IRC_COMMAND(irc_c_init)
{
	int		i, k;
	char		*t;
	switch (irccommands[ecode].num)
	{
		case 1:
			protocol_connected_emit(s);

			t = xstrchr(args[1], '!');
			xfree(j->host_ident);
			j->host_ident = t ? xstrdup(t+1) : NULL;
			debug_ok("\nirc_c_init()/num=1/...[%s:%s] given: %s\n", j->nick, j->host_ident, args[0]);

			xfree(j->nick);
			j->nick = xstrdup(args[0]);

			j->autoreconnecting = 0;

			j->casemapping = IRC_CASEMAPPING_RFC1459;
			xfree(SOP(_005_PREFIX));
			SOP(_005_PREFIX) = xstrdup("(ov)@+");
			j->nick_signs = SOP(_005_PREFIX) + 4;
			xfree(j->nick_modes);
			j->nick_modes = xstrdup("ov");

			xfree(SOP(_005_CHANTYPES)); SOP(_005_CHANTYPES) = xstrdup("#!");
			xfree(SOP(_005_MODES)); SOP(_005_MODES) = xstrdup("3");
			xfree(SOP(_005_NICKLEN)); SOP(_005_NICKLEN) = xstrdup("9");
			/* ~~ kinda optimal: */
			xfree(SOP(_005_CHANMODES)); SOP(_005_CHANMODES) = xstrdup("b,k,l,imnpsta");
			/* http://www.irc.org/tech_docs/005.html
			CHANMODES= b,  k,l, imnpstr (ircu)
			CHANMODES= b,  k,l, iLmMnOprRst (Bahamut)
			CHANMODES= beI,k,l, imnpstaqr (IRCNet)
			CHANMODES= beI,k,l, imnpsta (Hybrid)
			*/
			break;
		case 4:
			xfree(SOP(USERMODES)); SOP(USERMODES) = xstrdup(args[3]);
			xfree(SOP(CHANMODES)); SOP(CHANMODES) = xstrdup(args[4]);
			break;
		case 5:
			for (i = 1; args[i]; i++) {
				for (k = 0; k < SERVOPTS; k++) {
					if (sopt_keys[k] == NULL)
						continue;
					if (xstrncmp(args[i], sopt_keys[k], xstrlen(sopt_keys[k])) || args[i][xstrlen(sopt_keys[k])] != '=' )
						continue;
					xfree(SOP(k));
					SOP(k) = xstrdup(xstrchr(args[i], '=')+1);
					if (xstrlen(SOP(k))==0) {
						xfree(SOP(k));
						SOP(k) = NULL;
					}
				}

				if (!xstrncmp(args[i], sopt_casemapping, xstrlen(sopt_casemapping)))
					if ((t = xstrchr(args[i], '='))) {
						/* I know this could be 'for', but I'm leavin' it for readability */
						if (!xstrcmp(t+1, sopt_casemapping_values[IRC_CASEMAPPING_ASCII])) {
							j->casemapping = IRC_CASEMAPPING_ASCII;
						} else if (!xstrcmp(t+1, sopt_casemapping_values[IRC_CASEMAPPING_RFC1459])) {
							j->casemapping = IRC_CASEMAPPING_RFC1459;
						} else if (!xstrcmp(t+1, sopt_casemapping_values[IRC_CASEMAPPING_RFC1459_STRICT])) {
							j->casemapping = IRC_CASEMAPPING_RFC1459_STRICT;
						/* this one is already set above, as default
						} else {
							j->casemapping = IRC_CASEMAPPING_RFC1459;
						*/
						}
					}
			}

			k = (xstrlen(SOP(_005_PREFIX))>>1) - 1;
			j->nick_signs = SOP(_005_PREFIX) + k + 2;
			xfree(j->nick_modes);
			j->nick_modes = xstrndup(SOP(_005_PREFIX)+1, k);

			irc_autorejoin(s, IRC_REJOIN_CONNECT, NULL);

			break;
		default:
			break;
	}
	return 0;
}

IRC_COMMAND(irc_c_error)
{
	int		future, i, n_params = g_strv_length(args);
	char		*t = NULL, *dest = NULL, *coloured = NULL, *bang = NULL;
	time_t		try;
	window_t	*w;
	char		*altnick;
	channel_t	*chanp;
	char 		*tmpchn = NULL;

#define IOK(x) x<g_strv_length(param)?param[x]:""
#define NOK(x) x<g_strv_length(args)?args[x]:""

	if (irccommands[ecode].future == 0 && !xstrcmp("ERROR", cmdname)) {
		/* here error @ CONNECT
		 *   21:03:35 [:_empty_][ERROR][:Trying to reconnect too fast.]
		 * no I:line's etc.. everything that disconnects fd
		 */
/*		print_info(NULL, s, "IRC_ERR_FIRSTSECOND", session_name(s), cmdname, NOK(0)); */
		if (s->connecting)
			irc_handle_disconnect(s, NOK(0), EKG_DISCONNECT_FAILURE);
		else	debug_error("[irc] !s->connecting\n");
		return -1;
	}

	future = irccommands[ecode].future&0xff;
	i = irccommands[ecode].future&0x100;
	if ((n_params <= 2) && ((IRC_ERR_NEW == future) || (IRC_ERR_21 == future) || (IRC_ERR_12 == future)))
		future = IRC_ERR_ONLY1;

	switch (future)
	{
		case IRC_ERR_21:
			print_info(NULL, s,
					i?"IRC_RPL_SECONDFIRST":"IRC_ERR_SECONDFIRST",
					session_name(s), args[1], args[2]);
			return (0);
		case IRC_ERR_12:
			print_info(NULL, s,
					i?"IRC_RPL_FIRSTSECOND":"IRC_ERR_FIRSTSECOND",
					session_name(s), args[1], args[2]);
			return (0);
		case IRC_ERR_ONLY1:
			print_info(NULL, s,
					i?"IRC_RPL_JUSTONE":"IRC_ERR_JUSTONE",
					session_name(s), NOK(1));
			return (0);
		case IRC_ERR_NEW:
			print_info(NULL, s,
					i?"IRC_RPL_NEWONE":"IRC_ERR_NEWONE",
					session_name(s), cmdname, args[1], args[2], NOK(3), NOK(4));
			return (0);
		case IRC_ERR_IGNO:
			return(0);
		default:
			break;
	}
	i = irccommands[ecode].num;
	/* GiM : XXX FIX IT
	 *
	 */
	if (args[1]) {
		t = irc_uid(args[1]);
		w = window_find_s(s, t);
		if (!w) {
			char *temp2;
			temp2 = irc_uid(args[1]);
			IRC_TO_LOWER(temp2);
			w = window_find_s(s, temp2);
			xfree(temp2);
		}
		dest = w?t:NULL;
	}
	switch (i) {
		case 433:
			print_info(NULL, s, "IRC_ERR_SECONDFIRST",
					session_name(s), args[1], NOK(2));
			if (s->connecting) {
				altnick = (char *) session_get(s, "alt_nick");
				/* G->dj: why this !xstrcmp ? */
				if (altnick && !xstrcmp(args[1], session_get(s, "nickname")) && xstrcmp(args[1], altnick)) {
					print_info(NULL, s, "IRC_TRYNICK",
							session_name(s), altnick);
					xfree(j->nick);
					j->nick = xstrdup(altnick);
					irc_write(s, "NICK %s\r\n", j->nick);
				}
			}
			break;
		case 404:
			tmpchn = clean_channel_names(s, args[1]);
			print_info(dest, s, "IRC_RPL_CANTSEND", session_name(s), tmpchn);
			break;
		case 301:
			if (!session_int_get(s, "DISPLAY_AWAY_NOTIFICATION"))
				break;
			dest = t;
			// NO BREAK!;
		/* topic */
		case 331:	// RPL_NOTOPIC		[mynick] [channel] [:No topic is set]
		case 332:	// RPL_TOPIC		[mynick] [channel] [:topic of channel]
			IRC_TO_LOWER(args[1]);
			if ((chanp = irc_find_channel(j->channels, args[1]))) {
				g_free(chanp->topic);
				chanp->topic  = g_strdup(args[2]);

				coloured = irc_ircoldcolstr_to_ekgcolstr(s, chanp->topic, 1);
				tmpchn	= clean_channel_names(s, args[1]);
				print_info(dest, s, irccommands[ecode].name, session_name(s), tmpchn, coloured);
				xfree(coloured);
			}
			break;
		case 333:	// RPL_TOPICBY		[mynick] [channel] [nick!user@host] [time]
				//		or	[mynick] [channel] [time]
			IRC_TO_LOWER(args[1]);
			if ((chanp = irc_find_channel(j->channels, args[1]))) {
				char *nick = "unknown";
				if (n_params>3) {
					nick = args[2];
					if ((bang = xstrchr(args[2], '!'))) *bang = '\0';
					try = atol(args[3]);
				} else {
					try = atol(args[2]);
				}
				g_free(chanp->topicby);
				chanp->topicby = g_strdup(args[2]);
				print_info(dest, s, "IRC_RPL_TOPICBY",
						session_name(s), nick, bang?bang+1:"",
						try ? ctime(&try) : "unknown");
				if (bang) *bang ='!';
			}
			break;

		case 341:	// RPL_INVITING		[mynick] [nick] [channel]
			tmpchn = clean_channel_names(s, args[2]);
			print_info(dest, s, irccommands[ecode].name, session_name(s), args[1], tmpchn);
			break;
		case 376:	// RPL_ENDOFMOTD	[mynick] [:End of MOTD command]
			/* zero, identify with nickserv */
			if (xstrlen(session_get(s, "identify"))) {
				/* temporary */
				irc_write(s, "PRIVMSG nickserv :IDENTIFY %s\n", session_get(s, "identify"));
				/* XXX, bedzie:
				 *	session_get(s, "identify")
				 *		<nick_ns> <host_ns *weryfikacja zeby nikt nie spoofowac*> "<NICK1 HASLO>" "<NICK2 HASLO>" "[GLOWNE HASLO]"
				 *
				 *	array_make() splitowane po spacjach. " " traktuj jako jeden parametr
				 *		p[2...x] jesli ma spacje sprawdz czy nicki do spacji sie zgadzaja... jesli tak uzyj haslo.
				 *				jesli nie, uzywaj haslo.
				 *
				 *	i dopiero robmy IDENTIFY jesli dostaniem request od serwisow !!!
				 */
			}

			/* first we join */
			if (xstrlen(session_get(s, "AUTO_JOIN")))
				irc_write(s, "JOIN %s\r\n", session_get(s, "AUTO_JOIN"));
		case 372:	// RPL_MOTD		[mynick] [:- <text>]
		case 375:	// RPL_MOTDSTART	[mynick] [:- <server> Message of the day - ]
			if (session_int_get(s, "SHOW_MOTD") != 0) {
				char *txt = ('-' == *args[1]) ? args[1]+1 : args[1]; 	/* skip first hyphen */
				coloured = irc_ircoldcolstr_to_ekgcolstr(s, txt, 1);
				print_info("__status", s, irccommands[ecode].name, session_name(s), coloured);
				xfree(coloured);
			}
			break;
		default:
			return(-1);
	}

	xfree(tmpchn);
	xfree(t);
	return 0;
}

char *clean_channel_names(session_t *session, char *channels) {
	irc_private_t *j = session->priv;
	char *chmode;

	char *dest, *src, *next, *p;
	int len, skip;
	char *ret;

	if (!irc_config_clean_channel_name)
		return xstrdup(channels);

	if (!SOP(_005_IDCHAN))
		return xstrdup(channels);

	chmode = j->nick_signs;

	dest = src = ret = xstrdup(channels);
	while ( src && *src ) {
		char *idchan;

		if ((*src == ' ') || (strchr(chmode, *src))) {
			*dest++ = *src++;
			continue;
		}
		p = src;

		next = strchr(src, ' ');

		if (next)
			*next = '\0';

		idchan = SOP(_005_IDCHAN);
		while (*idchan) {
			char chpfx = *idchan;

			if (idchan[1] != ':')		/* ?WO? Check it only when IDCHAN is set & dot't check here */
				break;

			skip = strtoul(idchan+2, &idchan, 10);
			if (*idchan == ',')
				idchan++;
			else if (*idchan)		/* ?WO? Again: Check it only when IDCHAN is set & dot't check here */
				break;

			if (chpfx != p[0])
				break;

			if (strlen(p)-2 < skip)		/* we need prefix and one (or more) chars in channel name */
				break;

			strcpy(p + 1, p + skip + 1);
		}

		len = strlen(p);
		strcpy(dest, p);
		dest += len;
		src = next;
		if (next)
			*next=' ';
	}
	*dest='\0';
	return ret;
}

IRC_COMMAND(irc_c_whois) {
/*
	311	RPL_WHOISUSER		[mynick] <nick> <user> <host> * :<real name>
	312	RPL_WHOISSERVER		[mynick] <nick> <server> :<server info>
	313	RPL_WHOISOPERATOR	[mynick] <nick> :is an IRC operator
	314	RPL_WHOWASUSER		[mynick] <nick> <user> <host> * :<real name>
	317	RPL_WHOISIDLE		[mynick] <nick> <integer> :seconds idle
	318	RPL_ENDOFWHOIS		[mynick] <nick> :End of WHOIS list
	319	RPL_WHOISCHANNELS	[mynick] <nick> :*( ( "@" / "+" ) <channel> " " )
	320	RPL_WHOISMISC		[mynick]
	369	RPL_ENDOFWHOWAS		[mynick] <nick> :End of WHOWAS
*/
	char		*t = irc_uid(args[1]), *dest = NULL;
	char		*str, *tmp, *col[5];
	int		secs = 0, mins, hours, days, which, i;
	time_t		timek;
	int		timek_int = (int) timek;
	window_t	*w = window_find_s(s, t);

	if (session_int_get(s, "DISPLAY_IN_CURRENT")&2)
		dest = w?t:NULL;

	if (irccommands[ecode].num != 317) { /* idle */
		char *chlist = NULL;
		int n_params = g_strv_length(args);

		for (i=0; i<5; i++)
			col[i] = irc_ircoldcolstr_to_ekgcolstr(s, (1+i<n_params)?args[1+i]:NULL, 1);

		if (irccommands[ecode].num == 319)
			chlist = clean_channel_names(s, col[1]);
		/*
		if (irccommands[ecode].future & IRC_WHOERR)
			print_info(dest, s, "IRC_WHOERROR", session_name(s), col[0],  col[1]);
		else
		*/
			print_info(dest, s, irccommands[ecode].name,
					session_name(s), col[0], chlist ? chlist : col[1],
					col[2], col[3], col[4]);

		for (i=0; i<5; i++)
			xfree(col[i]);

		xfree(chlist);
		xfree(t);
		return (0);
	}
	gatoi(NOK(2), &secs);
	which = gatoi(NOK(3), &timek_int);
	timek = (time_t)timek_int;

	/* GiM: Yes, I know what is modulo ;> */
	mins = secs/60;
	secs -= (mins * 60);
	hours = mins/60;
	mins -= (hours * 60);
	days = hours/24;
	hours -= (days * 24);

#define IOK3(x) (x)?(x):""
	/* GiM: No, I'm not going to do the same in polish
	 * it'd have to be more cases ;> */
	str = days?saprintf("%d %s ", days, days==1?"day":"days"):NULL;
	tmp = hours?saprintf("%s %d %s ", IOK3(str), hours,
			hours==1?"hour":"hours"):str;
	if (hours) xfree(str); str=tmp;
	tmp = mins?saprintf("%s %d %s ", IOK3(str), mins,
			mins==1?"minute":"minutes"):str;
	if (mins) xfree(str); str=tmp;
	tmp = secs?saprintf("%s %d %s ", IOK3(str), secs,
			secs==1?"second":"seconds"):str;
	if (secs) xfree(str); str=tmp;

	if (!str) str = xstrdup("Incredible, no idle!");

	tmp = xstrdup(ctime(&timek));
	if (tmp && tmp[xstrlen(tmp)-1] == '\n') tmp[xstrlen(tmp)-1]='\0';

	print_info(dest, s, irccommands[ecode].name,
			session_name(s), NOK(1), str,
			which?"N/A":tmp);
	xfree(t);
	xfree(str);
	xfree(tmp);
	return 0;
}

int mode_act = 0;

/**
 * irc_c_list - this function is only for evil hackers,
 * do not touch this stuff unless you want yourself get burned :>
 *
 * handling some list stuff
 * STATS, WHO, LIST, LINKS, IVITELIST, EXCEPTLIST, BANLIST
 */
IRC_COMMAND(irc_c_list)
{
#define PRINT_INFO if (!chan || !chan->syncmode) print_info
	char		*dest, *t = NULL;
	int		ltype = irccommands[ecode].future;

	int		endlist = ltype & IRC_LISTEND;
	char		*realname;
	char		*coloured = NULL;
	char		*tmpchn = NULL;

	window_t	*w	  = NULL;
	people_t	*osoba	  = NULL;
	channel_t	*chan	  = NULL;
	list_t		*tlist	  = NULL;

	if (endlist) ltype -= IRC_LISTEND;

	if (ltype == IRC_LISTWHO || ltype == IRC_LISTCHA || ltype == IRC_LISTSTA)
		t = NULL;
	else {
		IRC_TO_LOWER(NOK(1));
		t = irc_uid(NOK(1));
	}

	w    = window_find_s(s, t);
	dest = w?t:NULL;

	if (ltype == IRC_LISTWHO || ltype == IRC_LISTBAN) {
		IRC_TO_LOWER(NOK(1));
		chan = irc_find_channel(j->channels, NOK(1));
		/* debug("!!!> %s %08X %d %d\n", NOK(1), chan, chan?chan->syncmode:-1, ltype); */
	}

	if (!mode_act && ltype != IRC_LISTCHA)
		PRINT_INFO(dest, s, "RPL_LISTSTART", session_name(s));

	if (endlist) {
		if (!mode_act)
			PRINT_INFO(dest, s, "RPL_EMPTYLIST", session_name(s), NOK(1));

		if (ltype == IRC_LISTSTA) {
			print_info(dest, s, "RPL_STATSEND", session_name(s), NOK(2), NOK(1));
		} else if (ltype == IRC_LISTCHA) {
			print_info(dest, s, "RPL_ENDOFLIST", session_name(s), NOK(1));
		} else {
			PRINT_INFO(dest, s, "RPL_ENDOFLIST", session_name(s), NOK(2));
		}
		if (chan) {
			if (chan->syncmode > 0)  {
				chan->syncmode--;
				if (chan->syncmode == 0) {
					tmpchn = clean_channel_names(s, chan->name+4);
					GTimeVal tv;
					g_get_current_time(&tv);
					tv.tv_usec+=(1000000-chan->syncstart.tv_usec);
					if (tv.tv_usec>1000000)
						tv.tv_sec++, tv.tv_usec-=1000000;
					tv.tv_sec-=chan->syncstart.tv_sec;

					print_info(dest, s, "IRC_CHANNEL_SYNCED", session_name(s), tmpchn, ekg_itoa(tv.tv_sec), ekg_itoa(tv.tv_usec));
				}
			}
		}
		mode_act = 0;
	} else {
		if (irccommands[ecode].num != 321)
			mode_act++;
		switch (ltype) {
			/* TODO: poprawic te 2 pierwsze... */
			case (IRC_LISTSTA):
				print_info(dest, s, irccommands[ecode].name, session_name(s), ekg_itoa(mode_act), NOK(1), NOK(2), NOK(3), NOK(4), NOK(5), NOK(6));
				break;
			case (IRC_LISTWHO):
				/* ok new irc-find-person checked */
				osoba	 = irc_find_person(j, j->people, NOK(5));
				realname = xstrchr(NOK(7), ' ');
				tmpchn = clean_channel_names(s, NOK(1));
				PRINT_INFO(dest, s, irccommands[ecode].name, session_name(s), ekg_itoa(mode_act), tmpchn, NOK(2), NOK(3), NOK(4), NOK(5), NOK(6), realname);
				if (osoba) {
					xfree(osoba->host);
					osoba->host = xstrdup(NOK(3));
					xfree(osoba->ident);
					osoba->ident= xstrdup(NOK(2));
					xfree(osoba->realname);
					osoba->realname = xstrdup(realname);

					if (chan && chan->syncmode)
						irc_access_parse(s, chan, osoba, 0);
				}
				break;
			/*
			case (IRC_LISTCHA):
				// TODO: /join #number (?)
				tlist = ...
			case (IRC_LISTINV):
				tlist = ...
			case (IRC_LISTEXC):
				tlist = ...
*/
			case (IRC_LISTBAN):
				if (!tlist)
					tlist = &(chan->banlist);
				if (chan) {
					if (mode_act == 1 && *tlist) {
						debug("[IRC_LIST] Delete list 0x%x\n", *tlist);
						list_destroy(*tlist, 0);
						*tlist = NULL;
					}
					/*debug("[IRC_LIST] Add to list (id=%d; co=%s) 0x%x\n", mode_act, NOK(2), tlist);*/
					list_add(tlist, xstrdup(NOK(2)));
				}
			default:
				if (args[3] && *args[3]) {
					coloured = irc_ircoldcolstr_to_ekgcolstr(s, args[3], 1);
					PRINT_INFO(dest, s, irccommands[ecode].name, session_name(s), NOK(1), NOK(2), coloured, ekg_itoa(mode_act));
				} else {
					tmpchn = clean_channel_names(s, NOK(1));
					PRINT_INFO(dest, s, irccommands[ecode].name, session_name(s), tmpchn, NOK(2), NOK(3), ekg_itoa(mode_act));
				}
				xfree(coloured);
				break;
		}
	}

	xfree(tmpchn);
	xfree(t);
	return 0;
#undef PRINT_INFO
}

#undef IOK
#undef NOK

/* cmd  - PING
 * p[0] - (:server|:something)		;>
 */
IRC_COMMAND(irc_c_ping)
{
	char *arg0 = args[0]?args[0]:"";
	irc_write(s, "PONG :%s\r\n", arg0);
	if (session_int_get(s, "DISPLAY_PONG"))
		print_info("__status", s, "IRC_PINGPONG", session_name(s), arg0);
	return 0;
}

/* pfx  - :nick!ident@host
 * cmd  - NICK
 * p[0] - :newnick
 */
IRC_COMMAND(irc_c_nick)
{
	char		*temp;
	char		*newnick;
	int		nickdisp = session_int_get(s, "DISPLAY_NICKCHANGE");
	people_t	*per;
	people_chan_t	*ch;
	list_t		l;
	window_t	*w;

	newnick = args[0];

	irc_nick_change(s, j, pfx_nick, newnick);
	if (!xstrcmp(j->nick, pfx_nick)) {
		print_info(window_current->target, s, "IRC_YOUNEWNICK", session_name(s), pfx_ihost, newnick);

		xfree(j->nick);
		j->nick = xstrdup(newnick);
	} else {
		/* ok new irc-find-person checked */
		per = irc_find_person(j, j->people, newnick);
		debug_function("[irc]_c_nick %08X %s\n", per, pfx_nick);
		if (nickdisp || !per)
			print_info(nickdisp==2?window_current->target:"__status",
					s, "IRC_NEWNICK",
					session_name(s), pfx_nick, pfx_ihost, newnick);
		else if (per) {
			for (l = per->channels; l; l=l->next) {
				ch = (people_chan_t *)l->data;
				print_info(ch->chanp->name, s, "IRC_NEWNICK", session_name(s), pfx_nick, pfx_ihost, newnick);
			}
		}

		temp = irc_uid(pfx_nick);
		if ((w = window_find_s(s, temp))) {
			xfree(w->target);
			w->target = irc_uid(newnick);

			query_emit(NULL, "ui-window-target-changed", &w);

			print_window_w(w, EKG_WINACT_JUNK, "IRC_NEWNICK",
					session_name(s), pfx_nick, pfx_ihost, newnick);
		}
		xfree(temp);
	}
	return 0;
}

/* pfx - :nick!ident@host
 * cmd - PRIVMSG | NOTICE
 * p[0] - destination (channel|nick)
 * p[1] - :message
 * b
 */
IRC_COMMAND(irc_c_msg)
{
	char		*dest, *format;
	char		*head, *sender;
	char		*ctcpstripped, *coloured, nickmode[2];
	int		class, ekgbeep= EKG_NO_BEEP;
	int		mw, prv;
	window_t	*w = NULL;
	people_t	*person;
	people_chan_t	*perchn = NULL;
	int		secure = 0, xosd_to_us = 0, xosd_is_priv = 0;
	char		*ignore_nick = NULL;
	char		*message, *recipient, *clear_string;

	prv = !xstrcasecmp(cmdname, "privmsg");
	if (!prv && xstrcasecmp(cmdname, "notice"))
		return 0;

	mw = session_int_get(s, "make_window");

	sender = pfx_nick;

	recipient	= g_strdup(args[0]);	/* destination (channel|nick) */
	message		= g_strdup(args[1]);

	/* probably message from server ... */
	if (s->connecting && !prv) {
		/* (!xstrcmp(":_empty_", param[0]) || !xstrcmp("AUTH", args[0])) */
		class = (mw&16)?EKG_MSGCLASS_CHAT:EKG_MSGCLASS_MESSAGE;
		dest = xstrdup(recipient);
		format = xstrdup("irc_not_f_server");
		xosd_to_us = 1;
	/* priv_data message ... */
	} else if (!xstrcmp(j->nick, recipient)) {
		/* dj: if he's not on the list we should add him */
		/* G->dj: okey, but must be done in other way imho
		 * this 'param[0]' as a channel doesn't like nice to me...
		person = irc_find_person(j->people, param[0]+1);
		if (!person)
			person = irc_add_person(s, j, param[0]+1, NULL);
		if (person && t)
			irc_parse_ident_host(t+1, &(person->ident), &(person->host));
		*/
		class = (prv?(mw&2):(mw&1)) ? EKG_MSGCLASS_CHAT : EKG_MSGCLASS_MESSAGE;
		if (!*sender)
			sender = prefix;
		dest = irc_uid(sender);
		format = xstrdup(prv?"irc_msg_f_some":"irc_not_f_some");
		ekgbeep = EKG_TRY_BEEP;
		xosd_to_us = xosd_is_priv = 1;
	/* message on channel ... */
	} else {
		class = EKG_MSGCLASS_CHAT;
		dest = irc_uid(recipient);
		IRC_TO_LOWER(dest+4);

		w = window_find_s(s, dest);
		format = NULL;

		/* ok new irc-find-person checked */
		if (irc_config_allow_fake_contacts && !(person = irc_find_person(j, j->people, sender))) {
			person = irc_add_person(s, j, sender, dest);
		}

		if ((person = irc_find_person(j, j->people, sender)))
		{
			/* G->dj: I'm not sure if this what I've added
			 *	  will still do the same you wanted */
			if (pfx_ihost && !(person->ident) && !(person->host))
				irc_parse_ident_host(pfx_ihost, &(person->ident), &(person->host));

			perchn = irc_find_person_chan(person->channels, dest);
			debug("<person->channels: %08X %s %08X>\n", person->channels, dest, perchn);
		}

	}

	query_emit(NULL, prv ? "irc-privmsg" : "irc-notice", &(s->uid), &sender, &recipient, &message, &xosd_to_us);

	if (!xosd_to_us) {
		/* find our nick */
		char *p, *str, *str0;

		str0 = str = irc_ircoldcolstr_juststrip(s, message);
		while (!xosd_to_us && (p = xstrcasestr(str, j->nick))) {
			/* p - points to beginning of a nickname */
			char end = p[xstrlen(j->nick)];
			/* end - char after end of a nickname */
			if (!isalnum(end) && !isalpha_pl(end)) {
				/* End of nick is OK */
				if (p == str0 || (!isalnum(*(p-1)) && !isalpha_pl(*(p-1)) && *(p-1)!=1)) {
					/* nick found */
					ekgbeep = EKG_TRY_BEEP;
					xosd_to_us = 1;
				}
			}
			str = p + 1;
		}
		xfree(str0);
	}

	if ((ctcpstripped = ctcp_parser(s, prv, sender, recipient, message, xosd_to_us))) {
		char *padding = NULL;
		int isour = 0;

		if (xosd_is_priv) /* @ wrong place */
			query_emit(NULL, "message-decrypt", &(s->uid), &dest, &ctcpstripped, &secure , NULL);
		else
			query_emit(NULL, "message-decrypt", &dest, &(s->uid), &ctcpstripped, &secure , NULL);

		/* TODO 'secure' var checking, but still don't know how to react to it (GiM)
		 */
		coloured = irc_ircoldcolstr_to_ekgcolstr(s, ctcpstripped, 1);

		clear_string = irc_ircoldcolstr_juststrip(s, ctcpstripped);
		debug("<%c%s/%s> %s [%s]\n", perchn?*(perchn->sign):' ', sender, recipient, args[1], clear_string);
		xfree(clear_string);

		nickmode[1] = '\0';
		nickmode[0] = perchn?*(perchn->sign):' ';
		if (*nickmode==' ' && !session_int_get(s, "SHOW_NICKMODE_EMPTY"))
			*nickmode='\0';

		if (perchn)
			padding = nickpad_string_apply (perchn->chanp, sender);

		/* privmsg on channel */
		if (NULL == format)
		{
			if (xosd_to_us)
				ekgbeep = EKG_TRY_BEEP;

			/* rest */

			/* privmsg <--> notice
			 * w -> window not yet created (other format)
			 * ekgbeep -> higlight format or normal
			 */
			format = saprintf("irc_%s_f_chan%s%s", prv?"msg":"not",
					(!w)?"":"_n", ekgbeep?"h":"");

			if (!xosd_to_us)
				class |= EKG_MSGCLASS_NOT2US;
		}

		head = format_string(format_find(format), session_name(s),
				nickmode, sender, pfx_ihost, recipient, coloured, padding, "Y ");

		if (perchn)
			nickpad_string_restore (perchn->chanp);

		xfree(coloured);
		coloured = irc_ircoldcolstr_to_ekgcolstr(s, ctcpstripped, 0);

	/*
234707 <@dredzik> GiM, string nadawca, string wiadomo��, bool
234707 wiadomo��_do_ciebie, bool kana�_czy_priv, string
234707 je�eli_kana�_to_nazwa_kana�u
010539 <@dredzik> GiM, hm... jeszcze by si� przyda�a jedna rzecz - tak �eby
010539 pierwszym argumentem by�a sesja

isour - 0 tutaj czy wiadomosc jest od nas.

irc-protocol-message uid, nick, isour, istous, ispriv, dest.
	*/

		query_emit(NULL, "irc-protocol-message",
				&(s->uid), &sender, &coloured, &isour,
				&xosd_to_us, &xosd_is_priv, &dest);

		ignore_nick = irc_uid(sender);

		if (xosd_to_us && s->status == EKG_STATUS_AWAY && session_int_get(s, "away_log") == 1 && !(ignored_check(s, ignore_nick) & IGNORE_MSG)) {
			irc_awaylog_t *e = xmalloc(sizeof(irc_awaylog_t));

			if (xosd_is_priv) {
				e->channame	= NULL;
				e->uid		= xstrdup(dest);
			} else {
				e->uid		= irc_uid(sender);
				e->channame	= xstrdup(dest);
			}

			e->msg		= xstrdup(coloured);
			e->t		= time(NULL);

			list_add(&(j->awaylog), e);

		}

		xfree(ctcpstripped);
		xfree(coloured);
		class |= EKG_NO_THEMEBIT;

		if (xosd_is_priv || !(ignored_check(s, ignore_nick) & IGNORE_MSG))
			protocol_message_emit(s, dest, NULL, head, NULL, time(NULL), class, NULL, ekgbeep, secure);

		xfree(ignore_nick);
		xfree(head);
	}

	xfree(message);
	xfree(recipient);
	xfree(dest);
	xfree(format);

	return 0;
}
/* pfx  - :nick!ident@host
 * cmd  - JOIN
 * p[0] - :channel
 *
 */
IRC_COMMAND(irc_c_join)
{
	char		*ekg2_channel,  *chname, *irc_nick;
	char		*__channel;
	window_t	*newwin;
	people_t	*person;
	int		me = 0;

	/* irc channels are said to be case insensitive, so I think
	 * we can do it 'in place', without a copy
	 */
	__channel = xstrdup(IRC_TO_LOWER(args[0]));

	/* istnieje jaka�tam szansa �e kto� zrobi nick i part i b�dzie
	 * but I have no head to this now... */

	me = !xstrcmp(j->nick, pfx_nick); /* We join ? */

	if (query_emit(NULL, "irc-join", &s->uid, &__channel, &pfx_nick, &me, &pfx_ihost) == -1) {
		xfree(__channel);
		return -1;
	}

	irc_nick = irc_uid(pfx_nick);
	ekg2_channel = irc_uid(__channel);
	chname = clean_channel_names(s, __channel);

	if (me) {

		newwin = window_new(ekg2_channel, s, 0);

		if (xstrcmp(__channel, chname))
			newwin->alias = xstrdup(chname);
		else
			newwin->alias = xstrdup(__channel);

		query_emit(NULL, "ui-window-target-changed", &newwin);	/* let's emit UI_WINDOW_TARGET_CHANGED XXX, another/new query? */

		window_switch(newwin->id);
		debug_function("[irc] c_join() %08X\n", newwin);
		irc_add_channel(s, j , __channel, newwin);
	/* someone joined */
	} else {
		person = irc_add_person(s, j, pfx_nick, __channel);
		if (person && pfx_ihost && !(person->ident) && !(person->host))
			irc_parse_ident_host(pfx_ihost, &(person->ident), &(person->host));

		irc_access_parse(s, irc_find_channel(j->channels, __channel), person, 0);
	}

	if (!(ignored_check(s, ekg2_channel) & IGNORE_NOTIFY) && !(ignored_check(s, irc_nick) & IGNORE_NOTIFY)) {
		print_info(ekg2_channel, s, me ? "irc_joined_you" : "irc_joined",
				session_name(s), pfx_nick, pfx_ihost, chname);
		if (me)	{
			int __secure = 0;
			char *__sid	 = xstrdup(session_uid_get(s));
			char *__uid_full = xstrdup(ekg2_channel);
			char *__msg	 = xstrdup("test");

			if (query_emit(NULL, "message-encrypt", &__sid, &__uid_full, &__msg, &__secure) == 0 && __secure)
				print_info(ekg2_channel, s, "channel_secure", session_name(s), chname);
			else	print_info(ekg2_channel, s, "channel_unsecure", session_name(s), chname);
			xfree(__msg);
			xfree(__uid_full);
			xfree(__sid);
		}
	}

	xfree(chname);
	xfree(irc_nick);
	xfree(ekg2_channel);

	xfree(__channel);
	return 0;
}

/* pfx  - :nick!ident@host
 * cmd  - PART
 * p[0] - channel
 * (p[1] - :reason) - optional
 */
IRC_COMMAND(irc_c_part)
{
	char	*ekg2_channel, *coloured, *irc_nick;
	char	*__channel, *__reason;
	int	me = 0;

	debug_function("[irc]_c_part: %s %s\n", j->nick, pfx_nick);

	__channel = xstrdup(IRC_TO_LOWER(args[0]));
	__reason = g_strdup(args[1]);

	me = !xstrcmp(j->nick, pfx_nick); /* we part ? */

	if (query_emit(NULL, "irc-part", &s->uid, &__channel, &pfx_nick, &me, &pfx_ihost, &__reason) == -1) {
		xfree(__channel);
		xfree(__reason);
		return -1;
	}

	ekg2_channel = irc_uid(__channel);
	irc_nick = irc_uid(pfx_nick);
	/* Servers MUST be able to parse arguments in the form of
	 * a list of target, but SHOULD NOT use lists when sending
	 * PART messages to clients.
	 *
	 * damn it I think rfc should rather say MUSTN'T instead of
	 * SHOULD NOT ;/
	 */
	if (me)
		irc_del_channel(s, j, __channel);
	else
		irc_del_person_channel(s, j, pfx_nick, __channel);

	coloured = xstrlen(__reason) ? irc_ircoldcolstr_to_ekgcolstr(s, __reason, 1) : xstrdup(_("no reason"));

	/* TODO: if channel window exists do print_info, else do nothing (?)
	 * now after alt+k if user was on that channel-window, we recved info
	 * about parting on __status window, is it right ?
	 * G->dj: yep, but we can make this behaviour dependent on something
	 * e.g: on my fave: DISPLAY_IN_CURRENT :)
	 */
	if (!(ignored_check(s, ekg2_channel) & IGNORE_NOTIFY) && !(ignored_check(s, irc_nick) & IGNORE_NOTIFY)) {
		char *cchn = clean_channel_names(s, __channel);
		print_info(ekg2_channel, s, (me)?"irc_left_you":"irc_left", session_name(s),
				pfx_nick, pfx_ihost, cchn, coloured);
		xfree(cchn);
	}

	xfree(coloured);
	xfree(ekg2_channel);
	xfree(irc_nick);

	xfree(__channel);
	xfree(__reason);
	return 0;
}

/* this is quite similiar to PART
 * pfx  - :nick!ident@host
 * cmd  - KICK
 * p[0] - channel
 * p[1] - nick
 * (p[2] - :reason) - optional
 */
IRC_COMMAND(irc_c_kick)
{
	char	*ekg2_channel, *irc_channel, *coloured, *cchn;
	char	*_session, *_nick, *_uid;
	int	me = !xstrcmp(j->nick, args[1]);

	irc_channel = IRC_TO_LOWER(args[0]);
	ekg2_channel = irc_uid(irc_channel);

	/* we were kicked out */
	if (me)
		irc_del_channel(s, j, irc_channel);
	else
		irc_del_person_channel(s, j, args[1], irc_channel);

	coloured = xstrlen(args[2]) ? irc_ircoldcolstr_to_ekgcolstr(s, args[2], 1) : xstrdup(_("no reason"));

	cchn = clean_channel_names(s, irc_channel);
	/* session, kicked_nick, kicker_nick, kicker_ident+host, chan, reason */
	print_info(ekg2_channel, s, me ? "irc_kicked_you" : "irc_kicked",  session_name(s),
			args[1], *pfx_nick?pfx_nick:prefix, pfx_ihost,
			cchn, coloured);
	xfree(coloured);

/*sending irc-kick event*/
	_uid = irc_uid(*pfx_nick?pfx_nick:prefix);
	_session = xstrdup(session_uid_get(s));
	_nick = irc_uid(args[1]);
	query_emit(NULL, "irc-kick", &_session, &_nick, &ekg2_channel, &_uid);
	xfree(_uid);
	xfree(_nick);
	xfree(_session);

	xfree(cchn);
	xfree(ekg2_channel);
	return 0;
}

/* pfx  - :nick!ident@ihost
 * cmd  - QUIT
 * (p[0]) - reason
 */
IRC_COMMAND(irc_c_quit)
{
	char	*__reason;
	int	display_quit, me;
	/* TODO: SPLIT MODE! */
	int	split = 0;

	__reason = xstrlen(args[0]) ? irc_ircoldcolstr_to_ekgcolstr(s, args[0], 1) : xstrdup(_("no reason"));
	me = !xstrcmp(j->nick, pfx_nick); /* we quit? */

	if (query_emit(NULL, "irc-quit", &s->uid, &pfx_nick, &me, &pfx_ihost, &__reason) == -1) {
		xfree(__reason);
		return -1;
	}

	if (split)
		display_quit = 0; /* (?) */
	else
		display_quit = session_int_get(s, "DISPLAY_QUIT");

	irc_del_person(s, j, pfx_nick, pfx_ihost, __reason, !display_quit);

	if (display_quit)
		print_info(display_quit==2?window_current->target:"__status",
				s, (split)?"irc_split":"irc_quit",
				session_name(s), pfx_nick, pfx_ihost,
				__reason);

	xfree(__reason);

	return 0;
}
/*
 * pfx   :server
 * cmd	 353    RPL_NAMREPLY
 * p[0]	 nick
 * p[1]  ( '='/'*'/'@' )	'@' is used for secret channels,
 * 				'*' for private channels
 * 				'=' for others (public channels)
 * p[2]  channel
 * (p[3] :names)
 */
IRC_COMMAND(irc_c_namerpl)
{
	if (!args[1]) return -1;
	/* rfc2812 */
	if (*args[1] != '*' && *args[1] != '=' && *args[1] != '@')	{
		debug_error("[irc] c_namerpl() kindda shitty ;/\n");
		return -1;
	}
	if (!args[3]) {
		debug_error("[irc] c_namerpl() even more shitty!\n");
		return -1;
	}
	irc_add_people (s, j, args[3], IRC_TO_LOWER(args[2]));
	return 0;
}

/* pfx   - :nick!ident@ihost
 * cmd   - TOPIC
 * p[0]  - channel
 * (p[1] - topic)
 */
IRC_COMMAND(irc_c_topic)
{
	window_t	*w;
	char		*t, *dest=NULL;
	char		*coloured, *cchn;
	char		*__channel, *__topic;
	channel_t	*chanp = NULL;

	__channel = IRC_TO_LOWER(args[0]);
	__topic   = args[1];

	t = irc_uid(__channel);
	w = window_find_s(s, t);
	dest = w?w->target:NULL;
	g_free(t);

	cchn = clean_channel_names(s, __channel);

	if (xstrlen(__topic)) {
		coloured = irc_ircoldcolstr_to_ekgcolstr(s, __topic, 1);
		print_info(dest, s, "IRC_TOPIC_CHANGE", session_name(s), pfx_nick, pfx_ihost, cchn, coloured);
		xfree(coloured);
	} else {
		print_info(dest, s, "IRC_TOPIC_UNSET", session_name(s), pfx_nick, pfx_ihost, cchn);
	}

	g_free(cchn);

	if (!(chanp = irc_find_channel(j->channels, __channel)))
		return 0;

	g_free(chanp->topic);
	g_free(chanp->topicby);

	chanp->topicby = g_strdup(pfx_nick);
	chanp->topic   = g_strdup(xstrlen(__topic) ? __topic : _("No topic set!"));

	return 0;
}

#ifdef GDEBUG
#ifndef MARLENE
#error She's all I really care about. You shouldn't play with my GDEBUG!
#endif
#endif

/* pfx  - :nick!ident@ihost
 * cmd  - INVITE
 * p[0] - our nickname
 * p[1] - :channel
 */
/* TODO: add the person, that invites us, to list ? */
IRC_COMMAND(irc_c_invite)
{
	char	*cchn, *channel;

	channel = args[1];

	IRC_TO_LOWER(channel);
	cchn = clean_channel_names(s, channel);

	print_info(window_current->target, s, "IRC_INVITE", session_name(s), pfx_nick, pfx_ihost, args[0], cchn);
	xfree(cchn);

	if (session_int_get(s, "AUTO_JOIN_CHANS_ON_INVITE") == 1)
		irc_write(s, "JOIN %s\r\n", channel);

	return 0;
}

IRC_COMMAND(irc_c_mode)
{
	int		k, act=1, is324=irccommands[ecode].num==324;
	char		*t, *bang, **pars, *ekg2_channame, *irc_channame, *mode_abcd, *mode_c, *mode_d=NULL, *cchn;
	people_t	*per;
	people_chan_t	*ch;
	channel_t	*chan;
	userlist_t	*ul;
	window_t	*w;
	string_t	moderpl;

	/* MODE <channel|nick> <mode> <modeparams>
	 * <nick> <chan> <mode> <modeparams>
	 *
	 * 324    RPL_CHANNELMODEIS
	 *	<channel> <mode> <mode params>
	 */
	/* GiM: FIXME TODO [this shouldn't be xstrcasecmp! user mode
	 *	[well now [o3:o5:o4 CET 2oo8-16-o2] I think it's ok]
	 */
	if (is324) {
		args = &(args[1]);
	} else if (!xstrcasecmp(args[0], j->nick)) {
		print_info(window_current->target, s,
				"IRC_MODE", session_name(s),
				prefix, IRC_TO_LOWER(args[1]) );
		return 0;
	}

	irc_channame = IRC_TO_LOWER(args[0]);
	ekg2_channame = irc_uid(irc_channame);
	cchn = clean_channel_names(s, irc_channame);

	/* chan modes */
	mode_abcd = SOP(_005_CHANMODES);
	mode_c = mode_d = mode_abcd + xstrlen(mode_abcd);
	if ( (mode_c = xstrchr(mode_abcd, ',')) && ++mode_c)
		if ( (mode_c = xstrchr(mode_c, ',')) && ++mode_c)
			if ((mode_d=xstrchr(mode_c, ',')))
				mode_d++;

	for (t=args[1], k=2; *t && xstrlen(args[k]); t++) {
		char * __mode, *__param, *__p0 = prefix;

		if (*t=='+' || *t=='-') {
			act = ('+' == *t);
			continue;
		}

		/* 23:26:o2 CET 2oo5-22-o1 yet another ivil hack */
		if (xstrchr(args[k], ' '))
			*xstrchr(args[k], ' ') = '\0';

		__param = args[k];

		if ((bang = xstrchr(mode_abcd, *t))) {
			if ( (bang >= mode_d) ||		/* mode D never has a parameter */
			     ((bang >= mode_c) && !act) )	/* mode C only has a parameter when set */
			{
				__param = NULL;
			} else {				/* modes A & B always has a parameter */
				k++;
			}

			__mode = xstrndup(t, 1);
			query_emit(NULL, "irc-mode", &s->uid, &__p0, &irc_channame, &act, &__mode, &__param);
			xfree(__mode);

			continue;
		}

		/* Modes in PREFIX are not listed but could be considered type B. */
		if (!(bang=xstrchr(j->nick_modes, *t))) {
			debug_error("irc_c_mode() - unknown mode '%c'\n", *t);
			continue;
		}

		__mode = xstrndup(t, 1);
		query_emit(NULL, "irc-mode", &s->uid, &__p0, &irc_channame, &act, &__mode, &__param);
		xfree(__mode);

		if ((per = irc_find_person(j, j->people, args[k])) &&
		    (ch = irc_find_person_chan(per->channels, irc_channame)) )
		{
			int mask = 1 << (bang - j->nick_modes);

			if (act)	ch->mode |= mask;
			else		ch->mode &=~mask;

			if ((ul = userlist_find_u(&(ch->chanp->window->userlist), args[k]))) {
				irc_nick_prefix(j, ch, irc_color_in_contacts(j, ch->mode, ul));
				query_emit(NULL, "userlist-refresh");
			}
		}

		k++;
	}

	w = window_find_s(s, ekg2_channame);
	moderpl =  string_init("");
	pars=&(args[1]);

	while (*pars) {
		string_append(moderpl, *pars++);
		if (*pars)
			string_append_c(moderpl, ' ');
	}
	if (!is324) {
		print_info(w?w->target:NULL, s, "IRC_MODE_CHAN_NEW", session_name(s),
				pfx_nick, pfx_ihost, cchn, moderpl->str);
/*		if (moderpl->str[1] == 'b')
 *			irc_write(s, "MODE %s +%c\r\n",  irc_channame, moderpl->str[1]);
 */
	} else {
		print_info(w?w->target:NULL, s, "IRC_MODE_CHAN", session_name(s),
				cchn, moderpl->str);

		if ((chan = irc_find_channel(j->channels, irc_channame))) {
			xfree(chan->mode_str);
			chan->mode_str = xstrdup(moderpl->str);
		}
	}
	string_free(moderpl, 1);

	xfree(cchn);
	xfree(ekg2_channame);
	return 0;
}

#undef prefix
#undef pfx_nick
#undef pfx_ihost
#undef cmdname


/*
 * Local Variables:
 * mode: c
 * c-file-style: "k&r"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
