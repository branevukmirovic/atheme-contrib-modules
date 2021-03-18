/*
 * Copyright (C) 2018 Atheme Development Group (https://atheme.github.io/)
 * Rights to this code are documented in doc/LICENSE.
 *
 * Restrict authentication methods by name or by host.
 */

#include "atheme-compat.h"

#if (CURRENT_ABI_REVISION >= 730000)

struct blacklist_entry
{
	mowgli_node_t   node;
	char *          data;
};

static mowgli_list_t restricted_hosts;
static mowgli_list_t permitted_mechanisms;

static struct service *saslsvs = NULL;

static void
blacklist_clear_list(mowgli_list_t *const restrict list)
{
	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, list->head)
	{
		struct blacklist_entry *const entry = n->data;

		(void) mowgli_node_delete(n, list);
		(void) sfree(entry->data);
		(void) sfree(entry);
	}
}

static void
blacklist_process_configentry(mowgli_config_file_entry_t *const restrict ce, mowgli_list_t *const restrict list,
                              const char *const restrict name)
{
	if (! ce->entries)
		return;

	(void) blacklist_clear_list(list);

	mowgli_config_file_entry_t *subce;

	MOWGLI_ITER_FOREACH(subce, ce->entries)
	{
		if (! subce->entries)
		{
			struct blacklist_entry *const entry = smalloc(sizeof *entry);

			entry->data = sstrdup(subce->varname);

			(void) mowgli_node_add(entry, &entry->node, list);
		}
		else
			(void) conf_report_warning(ce, "Invalid saslserv::%s entry", name);
	}
}

static int
c_restricted_hosts(mowgli_config_file_entry_t *const restrict ce)
{
	(void) blacklist_process_configentry(ce, &restricted_hosts, "restricted_hosts");

	return 0;
}

static int
c_permitted_mechanisms(mowgli_config_file_entry_t *const restrict ce)
{
	(void) blacklist_process_configentry(ce, &permitted_mechanisms, "permitted_mechanisms");

	return 0;
}

static bool
is_restricted_host(char const *const restrict host)
{
	if (! host || ! *host)
		return false;

	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, restricted_hosts.head)
	{
		struct blacklist_entry *const entry = n->data;

		if (match(entry->data, host) == 0 || match_ips(entry->data, host) == 0)
			return true;
	}

	return false;
}

static bool
is_permitted_mechanism(char const *const restrict mech)
{
	if (! mech || ! *mech)
		return false;

	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, permitted_mechanisms.head)
	{
		struct blacklist_entry *const entry = n->data;

		if (strcmp(entry->data, mech) == 0)
			return true;
	}

	return false;
}

static void
blacklist_can_login(hook_user_login_check_t *const restrict c)
{
	if (! c->si)
		return;

	if (c->si->service == saslsvs)
	{
		const struct sasl_sourceinfo *const ssi = (struct sasl_sourceinfo *) c->si;

		if (! ssi->sess || ! ssi->sess->mechptr || ! (ssi->sess->host || ssi->sess->ip))
			return;

		if (is_restricted_host(ssi->sess->host) || is_restricted_host(ssi->sess->ip))
		{
			char const *const log_target = service_get_log_target(saslsvs);

			if (! is_permitted_mechanism(ssi->sess->mechptr->name))
			{
				(void) slog(CMDLOG_LOGIN, "%s %s:%s failed LOGIN to \2%s\2 ('%s' not allowed)",
				            log_target, entity(c->mu)->name, ssi->sess->uid, entity(c->mu)->name,
				            ssi->sess->mechptr->name);

				c->allowed = false;
			}
		}
	}
	else if (c->si->su)
	{
		if (is_restricted_host(c->si->su->host) || is_restricted_host(c->si->su->chost) ||
		    is_restricted_host(c->si->su->vhost) || is_restricted_host(c->si->su->ip))
		{
			(void) logcommand(c->si, CMDLOG_LOGIN, "failed IDENTIFY to \2%s\2 (restricted address)",
			                  entity(c->mu)->name);

			c->allowed = false;
		}
	}
}

static void
blacklist_can_register(hook_user_register_check_t *const restrict c)
{
	if (is_restricted_host(c->si->su->host) || is_restricted_host(c->si->su->chost) ||
	    is_restricted_host(c->si->su->vhost) || is_restricted_host(c->si->su->ip))
	{
		(void) logcommand(c->si, CMDLOG_LOGIN, "denied REGISTER of \2%s\2 (restricted address)", c->account);

		c->approved++;
	}
}

static void
mod_init(module_t *const restrict m)
{
	// We do this to depend on saslserv/main, so that if it is reloaded, we are too
	const struct sasl_core_functions *sasl_core_functions;
	MODULE_TRY_REQUEST_SYMBOL(m, sasl_core_functions, "saslserv/main", "sasl_core_functions");

	if (! (saslsvs = service_find("saslserv")))
	{
		(void) slog(LG_ERROR, "%s: could not find SASLServ (BUG!)", m->name);

		m->mflags |= MODFLAG_FAIL;
		return;
	}

	(void) hook_add_event("user_can_login");
	(void) hook_add_user_can_login(&blacklist_can_login);

	(void) hook_add_event("user_can_register");
	(void) hook_add_user_can_register(&blacklist_can_register);

	(void) add_conf_item("RESTRICTED_HOSTS", &saslsvs->conf_table, &c_restricted_hosts);
	(void) add_conf_item("PERMITTED_MECHANISMS", &saslsvs->conf_table, &c_permitted_mechanisms);
}

static void
mod_deinit(const module_unload_intent_t ATHEME_VATTR_UNUSED intent)
{
	(void) del_conf_item("RESTRICTED_HOSTS", &saslsvs->conf_table);
	(void) del_conf_item("PERMITTED_MECHANISMS", &saslsvs->conf_table);

	(void) hook_del_user_can_login(&blacklist_can_login);
	(void) hook_del_user_can_register(&blacklist_can_register);

	(void) blacklist_clear_list(&restricted_hosts);
	(void) blacklist_clear_list(&permitted_mechanisms);
}

#else /* (CURRENT_ABI_REVISION >= 730000) */

static void
mod_init(module_t *const restrict m)
{
	(void) slog(LG_ERROR, "%s: this module only works with Atheme v7.3.0 or above", m->name);

	m->mflags |= MODFLAG_FAIL;
}

static void
mod_deinit(const module_unload_intent_t ATHEME_VATTR_UNUSED intent)
{

}

#endif /* (CURRENT_ABI_REVISION < 730000) */

VENDOR_DECLARE_MODULE_V1("contrib/sasl_blacklist", MODULE_UNLOAD_CAPABILITY_OK, CONTRIB_VENDOR_FREENODE)
