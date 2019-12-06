/*
 * charybdis: A slightly useful ircd.
 * blacklist.c: Manages DNS blacklist entries and lookups
 *
 * Copyright (C) 2016 Atheme Development Group <http://atheme.github.io>
 * Copyright (C) 2006-2008 charybdis development team
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* To configure/use, add a block to the general{} section of your atheme.conf
 * like this:
 *
 * blacklists {
 *	"dnsbl.dronebl.org";
 *	"rbl.efnetrbl.org";
 * };
 */

#include "atheme-compat.h"

#if (CURRENT_ABI_REVISION < 730000)
#  include "conf.h"
#endif

/* A configured DNSBL */
struct Blacklist {
	unsigned int status;	/* If CONF_ILLEGAL, delete when no clients */
	int refcount;
	char host[IRCD_RES_HOSTLEN + 1];
	unsigned int hits;
	time_t lastwarning;
};

/* A lookup in progress for a particular DNSBL for a particular client */
struct BlacklistClient {
	struct Blacklist *blacklist;
	user_t *u;
	mowgli_dns_query_t dns_query;
	mowgli_node_t node;
};

struct dnsbl_exempt_ {
	char *ip;
	time_t exempt_ts;
	char *creator;
	char *reason;
};

typedef struct dnsbl_exempt_ dnsbl_exempt_t;

static mowgli_list_t blacklist_list = { NULL, NULL, 0 };
static mowgli_list_t dnsbl_elist = { NULL, NULL, 0 };

static mowgli_patricia_t **os_set_cmdtree = NULL;
static mowgli_dns_t *dns_base = NULL;
static char *action = NULL;

static inline mowgli_list_t *
dnsbl_queries(user_t *u)
{
	mowgli_list_t *l;

	return_val_if_fail(u != NULL, NULL);

	l = privatedata_get(u, "dnsbl:queries");
	if (l != NULL)
		return l;

	l = mowgli_list_create();
	privatedata_set(u, "dnsbl:queries", l);

	return l;
}

static void
os_cmd_set_dnsblaction(sourceinfo_t *si, int parc, char *parv[])
{
	char *act = parv[0];

	if (!act)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SET DNSBLACTION");
		command_fail(si, fault_needmoreparams, _("Syntax: SET DNSBLACTION <action> <SNOOP|KLINE|NOTIFY|NONE>"));
		return;
	}

	if (!strcasecmp("SNOOP", act) || !strcasecmp("KLINE", act) || !strcasecmp("NOTIFY", act))
	{
		action = sstrdup(act);
		command_success_nodata(si, _("DNSBLACTION successfully set to \2%s\2"), act);
		logcommand(si, CMDLOG_ADMIN, "SET:DNSBLACTION: \2%s\2", act);
	}
	else if (!strcasecmp("NONE", act))
	{
		action = NULL;
		command_success_nodata(si, _("DNSBLACTION successfully set to \2%s\2"), act);
		logcommand(si, CMDLOG_ADMIN, "SET:DNSBLACTION: \2%s\2", act);
	}
	else
		command_fail(si, fault_badparams, _("Invalid action given."));
}

static void
os_cmd_dnsblexempt(sourceinfo_t *si, int parc, char *parv[])
{
	char *command = parv[0];
	char *ip = parv[1];
	char *reason = parv[2];
	mowgli_node_t *n, *tn;
	dnsbl_exempt_t *de;

	if (!command)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "DNSBLEXEMPT");
		command_fail(si, fault_needmoreparams, _("Syntax: DNSBLEXEMPT ADD|DEL|LIST [ip] [reason]"));
		return;
	}

	if (!strcasecmp("ADD", command))
	{

		if (!ip || !reason)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "DNSBLEXEMPT ADD");
			command_fail(si, fault_needmoreparams, _("Syntax: DNSBLEXEMPT ADD <ip> <reason>"));
			return;
		}

		MOWGLI_ITER_FOREACH(n, dnsbl_elist.head)
		{
			de = n->data;

			if (!irccasecmp(de->ip, ip))
			{
				command_success_nodata(si, _("\2%s\2 has already been entered into "
				                             "the DNSBL exempts list."), ip);
				return;
			}
		}

		de = smalloc(sizeof(dnsbl_exempt_t));
		de->exempt_ts = CURRTIME;;
		de->creator = sstrdup(get_source_name(si));
		de->reason = sstrdup(reason);
		de->ip = sstrdup(ip);
		mowgli_node_add(de, mowgli_node_create(), &dnsbl_elist);

		command_success_nodata(si, _("You have added \2%s\2 to the DNSBL exempts list."), ip);
		logcommand(si, CMDLOG_ADMIN, "DNSBL:EXEMPT:ADD: \2%s\2 \2%s\2", ip, reason);
	}
	else if (!strcasecmp("DEL", command))
	{

		if (!ip)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "DNSBLEXEMPT DEL");
			command_fail(si, fault_needmoreparams, _("Syntax: DNSBLEXEMPT DEL <ip>"));
			return;
		}

		MOWGLI_ITER_FOREACH_SAFE(n, tn, dnsbl_elist.head)
		{
			de = n->data;

			if (!irccasecmp(de->ip, ip))
			{
				logcommand(si, CMDLOG_SET, "DNSBL:EXEMPT:DEL: \2%s\2", de->ip);
				command_success_nodata(si, _("DNSBL Exempt IP \2%s\2 has been deleted."), de->ip);

				mowgli_node_delete(n, &dnsbl_elist);
				sfree(de->creator);
				sfree(de->reason);
				sfree(de->ip);
				sfree(de);

				return;
			}
		}

		command_success_nodata(si, _("IP \2%s\2 not found in DNSBL Exempt database."), ip);
	}
	else if (!strcasecmp("LIST", command))
	{
		char buf[BUFSIZE];
		struct tm tm;

		MOWGLI_ITER_FOREACH(n, dnsbl_elist.head)
		{
			de = n->data;

			tm = *localtime(&de->exempt_ts);
			strftime(buf, BUFSIZE, TIME_FORMAT, &tm);
			command_success_nodata(si, _("IP: \2%s\2, Reason: \2%s\2 (%s - %s)"),
			                             de->ip, de->reason, de->creator, buf);
		}

		command_success_nodata(si, _("End of list."));
		logcommand(si, CMDLOG_GET, "DNSBL:EXEMPT:LIST");
	}
	else
	{
		command_fail(si, fault_needmoreparams, STR_INVALID_PARAMS, "DNSBLEXEMPT");
		command_fail(si, fault_needmoreparams, _("Syntax: DNSBLEXEMPT ADD|DEL|LIST [ip] [reason]"));
	}
}

static void
dnsbl_hit(user_t *u, struct Blacklist *blptr)
{
	service_t *const svs = service_find("operserv");

	if (!strcasecmp("SNOOP", action))
	{
		slog(LG_INFO, "DNSBL: \2%s\2!%s@%s [%s] is listed in DNS Blacklist %s.",
		              u->nick, u->user, u->host, u->gecos, blptr->host);

		/* abort_blacklist_queries(u); */
	}
	else if (!strcasecmp("NOTIFY", action))
	{
		slog(LG_INFO, "DNSBL: \2%s\2!%s@%s [%s] is listed in DNS Blacklist %s.",
		              u->nick, u->user, u->host, u->gecos, blptr->host);

		notice(svs->nick, u->nick, "Your IP address %s is listed in DNS Blacklist %s", u->ip, blptr->host);

		/* abort_blacklist_queries(u); */
	}
	else if (!strcasecmp("KLINE", action))
	{
		if (! (u->flags & UF_KLINESENT)) {
			slog(LG_INFO, "DNSBL: k-lining \2%s\2!%s@%s [%s] who is listed in DNS Blacklist %s.",
			              u->nick, u->user, u->host, u->gecos, blptr->host);

			/* abort_blacklist_queries(u); */
			notice(svs->nick, u->nick, "Your IP address %s is listed in DNS Blacklist %s", u->ip, blptr->host);
			kline_add(u->user, u->host, "Banned (DNS Blacklist)", 86400, "*");
			u->flags |= UF_KLINESENT;
		}
	}
}

static void
blacklist_dns_callback(mowgli_dns_reply_t *reply, int result, void *vptr)
{
	struct BlacklistClient *blcptr = (struct BlacklistClient *) vptr;
	int listed = 0;
	mowgli_list_t *l;

	if (blcptr == NULL)
		return;

	if (blcptr->u == NULL)
	{
		sfree(blcptr);
		return;
	}

	if (reply != NULL)
	{
		/* only accept 127.x.y.z as a listing */
		if (reply->addr.addr.ss_family == AF_INET &&
				!memcmp(&((struct sockaddr_in *)&reply->addr.addr)->sin_addr, "\177", 1))
			listed++;
		else if (blcptr->blacklist->lastwarning + 3600 < CURRTIME)
		{
			slog(LG_DEBUG, "Garbage reply from blacklist %s", blcptr->blacklist->host);
			blcptr->blacklist->lastwarning = CURRTIME;
		}
	}

	/* they have a blacklist entry for this client */
	if (listed)
	{
		dnsbl_hit(blcptr->u, blcptr->blacklist);
		return;
	}

	l = dnsbl_queries(blcptr->u);
	mowgli_node_delete(&blcptr->node, l);

	sfree(blcptr);
}

/* XXX: no IPv6 implementation, not to concerned right now though. */
static void
initiate_blacklist_dnsquery(struct Blacklist *blptr, user_t *u)
{
	struct BlacklistClient *blcptr = smalloc(sizeof(struct BlacklistClient));
	char buf[IRCD_RES_HOSTLEN + 1];
	unsigned int ip[4];
	mowgli_list_t *l;

	blcptr->blacklist = blptr;
	blcptr->u = u;

	blcptr->dns_query.ptr = blcptr;
	blcptr->dns_query.callback = blacklist_dns_callback;

	/* A sscanf worked fine for chary for many years, it'll be fine here */
	sscanf(u->ip, "%u.%u.%u.%u", &ip[3], &ip[2], &ip[1], &ip[0]);

	/* becomes 2.0.0.127.torbl.ahbl.org or whatever */
	snprintf(buf, sizeof buf, "%u.%u.%u.%u.%s", ip[0], ip[1], ip[2], ip[3], blptr->host);

	mowgli_dns_gethost_byname(dns_base, buf, &blcptr->dns_query, MOWGLI_DNS_T_A);

	l = dnsbl_queries(u);
	mowgli_node_add(blcptr, &blcptr->node, l);
	blptr->refcount++;
}

static void
lookup_blacklists(user_t *u)
{
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, blacklist_list.head)
	{
		struct Blacklist *blptr = (struct Blacklist *) n->data;
		blptr->status = 0;

		if (u == NULL)
			return;

		initiate_blacklist_dnsquery(blptr, u);
	}
}

static void
os_cmd_dnsblscan(sourceinfo_t *si, int parc, char *parv[])
{
	char *user = parv[0];
	user_t *u;

	if (!user)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "DNSBLSCAN");
		command_fail(si, fault_needmoreparams, _("Syntax: DNSBLSCAN <nickname>"));
		return;
	}

	if ((u = user_find_named(user)))
	{
		lookup_blacklists(u);
		logcommand(si, CMDLOG_ADMIN, "DNSBLSCAN: %s", user);
		command_success_nodata(si, _("%s has been scanned."), user);
	}
	else
		command_fail(si, fault_badparams, _("User %s is not on the network, you cannot scan them."), user);
}

/* private interfaces */
static struct Blacklist *
find_blacklist(char *name)
{
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, blacklist_list.head)
	{
		struct Blacklist *blptr = (struct Blacklist *) n->data;

		if (!strcasecmp(blptr->host, name))
			return blptr;
	}

	return NULL;
}

/* public interfaces */
static struct Blacklist *
new_blacklist(char *name)
{
	struct Blacklist *blptr;

	if (name == NULL)
		return NULL;

	blptr = find_blacklist(name);

	if (blptr == NULL)
	{
		blptr = smalloc(sizeof(struct Blacklist));
		mowgli_node_add(blptr, mowgli_node_create(), &blacklist_list);
	}

	mowgli_strlcpy(blptr->host, name, IRCD_RES_HOSTLEN + 1);
	blptr->lastwarning = 0;

	return blptr;
}

/* This appears to be unnecessary on Atheme and only causes crashes so #if 0
 * it out, at least for now. --jdhore
 */
#if 0
static void
abort_blacklist_queries(user_t *u)
{
	mowgli_node_t *n, *tn;
	mowgli_list_t *l;
	struct BlacklistClient *blcptr;

	if (u == NULL)
		return;

	l = dnsbl_queries(u);

	MOWGLI_ITER_FOREACH_SAFE(n, tn, l->head)
	{
		blcptr = n->data;
		mowgli_node_delete(&blcptr->node, l);
		unref_blacklist(blcptr->blacklist);
		mowgli_dns_delete_query(dns_base, &blcptr->dns_query);
		sfree(blcptr);
	}
}
#endif

static void
destroy_blacklists(void)
{
	mowgli_node_t *n, *tn;
	struct Blacklist *blptr;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, blacklist_list.head)
	{
		blptr = n->data;
		blptr->hits = 0; /* keep it simple and consistent */
		sfree(n->data);
		mowgli_node_delete(n, &blacklist_list);
		mowgli_node_free(n);
	}
}

static int
dnsbl_config_handler(mowgli_config_file_entry_t *ce)
{
	mowgli_config_file_entry_t *cce;

	MOWGLI_ITER_FOREACH(cce, ce->entries)
	{
		char *line = sstrdup(cce->varname);
		new_blacklist(line);
		sfree(line);
	}

	return 0;
}

static void
dnsbl_config_purge(void *unused)
{
	destroy_blacklists();
}

static void
check_dnsbls(hook_user_nick_t *data)
{
	user_t *u = data->u;
	mowgli_node_t *n;

	if (!u)
		return;

	if (is_internal_client(u))
		return;

	if (!action)
		return;

	MOWGLI_ITER_FOREACH(n, dnsbl_elist.head)
	{
		dnsbl_exempt_t *de = n->data;

		if (!irccasecmp(de->ip, u->ip))
			return;
	}

	lookup_blacklists(u);
}

static void
osinfo_hook(sourceinfo_t *si)
{
	mowgli_node_t *n;

	if (action)
		command_success_nodata(si, _("Action taken when a user is an a DNSBL: %s"), action);
	else
		command_success_nodata(si, _("Action taken when a user is an a DNSBL: %s"), _("None"));

	MOWGLI_ITER_FOREACH(n, blacklist_list.head)
	{
		struct Blacklist *blptr = (struct Blacklist *) n->data;

		command_success_nodata(si, _("Using Blacklist: %s"), blptr->host);
	}
}

static void
write_dnsbl_exempt_db(database_handle_t *db)
{
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, dnsbl_elist.head)
	{
		dnsbl_exempt_t *de = n->data;

		db_start_row(db, "BW");
		db_write_word(db, de->ip);
		db_write_time(db, de->exempt_ts);
		db_write_word(db, de->creator);
		db_write_word(db, de->reason);
		db_commit_row(db);
	}
}

static void
db_h_ble(database_handle_t *db, const char *type)
{
	const char *ip = db_sread_word(db);
	time_t exempt_ts = db_sread_time(db);
	const char *creator = db_sread_word(db);
	const char *reason = db_sread_word(db);

	dnsbl_exempt_t *de = smalloc(sizeof(dnsbl_exempt_t));

	de->ip = sstrdup(ip);
	de->exempt_ts = exempt_ts;
	de->creator = sstrdup(creator);
	de->reason = sstrdup(reason);

	mowgli_node_add(de, mowgli_node_create(), &dnsbl_elist);
}

static command_t os_set_dnsblaction = {
	"DNSBLACTION",
	N_("Changes what happens to a user when they hit a DNSBL."),
	PRIV_USER_ADMIN,
	1,
	&os_cmd_set_dnsblaction,
	{ .path = "contrib/set_dnsblaction" },
};

static command_t os_dnsblexempt = {
	"DNSBLEXEMPT",
	N_("Manage the list of IP's exempt from DNSBL checking."),
	PRIV_USER_ADMIN,
	3,
	&os_cmd_dnsblexempt,
	{ .path = "contrib/dnsblexempt" },
};

static command_t os_dnsblscan = {
	"DNSBLSCAN",
	N_("Manually scan if a user is in a DNSBL."),
	PRIV_USER_ADMIN,
	1,
	&os_cmd_dnsblscan,
	{ .path = "contrib/dnsblscan" },
};

static void
mod_init(module_t *m)
{
	MODULE_CONFLICT(m, "proxyscan/dnsbl");

	MODULE_TRY_REQUEST_SYMBOL(m, os_set_cmdtree, "operserv/set", "os_set_cmdtree");

	if (!module_find_published("backend/opensex"))
	{
		(void) slog(LG_ERROR, "Module %s requires use of the OpenSEX database backend, refusing to load.", m->name);
		m->mflags |= MODFLAG_FAIL;
		return;
	}

	if (! (dns_base = mowgli_dns_create(base_eventloop, MOWGLI_DNS_TYPE_ASYNC)))
	{
		(void) slog(LG_ERROR, "%s: failed to create Mowgli DNS resolver object", m->name);
		m->mflags |= MODFLAG_FAIL;
		return;
	}

	hook_add_db_write(write_dnsbl_exempt_db);

	db_register_type_handler("BLE", db_h_ble);

	service_named_bind_command("operserv", &os_dnsblexempt);
	service_named_bind_command("operserv", &os_dnsblscan);

	hook_add_event("config_purge");
	hook_add_config_purge(dnsbl_config_purge);

	hook_add_event("user_add");
	hook_add_user_add(check_dnsbls);

	hook_add_event("operserv_info");
	hook_add_operserv_info(osinfo_hook);

	add_dupstr_conf_item("dnsbl_action", &conf_gi_table, 0, &action, NULL);
	add_conf_item("BLACKLISTS", &conf_gi_table, dnsbl_config_handler);
	command_add(&os_set_dnsblaction, *os_set_cmdtree);
}

static void
mod_deinit(module_unload_intent_t intent)
{
	(void) mowgli_dns_destroy(dns_base);

	hook_del_db_write(write_dnsbl_exempt_db);
	hook_del_user_add(check_dnsbls);
	hook_del_config_purge(dnsbl_config_purge);
	hook_del_operserv_info(osinfo_hook);

	db_unregister_type_handler("BLE");

	del_conf_item("dnsbl_action", &conf_gi_table);
	del_conf_item("BLACKLISTS", &conf_gi_table);
	command_delete(&os_set_dnsblaction, *os_set_cmdtree);
	service_named_unbind_command("operserv", &os_dnsblexempt);
	service_named_unbind_command("operserv", &os_dnsblscan);
}

SIMPLE_DECLARE_MODULE_V1("contrib/dnsbl", MODULE_UNLOAD_CAPABILITY_OK)
