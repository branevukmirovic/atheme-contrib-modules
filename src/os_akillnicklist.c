/*
 * Copyright (c) 2016 Atheme Development Group <http://atheme.github.io>
 * Copyright (c) 2010 William Pitcock, et al.
 * The rights to this code are as documented under doc/LICENSE.
 *
 * Automatically AKILL a list of clients, given their operating parameters.
 *
 * Basically this builds a keyword patricia.  O(NICKLEN) lookups at the price
 * of a longer startup process.
 *
 * Configuration.
 * ==============
 *
 * This module adds a new block to the config file:
 *
 *     nicklists {
 *         all = "/home/nenolod/atheme-production.hg4576/etc/nicklists/azn.txt";
 *         nick = "/home/nenolod/atheme-production.hg4576/etc/nicklists/bottler-nicks.txt";
 *         user = "/home/nenolod/atheme-production.hg4576/etc/nicklists/bottler-users.txt";
 *         real = "/home/nenolod/atheme-production.hg4576/etc/nicklists/bottler-gecos.txt";
 *     };
 *
 * You can add multiple all, nick, user and real entries.  The entries will be merged.
 * I would also like to say: fuck you GNAA, you guys need to go play in fucking traffic.
 * Thanks for reading my crappy docs, and have a nice day.
 */

#include "atheme-compat.h"

static mowgli_patricia_t *akillalllist = NULL;
static mowgli_patricia_t *akillnicklist = NULL;
static mowgli_patricia_t *akilluserlist = NULL;
static mowgli_patricia_t *akillreallist = NULL;

static mowgli_list_t conft = { NULL, NULL, 0 };

static void
add_contents_of_file_to_list(const char *filename, mowgli_patricia_t *list)
{
	char value[BUFSIZE];
	FILE *f;

	f = fopen(filename, "r");
	if (!f)
		return;

	while (fgets(value, BUFSIZE, f) != NULL)
	{
		strip(value);

		if (!*value)
			continue;

		mowgli_patricia_add(list, value, (void *) 0x1);
	}

	fclose(f);
}

static int
nicklist_config_handler_all(mowgli_config_file_entry_t *entry)
{
	add_contents_of_file_to_list(entry->vardata, akillalllist);

	return 0;
}

static int
nicklist_config_handler_nick(mowgli_config_file_entry_t *entry)
{
	add_contents_of_file_to_list(entry->vardata, akillnicklist);

	return 0;
}

static int
nicklist_config_handler_user(mowgli_config_file_entry_t *entry)
{
	add_contents_of_file_to_list(entry->vardata, akilluserlist);

	return 0;
}

static int
nicklist_config_handler_real(mowgli_config_file_entry_t *entry)
{
	add_contents_of_file_to_list(entry->vardata, akillreallist);

	return 0;
}

static void
aknl_nickhook(hook_user_nick_t *data)
{
	return_if_fail(data != NULL);

	if (! data->u)
		return;

	user_t *const u = data->u;

	if (is_internal_client(u))
		return;

	if (is_autokline_exempt(u))
		return;

	if (u->flags & UF_KLINESENT)
		return;

	const char *username = u->user;

	if (*username == '~')
		username++;

	bool doit = false;

	if (mowgli_patricia_retrieve(akillnicklist, u->nick))
		doit = true;
	else if (mowgli_patricia_retrieve(akilluserlist, username))
		doit = true;
	else if (mowgli_patricia_retrieve(akillreallist, u->gecos))
		doit = true;
	else if (mowgli_patricia_retrieve(akillalllist, u->nick))
		doit = true;
	else if (mowgli_patricia_retrieve(akillalllist, username))
		doit = true;
	else if (mowgli_patricia_retrieve(akillalllist, u->gecos))
		doit = true;
	else
		return;

	slog(LG_INFO, "AKNL: k-lining \2%s\2!%s@%s [%s] due to appearing to be a possible spambot", u->nick, u->user, u->host, u->gecos);
	kline_add(u->user, u->host, "Possible spambot", 86400, "*");
	u->flags |= UF_KLINESENT;
}

static void
mod_init(module_t *m)
{
	add_subblock_top_conf("NICKLISTS", &conft);
	add_conf_item("ALL", &conft, nicklist_config_handler_all);
	add_conf_item("NICK", &conft, nicklist_config_handler_nick);
	add_conf_item("USER", &conft, nicklist_config_handler_user);
	add_conf_item("REAL", &conft, nicklist_config_handler_real);

	akillalllist = mowgli_patricia_create(strcasecanon);
	akillnicklist = mowgli_patricia_create(strcasecanon);
	akilluserlist = mowgli_patricia_create(strcasecanon);
	akillreallist = mowgli_patricia_create(strcasecanon);

	hook_add_event("user_add");
	hook_add_user_add(aknl_nickhook);
	hook_add_event("user_nickchange");
	hook_add_user_nickchange(aknl_nickhook);
}

static void
mod_deinit(module_unload_intent_t intent)
{
	hook_del_user_add(aknl_nickhook);
	hook_del_user_nickchange(aknl_nickhook);

	del_conf_item("ALL", &conft);
	del_conf_item("NICK", &conft);
	del_conf_item("USER", &conft);
	del_conf_item("REAL", &conft);
	del_top_conf("NICKLISTS");
}

SIMPLE_DECLARE_MODULE_V1("contrib/os_akillnicklist", MODULE_UNLOAD_CAPABILITY_OK)
