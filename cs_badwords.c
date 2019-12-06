/*
 * Copyright (c) 2005 William Pitcock <nenolod -at- nenolod.net>
 * Rights to this code are as documented in doc/LICENSE.
 */

#include "atheme-compat.h"

struct badword_ {
	char *badword;
	time_t add_ts;
	char *creator;
	char *channel;
	char *action;
	mowgli_node_t node;
};

typedef struct badword_ badword_t;

static mowgli_patricia_t **cs_set_cmdtree = NULL;

static inline mowgli_list_t *
badwords_list_of(mychan_t *mc)
{
	mowgli_list_t *l;

	return_val_if_fail(mc != NULL, NULL);

	l = privatedata_get(mc, "badword:list");
	if (l != NULL)
		return l;

	l = mowgli_list_create();
	privatedata_set(mc, "badword:list", l);

	return l;
}

static void
write_badword_db(database_handle_t *db)
{
	mowgli_node_t *n;
	mychan_t *mc;
	mowgli_patricia_iteration_state_t state;
	mowgli_list_t *l;

	MOWGLI_PATRICIA_FOREACH(mc, &state, mclist)
	{
		l = badwords_list_of(mc);

		if (l == NULL)
			return;

		MOWGLI_ITER_FOREACH(n, l->head)
		{
			badword_t *bw = n->data;

			db_start_row(db, "BW");
			db_write_word(db, bw->badword);
			db_write_time(db, bw->add_ts);
			db_write_word(db, bw->creator);
			db_write_word(db, bw->channel);
			db_write_word(db, bw->action);
			db_commit_row(db);
		}
	}
}

static void
db_h_bw(database_handle_t *db, const char *type)
{
	mychan_t *mc;
	mowgli_patricia_iteration_state_t state;
	mowgli_list_t *l;

	const char *badword = db_sread_word(db);
	time_t add_ts = db_sread_time(db);
	const char *creator = db_sread_word(db);
	const char *channel = db_sread_word(db);
	const char *action = db_sread_word(db);

	MOWGLI_PATRICIA_FOREACH(mc, &state, mclist)
	{
		if (irccasecmp(mc->name, channel))
			continue;

		l = badwords_list_of(mc);

		badword_t *bw = smalloc(sizeof(badword_t));

		bw->badword = sstrdup(badword);
		bw->add_ts = add_ts;
		bw->creator = sstrdup(creator);
		bw->channel = sstrdup(channel);
		bw->action = sstrdup(action);

		mowgli_node_add(bw, &bw->node, l);
	}
}

static void
on_channel_message(hook_cmessage_data_t *data)
{
	badword_t *bw;
	mowgli_node_t *n;
	mowgli_list_t *l;

	mychan_t *mc = mychan_from(data->c);

	if (mc == NULL)
		return;

	if (metadata_find(mc, "blockbadwords") == NULL)
		return;

	l = badwords_list_of(mc);
	if (MOWGLI_LIST_LENGTH(l) == 0)
		return;

	char *kickstring = "Foul language is prohibited here.";

	if (data != NULL && data->msg != NULL)
	{
		MOWGLI_ITER_FOREACH(n, l->head)
		{
			bw = n->data;
			chanuser_t *cu;
			cu = chanuser_find(data->c, data->u);
			if (cu == NULL)
				return;
			if ((metadata_find(mc, "blockbadwordsops") != NULL) && ((CSTATUS_OP | CSTATUS_PROTECT | CSTATUS_OWNER) & cu->modes))
				return;

			if (!match(bw->badword, data->msg))
			{
				if (!strcasecmp("KICKBAN", bw->action))
				{
					char hostbuf[BUFSIZE];

					hostbuf[0] = '\0';

					mowgli_strlcat(hostbuf, "*!*@", BUFSIZE);
					mowgli_strlcat(hostbuf, data->u->vhost, BUFSIZE);

					modestack_mode_param(chansvs.nick, data->c, MTYPE_ADD, 'b', hostbuf);
					chanban_add(data->c, hostbuf, 'b');
					kick(chansvs.me->me, data->c, data->u, kickstring);
					return;
				}
				else if (!strcasecmp("KICK", bw->action))
				{
					kick(chansvs.me->me, data->c, data->u, kickstring);
					return;
				}
				else if (!strcasecmp("WARN", bw->action))
				{
					notice(chansvs.nick, data->u->nick, "Foul language is prohibited on %s.", data->c->name);
					return;
				}
				else if (!strcasecmp("QUIET", bw->action))
				{
					char hostbuf[BUFSIZE];

					hostbuf[0] = '\0';

					mowgli_strlcat(hostbuf, "*!*@", BUFSIZE);
					mowgli_strlcat(hostbuf, data->u->vhost, BUFSIZE);

					modestack_mode_param(chansvs.nick, data->c, MTYPE_ADD, 'q', hostbuf);
					chanban_add(data->c, hostbuf, 'q');
					return;
				}
				else if (!strcasecmp("BAN", bw->action))
				{
					char hostbuf[BUFSIZE];

					hostbuf[0] = '\0';

					mowgli_strlcat(hostbuf, "*!*@", BUFSIZE);
					mowgli_strlcat(hostbuf, data->u->vhost, BUFSIZE);

					modestack_mode_param(chansvs.nick, data->c, MTYPE_ADD, 'b', hostbuf);
					chanban_add(data->c, hostbuf, 'b');
					return;
				}
			}
		}
	}
}

static void
cs_cmd_badwords(sourceinfo_t *si, int parc, char *parv[])
{
	char *channel = parv[0];
	char *command = parv[1];
	char *word = parv[2];
	char *action = parv[3];
	mychan_t *mc;
	mowgli_node_t *n, *tn;
	badword_t *bw;
	mowgli_list_t *l;

	if (!channel || !command)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SET BADWORDS");
		command_fail(si, fault_needmoreparams, _("Syntax: BADWORDS <#channel> ADD|DEL|LIST [badword] [action]"));
		return;
	}

	if (!(mc = mychan_find(channel)))
	{
		command_fail(si, fault_nosuch_target, STR_IS_NOT_REGISTERED, channel);
		return;
	}

	l = badwords_list_of(mc);

	if (!strcasecmp("ADD", command))
	{

		if (!word || !action)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "BADWORDS");
			command_fail(si, fault_needmoreparams, _("Syntax: BADWORDS <#channel> ADD <badword> <action>"));
			return;
		}

		if (!chanacs_source_has_flag(mc, si, CA_SET))
		{
			command_fail(si, fault_noprivs, STR_NOT_AUTHORIZED);
			return;
		}

		if (!strcasecmp("KICK", action) || !strcasecmp("KICKBAN", action) || !strcasecmp("WARN", action) ||
		    !strcasecmp("BAN", action) || (!strcasecmp("QUIET", action) && ircd != NULL &&
		    strchr(ircd->ban_like_modes, 'q')))
		{
			if (l != NULL)
			{
				MOWGLI_ITER_FOREACH(n, l->head)
				{
					bw = n->data;

					if (!irccasecmp(bw->badword, word))
					{
						command_success_nodata(si, _("\2%s\2 has already been entered "
						                             "into the bad word list."), word);
						return;
					}
				}
			}

			bw = smalloc(sizeof(badword_t));
			bw->add_ts = CURRTIME;;
			bw->creator = sstrdup(get_source_name(si));
			bw->channel = sstrdup(mc->name);
			bw->badword = sstrdup(word);
			bw->action = sstrdup(action);
			mowgli_node_add(bw, &bw->node, l);

			command_success_nodata(si, _("You have added \2%s\2 as a bad word."), word);
			logcommand(si, CMDLOG_SET, "BADWORDS:ADD: \2%s\2 \2%s\2 \2%s\2", channel, word, action);
		}
		else
		{
			command_fail(si, fault_badparams, _("Invalid action given."));
			return;
		}
	}
	else if (!strcasecmp("DEL", command))
	{

		if (!word)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "BADWORDS");
			command_fail(si, fault_needmoreparams, _("Syntax: BADWORDS <#channel> DEL <badword>"));
			return;
		}

		if (!chanacs_source_has_flag(mc, si, CA_SET))
		{
			command_fail(si, fault_noprivs, STR_NOT_AUTHORIZED);
			return;
		}

		if (l == NULL)
		{
			command_fail(si, fault_nosuch_target, _("There are no badwords set in this channel."));
			return;
		}

		MOWGLI_ITER_FOREACH_SAFE(n, tn, l->head)
		{
			bw = n->data;

			if (!irccasecmp(bw->badword, word))
			{
				logcommand(si, CMDLOG_SET, "BADWORDS:DEL: \2%s\2 \2%s\2", mc->name, bw->badword);
				command_success_nodata(si, _("Bad word \2%s\2 has been deleted."), bw->badword);

				mowgli_node_delete(&bw->node, l);

				sfree(bw->creator);
				sfree(bw->channel);
				sfree(bw->badword);
				sfree(bw->action);
				sfree(bw);

				return;
			}
		}

		command_success_nodata(si, _("Word \2%s\2 not found in bad word database."), word);
	}
	else if (!strcasecmp("LIST", command))
	{
		char buf[BUFSIZE];
		struct tm tm;

		if (!chanacs_source_has_flag(mc, si, CA_ACLVIEW))
		{
			command_fail(si, fault_noprivs, STR_NOT_AUTHORIZED);
			return;
		}

		if (l == NULL)
		{
			command_fail(si, fault_nosuch_target, _("There are no badwords set in this channel."));
			return;
		}

		MOWGLI_ITER_FOREACH(n, l->head)
		{
			bw = n->data;

			tm = *localtime(&bw->add_ts);
			strftime(buf, BUFSIZE, TIME_FORMAT, &tm);
			command_success_nodata(si, _("Word: \2%s\2, Action: \2%s\2 (%s - %s)"),
			                             bw->badword, bw->action, bw->creator, buf);
		}

		command_success_nodata(si, "End of list.");
		logcommand(si, CMDLOG_GET, "BADWORDS:LIST");
	}
	else
	{
		command_fail(si, fault_needmoreparams, STR_INVALID_PARAMS, "BADWORDS");
		command_fail(si, fault_needmoreparams, _("Syntax: BADWORDS <#channel> ADD|DEL|LIST [badword] [action]"));
		return;
	}
}

static void
cs_set_cmd_blockbadwords(sourceinfo_t *si, int parc, char *parv[])
{
	mychan_t *mc;

	if (!(mc = mychan_find(parv[0])))
	{
		command_fail(si, fault_nosuch_target, STR_IS_NOT_REGISTERED, parv[0]);
		return;
	}

	if (!parv[1])
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SET BLOCKBADWORDS");
		return;
	}

	if (!chanacs_source_has_flag(mc, si, CA_SET))
	{
		command_fail(si, fault_noprivs, STR_NOT_AUTHORIZED);
		return;
	}

	if (!strcasecmp("ON", parv[1]))
	{
		metadata_t *md = metadata_find(mc, "blockbadwords");

		if (md)
		{
			command_fail(si, fault_nochange, _("The \2%s\2 flag is already set for channel \2%s\2."),
			                                   "BLOCKBADWORDS", mc->name);
			return;
		}

		metadata_add(mc, "blockbadwords", "on");
		logcommand(si, CMDLOG_SET, "SET:BLOCKBADWORDS:ON: \2%s\2", mc->name);
		command_success_nodata(si, _("The \2%s\2 flag has been set for channel \2%s\2."),
		                             "BLOCKBADWORDS", mc->name);
	}
	else if (!strcasecmp("OFF", parv[1]))
	{
		metadata_t *md = metadata_find(mc, "blockbadwords");

		if (!md)
		{
			command_fail(si, fault_nochange, _("The \2%s\2 flag is not set for channel \2%s\2."),
			                                   "BLOCKBADWORDS", mc->name);
			return;
		}

		metadata_delete(mc, "blockbadwords");
		logcommand(si, CMDLOG_SET, "SET:BLOCKBADWORDS:OFF: \2%s\2", mc->name);
		command_success_nodata(si, _("The \2%s\2 flag has been removed for channel \2%s\2."),
		                             "BLOCKBADWORDS", mc->name);
	}
	else
		command_fail(si, fault_badparams, STR_INVALID_PARAMS, "BLOCKBADWORDS");
}

static void
cs_set_cmd_blockbadwordsops(sourceinfo_t *si, int parc, char *parv[])
{
	mychan_t *mc;

	if (!(mc = mychan_find(parv[0])))
	{
		command_fail(si, fault_nosuch_target, STR_IS_NOT_REGISTERED, parv[0]);
		return;
	}

	if (!parv[1])
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SET BLOCKBADWORDSOPS");
		return;
	}

	if (!chanacs_source_has_flag(mc, si, CA_SET))
	{
		command_fail(si, fault_noprivs, STR_NOT_AUTHORIZED);
		return;
	}

	if (!strcasecmp("ON", parv[1]))
	{
		metadata_t *md = metadata_find(mc, "blockbadwordsops");

		if (md)
		{
			command_fail(si, fault_nochange, _("The \2%s\2 flag is already set for channel \2%s\2."),
			                                   "BLOCKBADWORDSOPS", mc->name);
			return;
		}

		metadata_add(mc, "blockbadwordsops", "on");
		logcommand(si, CMDLOG_SET, "SET:BLOCKBADWORDSOPS:ON: \2%s\2", mc->name);
		command_success_nodata(si, _("The \2%s\2 flag has been set for channel \2%s\2."),
		                             "BLOCKBADWORDSOPS", mc->name);
	}
	else if (!strcasecmp("OFF", parv[1]))
	{
		metadata_t *md = metadata_find(mc, "blockbadwordsops");

		if (!md)
		{
			command_fail(si, fault_nochange, _("The \2%s\2 flag is not set for channel \2%s\2."),
			                                   "BLOCKBADWORDSOPS", mc->name);
			return;
		}

		metadata_delete(mc, "blockbadwordsops");
		logcommand(si, CMDLOG_SET, "SET:BLOCKBADWORDSOPS:OFF: \2%s\2", mc->name);
		command_success_nodata(si, _("The \2%s\2 flag has been removed for channel \2%s\2."),
		                             "BLOCKBADWORDSOPS", mc->name);
	}
	else
		command_fail(si, fault_badparams, STR_INVALID_PARAMS, "BLOCKBADWORDSOPS");
}

static command_t cs_badwords = {
	.name           = "BADWORDS",
	.desc           = N_("Manage the list of channel bad words."),
	.access         = AC_AUTHENTICATED,
	.maxparc        = 4,
	.cmd            = &cs_cmd_badwords,
	.help           = { .path = "contrib/badwords" },
};

static command_t cs_set_blockbadwords = {
	.name           = "BLOCKBADWORDS",
	.desc           = N_("Set whether users can say badwords in channel or not."),
	.access         = AC_NONE,
	.maxparc        = 2,
	.cmd            = &cs_set_cmd_blockbadwords,
	.help           = { .path = "contrib/set_blockbadwords" },
};

static command_t cs_set_blockbadwordsops = {
	.name           = "BLOCKBADWORDSOPS",
	.desc           = N_("Set whether ops can say badwords in channel or not."),
	.access         = AC_NONE,
	.maxparc        = 2,
	.cmd            = &cs_set_cmd_blockbadwordsops,
	.help           = { .path = "contrib/set_blockbadwordsops" },
};

static void
mod_init(module_t *const restrict m)
{
	MODULE_TRY_REQUEST_SYMBOL(m, cs_set_cmdtree, "chanserv/set_core", "cs_set_cmdtree");

	if (!module_find_published("backend/opensex"))
	{
		slog(LG_INFO, "Module %s requires use of the OpenSEX database backend, refusing to load.", m->name);
		m->mflags |= MODFLAG_FAIL;
		return;
	}

	hook_add_event("channel_message");
	hook_add_channel_message(on_channel_message);

	hook_add_db_write(write_badword_db);

	db_register_type_handler("BW", db_h_bw);

	service_named_bind_command("chanserv", &cs_badwords);
	command_add(&cs_set_blockbadwords, *cs_set_cmdtree);
	command_add(&cs_set_blockbadwordsops, *cs_set_cmdtree);
}

static void
mod_deinit(const module_unload_intent_t intent)
{
	hook_del_channel_message(on_channel_message);
	hook_del_db_write(write_badword_db);

	db_unregister_type_handler("BW");

	service_named_unbind_command("chanserv", &cs_badwords);
	command_delete(&cs_set_blockbadwords, *cs_set_cmdtree);
	command_delete(&cs_set_blockbadwordsops, *cs_set_cmdtree);
}

SIMPLE_DECLARE_MODULE_V1("contrib/cs_badwords", MODULE_UNLOAD_CAPABILITY_OK)
