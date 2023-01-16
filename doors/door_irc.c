/*
 * LBBS -- The Lightweight Bulletin Board System
 *
 * Copyright (C) 2023, Naveen Albert
 *
 * Naveen Albert <bbs@phreaknet.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief IRC client
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#include "include/bbs.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h> /* use gettimeofday */

#include "include/module.h"
#include "include/node.h"
#include "include/user.h"
#include "include/door.h"
#include "include/term.h"
#include "include/linkedlists.h"
#include "include/utils.h"
#include "include/config.h"
#include "include/startup.h"

#include "lirc/irc.h"

static int unloading = 0;

struct participant {
	struct bbs_node *node;
	struct client *client;				/* Reference to the underlying client */
	const char *channel;				/* Channel */
	int chatpipe[2];					/* Pipe to store data */
	RWLIST_ENTRY(participant) entry;	/* Next participant */
};

RWLIST_HEAD(participants, participant);

struct client {
	RWLIST_ENTRY(client) entry; 		/* Next client */
	struct participants participants; 	/* List of participants */
	struct irc_client *client;			/* IRC client */
	pthread_t thread;					/* Thread for relay */
	unsigned int log:1;					/* Log to log file */
	FILE *logfile;						/* Log file */
	char name[0];						/* Unique client name */
};

RWLIST_HEAD_STATIC(clients, client);

static void __client_log(enum irc_log_level level, int sublevel, const char *file, int line, const char *func, const char *msg)
{
	/* Log messages already have a newline, don't add another one */
	switch (level) {
		case IRC_LOG_ERR:
			__bbs_log(LOG_ERROR, 0, file, line, func, "%s", msg);
			break;
		case IRC_LOG_WARN:
			__bbs_log(LOG_WARNING, 0, file, line, func, "%s", msg);
			break;
		case IRC_LOG_INFO:
			__bbs_log(LOG_NOTICE, 0, file, line, func, "%s", msg);
			break;
		case IRC_LOG_DEBUG:
			__bbs_log(LOG_DEBUG, sublevel, file, line, func, "%s", msg);
			break;
	}
}

static int load_config(void)
{
	struct bbs_config_section *section = NULL;
	struct bbs_config *cfg = bbs_config_load("door_irc.conf", 1);

	if (!cfg) {
		bbs_error("File 'door_irc.conf' is missing: IRC client declining to start\n");
		return -1; /* Abort, if we have no users, we can't start */
	}

	RWLIST_WRLOCK(&clients);
	while ((section = bbs_config_walk(cfg, section))) {
		struct client *client;
		int flags = 0;
		struct irc_client *ircl;
		const char *hostname, *username, *password, *autojoin;
		unsigned int port = 0;
		int tls = 0, tlsverify = 0, sasl = 0, logfile;
		if (!strcmp(bbs_config_section_name(section), "general")) {
			continue; /* Skip [general] */
		}
		/* It's a client section */
		hostname = bbs_config_sect_val(section, "hostname");
		username = bbs_config_sect_val(section, "username");
		password = bbs_config_sect_val(section, "password");
		autojoin = bbs_config_sect_val(section, "autojoin");
		/* XXX This is not efficient, there should be versions that take section directly */
		bbs_config_val_set_uint(cfg, bbs_config_section_name(section), "port", &port);
		bbs_config_val_set_true(cfg, bbs_config_section_name(section), "tls", &tls);
		bbs_config_val_set_true(cfg, bbs_config_section_name(section), "tlsverify", &tlsverify);
		bbs_config_val_set_true(cfg, bbs_config_section_name(section), "sasl", &sasl);
		bbs_config_val_set_true(cfg, bbs_config_section_name(section), "logfile", &logfile);
		client = calloc(1, sizeof(*client) + strlen(bbs_config_section_name(section)) + 1);
		if (!client) {
			bbs_error("calloc failed\n");
			continue;
		}
		strcpy(client->name, bbs_config_section_name(section)); /* Safe */
		ircl = irc_client_new(hostname, port, username, password);
		if (!ircl) {
			free(client);
			continue;
		}
		irc_client_autojoin(ircl, autojoin);
		if (tls) {
			flags |= IRC_CLIENT_USE_TLS;
		}
		if (tlsverify) {
			flags |= IRC_CLIENT_VERIFY_SERVER;
		}
		if (sasl) {
			flags |= IRC_CLIENT_USE_SASL;
		}
		irc_client_set_flags(ircl, flags);
		client->client = ircl;
		client->log = logfile;
		RWLIST_INSERT_TAIL(&clients, client, entry);
	}
	RWLIST_UNLOCK(&clients);
	return 0;
}

/* Forward declaration */
static void *client_relay(void *varg);

static int start_clients(void)
{
	struct client *client;
	int res, started = 0;

	RWLIST_WRLOCK(&clients);
	RWLIST_TRAVERSE_SAFE_BEGIN(&clients, client, entry) {
		res = irc_client_connect(client->client); /* Actually connect */
		if (!res) {
			res = irc_client_login(client->client); /* Authenticate */
		}
		if (!res && !irc_client_connected(client->client)) {
			bbs_error("Attempted to start client '%s', but disconnected prematurely?\n", client->name);
			res = -1;
		}
		if (res) {
			/* Connection failed? Remove it */
			bbs_error("Failed to start IRC client '%s'\n", client->name);
			irc_client_destroy(client->client);
			RWLIST_REMOVE_CURRENT(entry);
			free(client);
		} else {
			started++;
			/* Now, start the event loop to receive messages from the server */
			if (bbs_pthread_create(&client->thread, NULL, client_relay, (void*) client)) {
				return -1;
			}
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	RWLIST_UNLOCK(&clients);
	if (started) {
		bbs_verb(4, "Started %d IRC client%s\n", started, ESS(started));
	}
	return 0;
}

static void leave_client(struct client *client, struct participant *participant)
{
	struct participant *p;

	/* Lock the entire list first */
	RWLIST_WRLOCK(&clients);
	if (unloading) {
		RWLIST_UNLOCK(&clients);
		/* If the module is being unloaded, the client no longer exists.
		 * The participant list has also been freed. Just free ourselves and get out of here. */
		free(participant);
		return;
	}
	RWLIST_WRLOCK(&client->participants);
	RWLIST_TRAVERSE_SAFE_BEGIN(&client->participants, p, entry) {
		if (p == participant) {
			RWLIST_REMOVE_CURRENT(entry);
			/* Close the pipe */
			close(p->chatpipe[0]);
			close(p->chatpipe[1]);
			/* Free */
			free(p);
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	if (!p) {
		bbs_error("Failed to remove participant %p (node %d) from client %s?\n", participant, participant->node->id, client->name);
	}
	RWLIST_UNLOCK(&client->participants);
	RWLIST_UNLOCK(&clients);
}

static struct participant *join_client(struct bbs_node *node, const char *name)
{
	struct participant *p;
	struct client *client;

	RWLIST_WRLOCK(&clients);
	RWLIST_TRAVERSE(&clients, client, entry) {
		if (!strcasecmp(client->name, name)) {
			break;
		}
	}
	if (!client) {
		bbs_error("IRC client %s doesn't exist\n", name);
		return NULL;
	}
	/* Okay, we have the client. Add the newcomer to it. */
	p = calloc(1, sizeof(*p));
	if (!p) {
		bbs_error("calloc failure\n");
		RWLIST_UNLOCK(&clients);
		return NULL;
	}
	p->node = node;
	p->client = client;
	if (pipe(p->chatpipe)) {
		bbs_error("Failed to create pipe\n");
		free(p);
		RWLIST_UNLOCK(&clients);
		return NULL;
	}
	RWLIST_INSERT_HEAD(&client->participants, p, entry);
	RWLIST_UNLOCK(&clients);
	return p;
}

/* Forward declaration */
static int __attribute__ ((format (gnu_printf, 5, 6))) _chat_send(struct client *client, struct participant *sender, const char *channel, int dorelay, const char *fmt, ...);

#define relay_to_local(client, channel, fmt, ...) _chat_send(client, NULL, channel, 0, fmt, __VA_ARGS__)

static void handle_irc_msg(struct client *client, struct irc_msg *msg)
{
	if (msg->numeric) {
		/* Just ignore all these */
		switch (msg->numeric) {
		default:
			bbs_debug(5, "Got numeric: prefix: %s, num: %d, body: %s\n", msg->prefix, msg->numeric, msg->body);
		}
		return;
	}
	/* else, it's a command */
	if (!msg->command) {
		assert(0);
	}
	if (!strcmp(msg->command, "PRIVMSG") || !strcmp(msg->command, "NOTICE")) { /* This is intentionally first, as it's the most common one. */
		/* NOTICE is same as PRIVMSG, but should never be acknowledged (replied to), to prevent loops, e.g. for use with bots. */
		char *channel, *body = msg->body;

		/* Format of msg->body here is CHANNEL :BODY */
		channel = strsep(&body, " ");
		body++; /* Skip : */

		if (*body == 0x01) { /* sscanf stripped off the leading : */
			/* CTCP command: known extended data = ACTION, VERSION, TIME, PING, DCC, SED, etc. */
			/* Remember: CTCP requests use PRIVMSG, responses use NOTICE! */
			char *tmp, *ctcp_name;
			enum irc_ctcp ctcp;

			body++; /* Skip leading \001 */
			if (!*body) {
				bbs_error("Nothing after \\001?\n");
				return;
			}
			/* Don't print the trailing \001 */
			tmp = strchr(body, 0x01);
			if (tmp) {
				*tmp = '\0';
			} else {
				bbs_error("Couldn't find trailing \\001?\n");
			}

			ctcp_name = strsep(&body, " ");

			tmp = strchr(msg->prefix, '!');
			if (tmp) {
				*tmp = '\0'; /* Strip everything except the nickname from the prefix */
			}

			ctcp = irc_ctcp_from_string(ctcp_name);
			if (ctcp < 0) {
				bbs_error("Unsupported CTCP extended data type: %s\n", ctcp_name);
				return;
			}

			if (!strcmp(msg->command, "PRIVMSG")) {
				switch (ctcp) {
				case CTCP_ACTION: /* /me, /describe */
					relay_to_local(client, channel, "[ACTION] <%s> %s\n", msg->prefix, body);
					break;
				case CTCP_VERSION:
					irc_client_ctcp_reply(client->client, msg->prefix, ctcp, BBS_SHORTNAME " / LIRC 0.1.0");
					break;
				case CTCP_PING:
					irc_client_ctcp_reply(client->client, msg->prefix, ctcp, body); /* Reply with the data that was sent */
					break;
				case CTCP_TIME:
					{
						char timebuf[32];
						time_t nowtime;
						struct tm nowdate;

						nowtime = time(NULL);
						localtime_r(&nowtime, &nowdate);
						strftime(timebuf, sizeof(timebuf), "%a %b %e %Y %I:%M:%S %P %Z", &nowdate);
						irc_client_ctcp_reply(client->client, msg->prefix, ctcp, timebuf);
					}
					break;
				default:
					bbs_warning("Unhandled CTCP extended data type: %s\n", ctcp_name);
				}
			} else { /* NOTICE (reply) */
				/* Ignore */
			}
		} else {
			char *tmp = strchr(msg->prefix, '!');
			if (tmp) {
				*tmp = '\0'; /* Strip everything except the nickname from the prefix */
			}
			relay_to_local(client, channel, "<%s> %s\n", msg->prefix, body);
		}
	} else if (!strcmp(msg->command, "PING")) {
		/* Reply with the same data that it sent us (some servers may actually require that) */
		int sres = irc_send(client->client, "PONG :%s", msg->body ? msg->body + 1 : ""); /* If there's a body, skip the : and bounce the rest back */
		if (sres) {
			return;
		}
	} else if (!strcmp(msg->command, "JOIN")) {
		relay_to_local(client, msg->body, "%s has %sjoined%s\n", msg->prefix, COLOR_GREEN, COLOR_RESET);
	} else if (!strcmp(msg->command, "PART")) {
		relay_to_local(client, msg->body, "%s has %sleft%s\n", msg->prefix, COLOR_RED, COLOR_RESET);
	} else if (!strcmp(msg->command, "QUIT")) {
		relay_to_local(client, msg->body, "%s has %squit%s\n", msg->prefix, COLOR_RED, COLOR_RESET);
	} else if (!strcmp(msg->command, "KICKED")) {
		relay_to_local(client, msg->body, "%s has been %skicked%s\n", msg->prefix, COLOR_RED, COLOR_RESET);
	} else if (!strcmp(msg->command, "NICK")) {
		relay_to_local(client, NULL, "%s is %snow known as%s %s\n", msg->prefix, COLOR_CYAN, COLOR_RESET, msg->body);
	} else if (!strcmp(msg->command, "MODE")) {
		/* Ignore */
	} else if (!strcmp(msg->command, "ERROR")) {
		/* Ignore, do not send errors to users */
	} else if (!strcmp(msg->command, "TOPIC")) {
		/* Ignore */
	} else {
		bbs_warning("Unhandled command: prefix: %s, command: %s, body: %s\n", msg->prefix, msg->command, msg->body);
	}
}

static void *client_relay(void *varg)
{
	struct client *client = varg;
	/* Thread will get killed on shutdown */

	int res = 0;
	char readbuf[IRC_MAX_MSG_LEN + 1];
	struct irc_msg msg;
	char *prevbuf, *mybuf = readbuf;
	int prevlen, mylen = sizeof(readbuf) - 1;
	char *start, *eom;
	int rounds;
	char logfile[256];

	snprintf(logfile, sizeof(logfile), "%s/irc_%s.txt", BBS_LOG_DIR, client->name);

	if (client->log) {
		client->logfile = fopen(logfile, "a"); /* Create or append */
		if (!client->logfile) {
			bbs_error("Failed to open log file %s: %s\n", logfile, strerror(errno));
			return NULL;
		}
	}

	start = readbuf;
	for (;;) {
begin:
		rounds = 0;
		if (mylen <= 1) {
			/* IRC max message is 512, but we could have received multiple messages in one read() */
			char *a;
			/* Shift current message to beginning of the whole buffer */
			for (a = readbuf; *start; a++, start++) {
				*a = *start;
			}
			*a = '\0';
			mybuf = a;
			mylen = sizeof(readbuf) - 1 - (mybuf - readbuf);
			start = readbuf;
			if (mylen <= 1) { /* Couldn't shift, whole buffer was full */
				/* Could happen but this would not be valid. Abort read and reset. */
				bbs_error("Buffer truncation!\n");
				start = readbuf;
				mybuf = readbuf;
				mylen = sizeof(readbuf) - 1;
			}
		}
		/* Wait for data from server */
		if (res != sizeof(readbuf) - 1) {
			/* XXX We don't poll if we read() into an entirely full buffer and there's still more data to read.
			 * poll() won't return until there's even more data (but it feels like it should). */
			res = irc_poll(client->client, -1, -1);
			if (res <= 0) {
				break;
			}
		}
		prevbuf = mybuf;
		prevlen = mylen;
		res = irc_read(client->client, mybuf, mylen);
		if (res <= 0) {
			break;
		}

		mybuf[res] = '\0'; /* Safe */
		do {
			eom = strstr(mybuf, "\r\n");
			if (!eom) {
				/* read returned incomplete message */
				mybuf = prevbuf + res;
				mylen = prevlen - res;
				goto begin; /* In a double loop, can't continue */
			}

			/* Got more than one message? */
			if (*(eom + 2)) {
				*(eom + 1) = '\0'; /* Null terminate before the next message starts */
			}

			memset(&msg, 0, sizeof(msg));
			if (client->logfile) {
				fprintf(client->logfile, "%s\n", start); /* Append to log file */
			}
			if (!irc_parse_msg(&msg, start)) {
				handle_irc_msg(client, &msg);
			}

			mylen -= (eom + 2 - mybuf);
			start = mybuf = eom + 2;
			rounds++;
		} while (mybuf && *mybuf);

		start = mybuf = readbuf; /* Reset to beginning */
		mylen = sizeof(readbuf) - 1;
	}

	bbs_debug(3, "IRC client '%s' thread has exited\n", client->name);
	return NULL;
}

static int __chat_send(struct client *client, struct participant *sender, const char *channel, int dorelay, const char *msg, int len)
{
	time_t now;
    struct tm sendtime;
	char datestr[18];
	int timelen;
	int res;
	struct participant *p;

	/* Calculate the current time once, for everyone, using the server's time (sorry if participants are in different time zones) */
	now = time(NULL);
    localtime_r(&now, &sendtime);
	/* So, %P is lowercase and %p is uppercase. Just consult your local strftime(3) man page if you don't believe me. Good grief. */
	strftime(datestr, sizeof(datestr), "%m-%d %I:%M:%S%P ", &sendtime); /* mm-dd hh:mm:ssPP + space at end (before message) = 17 chars */
	timelen = strlen(datestr); /* Should be 17 */
	bbs_assert(timelen == 17);

	/* If sender is set, it's safe to use even with no locks, because the sender is a calling function of this one */
	if (sender) {
		bbs_debug(7, "Broadcasting to %s,%s (except node %d): %s%.*s\n", client->name, channel, sender->node->id, datestr, len, msg);
	} else {
		bbs_debug(7, "Broadcasting to %s,%s: %s%.*s\n", client->name, channel, datestr, len, msg);
	}

	/* Relay the message to everyone */
	RWLIST_RDLOCK(&client->participants);
	if (dorelay) {
		irc_client_msg(client->client, channel, msg); /* Actually send to IRC */
	}
	RWLIST_TRAVERSE(&client->participants, p, entry) {
		/* We intentionally relaying to other BBS nodes ourselves, separately from IRC, rather than
		 * just enabling echo on the IRC client and letting that bounce back for other participants.
		 * This is because we don't want our own messages to echo back to ourselves,
		 * and rather than parse messages to figure out if we should ignore something we just sent,
		 * it's easier to not have to ignore anything in the first place (at least for this purpose, still need to do channel filtering) */
		if (p == sender) {
			continue; /* Don't send a sender's message back to him/herself */
		}
		/* XXX Restricts users to a single channel, currently */
		if (!strlen_zero(channel) && strcmp(p->channel, channel)) {
			continue; /* Channel filter doesn't match for this participant */
		}
		if (!NODE_IS_TDD(p->node)) {
			res = write(p->chatpipe[1], datestr, timelen); /* Don't send timestamps to TDDs, for brevity */
		}
		if (res > 0) {
			res = write(p->chatpipe[1], msg, len);
		}
		if (res <= 0) {
			bbs_error("write failed: %s\n", strerror(errno));
			continue; /* Even if one send fails, don't fail all of them */
		}
	}
	RWLIST_UNLOCK(&client->participants);
	return 0;
}

#define chat_send(client, sender, channel, fmt, ...) _chat_send(client, sender, channel, 1, fmt, __VA_ARGS__)

/*! \param sender If NULL, the message will be sent to the sender, if specified, the message will not be sent to this participant */
static int __attribute__ ((format (gnu_printf, 5, 6))) _chat_send(struct client *client, struct participant *sender, const char *channel, int dorelay, const char *fmt, ...)
{
	char *buf;
	int res, len;
	va_list ap;

	if (!strchr(fmt, '%')) {
		/* No format specifiers in the format string, just do it directly to avoid an unnecessary allocation. */
		return __chat_send(client, sender, channel, dorelay, fmt, strlen(fmt));
	}

	va_start(ap, fmt);
	len = vasprintf(&buf, fmt, ap);
	va_end(ap);

	if (len < 0) {
		bbs_error("vasprintf failure\n");
		return -1;
	}
	res = __chat_send(client, sender, channel, dorelay, buf, len);
	free(buf);
	return res;
}

static int participant_relay(struct bbs_node *node, struct participant *p, const char *channel)
{
	char buf[384];
	int res;
	struct client *c = p->client;

	/* Join the channel */
	bbs_clear_screen(node);
	chat_send(c, NULL, channel, "%s@%d has joined %s\n", bbs_username(node->user), p->node->id, channel);

	bbs_unbuffer(node); /* Unbuffer so we can receive keys immediately. Otherwise, might print a message while user is typing */

	for (;;) {
		/* We need to poll both the node as well as the participant (chat) pipe */
		res = bbs_poll2(node, SEC_MS(10), p->chatpipe[0]);
		if (res < 0) {
			break;
		} else if (res == 1) {
			/* Node has activity: Typed something */
			res = bbs_read(node, buf, 1);
			if (res <= 0) {
				break;
			}
			res = 0;
			if (buf[0] == '\n') { /* User just pressed ENTER. Um, okay. */
				continue;
			}
			bbs_writef(node, "%c", buf[0]);
			/* Now, buffer input */
			/* XXX The user will be able to use terminal line editing, except for the first char */
			/* XXX ESC should cancel */
			/* XXX All this would be handled once we have a terminal line editor that works with unbuffered input */
			bbs_buffer(node);
			res = bbs_poll_read(node, SEC_MS(30), buf + 1, sizeof(buf) - 2); /* Leave the first char in the buffer alone, -1 for null termination, and -1 for the first char */
			if (res <= 0) {
				break;
			}
			res++; /* Add 1, since we read 1 char prior to the last read */
			buf[res] = '\0'; /* Now we can use strcasecmp */
			/* strcasecmp will fail because the buffer has a LF at the end. Use strncasecmp, so anything starting with /help or /quit will technically match too */
			if (STARTS_WITH(buf, "/quit")) {
				break; /* Quit */
			}
			bbs_unbuffer(node);

			if (buf[res - 1] != '\n') {
				bbs_warning("Doesn't end in LF? (%d)\n", buf[res - 1]); /* If it doesn't send in a LF for some reason, tack one on so it displays properly to recipients */
			}
			chat_send(c, p, channel, buf[res - 1] == '\n' ? "<%s@%d> %s" : "<%s@%d> %s\n", bbs_username(node->user), node->id, buf); /* buf already contains a newline from the user pressing ENTER, so don't add another one */
		} else if (res == 2) {
			/* Pipe has activity: Received a message */
			res = 0;
			res = read(p->chatpipe[0], buf, sizeof(buf) - 1);
			if (res <= 0) {
				break;
			}
			buf[res] = '\0'; /* Safe */
			/* Don't add a trailing LF, the sent message should already had one. */
			if (bbs_writef(node, "%.*s", res, buf) < 0) {
				res = -1;
				break;
			}
			/* Since we null terminated the buffer, we can safely use strstr */
			if (strcasestr(buf, bbs_username(node->user))) {
				bbs_debug(3, "Message contains '%s', alerting user\n", bbs_username(node->user));
				/* If the message contains our username, ring the bell.
				 * (Most IRC clients also do this for mentions.) */
				if (bbs_ring_bell(node) < 0) {
					res = -1;
					break;
				}
			}
		}
	}

	chat_send(c, NULL, channel, "%s@%d has left %s\n", bbs_username(node->user), node->id, channel);
	return res;
}

static int irc_client_exec(struct bbs_node *node, const char *args)
{
	char buf[84];
	char *channel, *client;
	struct participant *p;
	int res;

	if (strlen_zero(args)) {
		bbs_error("Must specify a client name to use (syntax: client,channel)\n");
		return 0; /* Don't disconnect the node */
	}

	safe_strncpy(buf, args, sizeof(buf));
	channel = buf;
	client = strsep(&channel, ",");

	if (strlen_zero(channel) || strlen_zero(client)) {
		bbs_error("Must specify a client and channel (syntax: client,channel)\n");
		return 0;
	}

	p = join_client(node, client);
	if (!p) {
		return 0;
	}

	p->channel = channel;
	res = participant_relay(node, p, channel);
	leave_client(p->client, p);
	return res;
}

static int load_module(void)
{
	int res;

	if (load_config()) {
		return -1;
	}
	irc_log_callback(__client_log); /* Set up logging */
	res = bbs_register_door("irc", irc_client_exec);
	if (!res) {
		/* Start the clients now, unless the BBS is still starting */
		if (bbs_is_fully_started()) {
			start_clients();
		} else {
			bbs_register_startup_callback(start_clients);
		}
	}
	return res;
}

static int unload_module(void)
{
	struct client *client;
	struct participant *p;

	RWLIST_WRLOCK(&clients);
	unloading = 1;

	while ((client = RWLIST_REMOVE_HEAD(&clients, entry))) {
		irc_client_destroy(client->client);
		/* If there are any clients still connected, boot them */
		while ((p = RWLIST_REMOVE_HEAD(&client->participants, entry))) {
			/* XXX Because the usecount will be positive if clients are being used, the handling to remove participants may be kind of moot */
			/* Remove from list, but don't actually free the participant itself. Each node will do that as it leaves. */
			close(p->chatpipe[1]); /* Close write end of pipe to kick the node from the client */
			p->chatpipe[1] = -1;
		}
		pthread_cancel(client->thread); /* Kill the relay thread for this client, if it hasn't already exited by now. */
		bbs_pthread_join(client->thread, NULL);
		if (client->logfile) {
			fclose(client->logfile);
		}
		free(client);
	}
	RWLIST_UNLOCK(&clients);

	return bbs_unregister_door("irc");
}

BBS_MODULE_INFO_STANDARD("Internet Relay Chat Client");
