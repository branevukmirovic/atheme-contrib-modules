/*
 * Copyright (c) 2005-2006 William Pitcock, et al.
 * Rights to this code are as documented in doc/LICENSE.
 *
 * This file contains functionality which implements the OService MODEALL
 * command.
 */

#include "atheme-compat.h"

static void
set_channel_mode(service_t *s, channel_t *c, int modeparc, char *modeparv[])
{
	channel_mode(s->me, c, modeparc, modeparv);
}

static void
os_cmd_modeall(sourceinfo_t *si, int parc, char *parv[])
{
	char *mode = parv[0];
	channel_t *c;
	int modeparc;
	char *modeparv[256];
	mowgli_patricia_iteration_state_t state;
	unsigned int count = 0;

        if (!mode)
        {
                command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "MODEALL");
                command_fail(si, fault_needmoreparams, _("Syntax: MODEALL <parameters>"));
                return;
        }

	modeparc = sjtoken(mode, ' ', modeparv);

	MOWGLI_PATRICIA_FOREACH(c, &state, chanlist)
	{
		set_channel_mode(si->service, c, modeparc, modeparv);
		count++;
	}

	command_success_nodata(si, _("Set modes \2%s\2 on \2%u\2 channels."), mode, count);
	wallops("\2%s\2 is using MODEALL (set: \2%s\2)",
		get_oper_name(si), mode);
	logcommand(si, CMDLOG_ADMIN, "MODEALL: \2%s\2", mode);
}

static command_t os_modeall = {
	.name           = "MODEALL",
	.desc           = N_("Changes modes on all channels."),
	.access         = PRIV_OMODE,
	.maxparc        = 2,
	.cmd            = &os_cmd_modeall,
	.help           = { .path = "contrib/os_modeall" },
};

static void
mod_init(module_t *const restrict m)
{
        service_named_bind_command("operserv", &os_modeall);
}

static void
mod_deinit(const module_unload_intent_t intent)
{
	service_named_unbind_command("operserv", &os_modeall);
}

SIMPLE_DECLARE_MODULE_V1("contrib/os_modeall", MODULE_UNLOAD_CAPABILITY_OK)
