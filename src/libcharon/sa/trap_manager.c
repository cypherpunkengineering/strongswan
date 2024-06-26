/*
 * Copyright (C) 2011-2015 Tobias Brunner
 * Copyright (C) 2009 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "trap_manager.h"

#include <daemon.h>
#include <threading/mutex.h>
#include <threading/rwlock.h>
#include <threading/rwlock_condvar.h>
#include <collections/linked_list.h>

#define INSTALL_DISABLED ((u_int)~0)

typedef struct private_trap_manager_t private_trap_manager_t;
typedef struct trap_listener_t trap_listener_t;

/**
 * listener to track acquires
 */
struct trap_listener_t {

	/**
	 * Implements listener interface
	 */
	listener_t listener;

	/**
	 * points to trap_manager
	 */
	private_trap_manager_t *traps;
};

/**
 * Private data of an trap_manager_t object.
 */
struct private_trap_manager_t {

	/**
	 * Public trap_manager_t interface.
	 */
	trap_manager_t public;

	/**
	 * Installed traps, as entry_t
	 */
	linked_list_t *traps;

	/**
	 * read write lock for traps list
	 */
	rwlock_t *lock;

	/**
	 * listener to track acquiring IKE_SAs
	 */
	trap_listener_t listener;

	/**
	 * list of acquires we currently handle
	 */
	linked_list_t *acquires;

	/**
	 * mutex for list of acquires
	 */
	mutex_t *mutex;

	/**
	 * number of threads currently installing trap policies, or INSTALL_DISABLED
	 */
	u_int installing;

	/**
	 * condvar to signal trap policy installation
	 */
	rwlock_condvar_t *condvar;

	/**
	 * Whether to ignore traffic selectors from acquires
	 */
	bool ignore_acquire_ts;
};

/**
 * A installed trap entry
 */
typedef struct {
	/** name of the trapped CHILD_SA */
	char *name;
	/** ref to peer_cfg to initiate */
	peer_cfg_t *peer_cfg;
	/** ref to instantiated CHILD_SA (i.e the trap policy) */
	child_sa_t *child_sa;
	/** TRUE in case of wildcard Transport Mode SA */
	bool wildcard;
} entry_t;

/**
 * A handled acquire
 */
typedef struct {
	/** pending IKE_SA connecting upon acquire */
	ike_sa_t *ike_sa;
	/** reqid of pending trap policy */
	uint32_t reqid;
	/** destination address (wildcard case) */
	host_t *dst;
} acquire_t;

/**
 * actually uninstall and destroy an installed entry
 */
static void destroy_entry(entry_t *this)
{
	this->child_sa->destroy(this->child_sa);
	this->peer_cfg->destroy(this->peer_cfg);
	free(this->name);
	free(this);
}

/**
 * destroy a cached acquire entry
 */
static void destroy_acquire(acquire_t *this)
{
	DESTROY_IF(this->dst);
	free(this);
}

/**
 * match an acquire entry by reqid
 */
static bool acquire_by_reqid(acquire_t *this, uint32_t *reqid)
{
	return this->reqid == *reqid;
}

/**
 * match an acquire entry by destination address
 */
static bool acquire_by_dst(acquire_t *this, host_t *dst)
{
	return this->dst && this->dst->ip_equals(this->dst, dst);
}

METHOD(trap_manager_t, install, uint32_t,
	private_trap_manager_t *this, peer_cfg_t *peer, child_cfg_t *child,
	uint32_t reqid)
{
	entry_t *entry, *found = NULL;
	ike_cfg_t *ike_cfg;
	child_sa_t *child_sa;
	host_t *me, *other;
	linked_list_t *my_ts, *other_ts, *list;
	enumerator_t *enumerator;
	status_t status;
	linked_list_t *proposals;
	proposal_t *proposal;
	protocol_id_t proto = PROTO_ESP;
	bool wildcard = FALSE;

	/* try to resolve addresses */
	ike_cfg = peer->get_ike_cfg(peer);
	other = ike_cfg->resolve_other(ike_cfg, AF_UNSPEC);
	if (other && other->is_anyaddr(other) &&
		child->get_mode(child) == MODE_TRANSPORT)
	{
		/* allow wildcard for Transport Mode SAs */
		me = host_create_any(other->get_family(other));
		wildcard = TRUE;
	}
	else if (!other || other->is_anyaddr(other))
	{
		DESTROY_IF(other);
		DBG1(DBG_CFG, "installing trap failed, remote address unknown");
		return 0;
	}
	else
	{
		me = ike_cfg->resolve_me(ike_cfg, other->get_family(other));
		if (!me || me->is_anyaddr(me))
		{
			DESTROY_IF(me);
			me = charon->kernel->get_source_addr(charon->kernel, other, NULL);
			if (!me)
			{
				me = host_create_any(other->get_family(other));
			}
			me->set_port(me, ike_cfg->get_my_port(ike_cfg));
		}
	}

	this->lock->write_lock(this->lock);
	if (this->installing == INSTALL_DISABLED)
	{	/* flush() has been called */
		this->lock->unlock(this->lock);
		other->destroy(other);
		me->destroy(me);
		return 0;
	}
	enumerator = this->traps->create_enumerator(this->traps);
	while (enumerator->enumerate(enumerator, &entry))
	{
		if (streq(entry->name, child->get_name(child)))
		{
			found = entry;
			if (entry->child_sa)
			{	/* replace it with an updated version, if already installed */
				this->traps->remove_at(this->traps, enumerator);
			}
			break;
		}
	}
	enumerator->destroy(enumerator);

	if (found)
	{
		if (!found->child_sa)
		{
			DBG1(DBG_CFG, "CHILD_SA '%s' is already being routed", found->name);
			this->lock->unlock(this->lock);
			other->destroy(other);
			me->destroy(me);
			return 0;
		}
		/* config might have changed so update everything */
		DBG1(DBG_CFG, "updating already routed CHILD_SA '%s'", found->name);
		reqid = found->child_sa->get_reqid(found->child_sa);
	}

	INIT(entry,
		.name = strdup(child->get_name(child)),
		.peer_cfg = peer->get_ref(peer),
		.wildcard = wildcard,
	);
	this->traps->insert_first(this->traps, entry);
	this->installing++;
	/* don't hold lock while creating CHILD_SA and installing policies */
	this->lock->unlock(this->lock);

	/* create and route CHILD_SA */
	child_sa = child_sa_create(me, other, child, reqid, FALSE, 0, 0);

	list = linked_list_create_with_items(me, NULL);
	my_ts = child->get_traffic_selectors(child, TRUE, NULL, list);
	list->destroy_offset(list, offsetof(host_t, destroy));

	list = linked_list_create_with_items(other, NULL);
	other_ts = child->get_traffic_selectors(child, FALSE, NULL, list);
	list->destroy_offset(list, offsetof(host_t, destroy));

	/* We don't know the finally negotiated protocol (ESP|AH), we install
	 * the SA with the protocol of the first proposal */
	proposals = child->get_proposals(child, TRUE);
	if (proposals->get_first(proposals, (void**)&proposal) == SUCCESS)
	{
		proto = proposal->get_protocol(proposal);
	}
	proposals->destroy_offset(proposals, offsetof(proposal_t, destroy));
	child_sa->set_protocol(child_sa, proto);
	child_sa->set_mode(child_sa, child->get_mode(child));
	child_sa->set_policies(child_sa, my_ts, other_ts);
	status = child_sa->install_policies(child_sa);
	my_ts->destroy_offset(my_ts, offsetof(traffic_selector_t, destroy));
	other_ts->destroy_offset(other_ts, offsetof(traffic_selector_t, destroy));
	if (status != SUCCESS)
	{
		DBG1(DBG_CFG, "installing trap failed");
		this->lock->write_lock(this->lock);
		this->traps->remove(this->traps, entry, NULL);
		this->lock->unlock(this->lock);
		entry->child_sa = child_sa;
		destroy_entry(entry);
		reqid = 0;
	}
	else
	{
		reqid = child_sa->get_reqid(child_sa);
		this->lock->write_lock(this->lock);
		entry->child_sa = child_sa;
		this->lock->unlock(this->lock);
	}
	if (found)
	{
		destroy_entry(found);
	}
	this->lock->write_lock(this->lock);
	/* do this at the end, so entries created temporarily are also destroyed */
	this->installing--;
	this->condvar->signal(this->condvar);
	this->lock->unlock(this->lock);
	return reqid;
}

METHOD(trap_manager_t, uninstall, bool,
	private_trap_manager_t *this, uint32_t reqid)
{
	enumerator_t *enumerator;
	entry_t *entry, *found = NULL;

	this->lock->write_lock(this->lock);
	enumerator = this->traps->create_enumerator(this->traps);
	while (enumerator->enumerate(enumerator, &entry))
	{
		if (entry->child_sa &&
			entry->child_sa->get_reqid(entry->child_sa) == reqid)
		{
			this->traps->remove_at(this->traps, enumerator);
			found = entry;
			break;
		}
	}
	enumerator->destroy(enumerator);
	this->lock->unlock(this->lock);

	if (!found)
	{
		DBG1(DBG_CFG, "trap %d not found to uninstall", reqid);
		return FALSE;
	}
	destroy_entry(found);
	return TRUE;
}

/**
 * convert enumerated entries to peer_cfg, child_sa
 */
static bool trap_filter(rwlock_t *lock, entry_t **entry, peer_cfg_t **peer_cfg,
						void *none, child_sa_t **child_sa)
{
	if (!(*entry)->child_sa)
	{	/* skip entries that are currently being installed */
		return FALSE;
	}
	if (peer_cfg)
	{
		*peer_cfg = (*entry)->peer_cfg;
	}
	if (child_sa)
	{
		*child_sa = (*entry)->child_sa;
	}
	return TRUE;
}

METHOD(trap_manager_t, create_enumerator, enumerator_t*,
	private_trap_manager_t *this)
{
	this->lock->read_lock(this->lock);
	return enumerator_create_filter(this->traps->create_enumerator(this->traps),
									(void*)trap_filter, this->lock,
									(void*)this->lock->unlock);
}

METHOD(trap_manager_t, find_reqid, uint32_t,
	private_trap_manager_t *this, child_cfg_t *child)
{
	enumerator_t *enumerator;
	entry_t *entry;
	uint32_t reqid = 0;

	this->lock->read_lock(this->lock);
	enumerator = this->traps->create_enumerator(this->traps);
	while (enumerator->enumerate(enumerator, &entry))
	{
		if (streq(entry->name, child->get_name(child)))
		{
			if (entry->child_sa)
			{
				reqid = entry->child_sa->get_reqid(entry->child_sa);
			}
			break;
		}
	}
	enumerator->destroy(enumerator);
	this->lock->unlock(this->lock);
	return reqid;
}

METHOD(trap_manager_t, acquire, void,
	private_trap_manager_t *this, uint32_t reqid,
	traffic_selector_t *src, traffic_selector_t *dst)
{
	enumerator_t *enumerator;
	entry_t *entry, *found = NULL;
	acquire_t *acquire;
	peer_cfg_t *peer;
	child_cfg_t *child;
	ike_sa_t *ike_sa;
	host_t *host;
	bool wildcard, ignore = FALSE;

	this->lock->read_lock(this->lock);
	enumerator = this->traps->create_enumerator(this->traps);
	while (enumerator->enumerate(enumerator, &entry))
	{
		if (entry->child_sa &&
			entry->child_sa->get_reqid(entry->child_sa) == reqid)
		{
			found = entry;
			break;
		}
	}
	enumerator->destroy(enumerator);

	if (!found)
	{
		DBG1(DBG_CFG, "trap not found, unable to acquire reqid %d", reqid);
		this->lock->unlock(this->lock);
		return;
	}
	reqid = found->child_sa->get_reqid(found->child_sa);
	wildcard = found->wildcard;

	this->mutex->lock(this->mutex);
	if (wildcard)
	{	/* for wildcard acquires we check that we don't have a pending acquire
		 * with the same peer */
		uint8_t mask;

		dst->to_subnet(dst, &host, &mask);
		if (this->acquires->find_first(this->acquires, (void*)acquire_by_dst,
									  (void**)&acquire, host) == SUCCESS)
		{
			host->destroy(host);
			ignore = TRUE;
		}
		else
		{
			INIT(acquire,
				.dst = host,
				.reqid = reqid,
			);
			this->acquires->insert_last(this->acquires, acquire);
		}
	}
	else
	{
		if (this->acquires->find_first(this->acquires, (void*)acquire_by_reqid,
									  (void**)&acquire, &reqid) == SUCCESS)
		{
			ignore = TRUE;
		}
		else
		{
			INIT(acquire,
				.reqid = reqid,
			);
			this->acquires->insert_last(this->acquires, acquire);
		}
	}
	this->mutex->unlock(this->mutex);
	if (ignore)
	{
		DBG1(DBG_CFG, "ignoring acquire, connection attempt pending");
		this->lock->unlock(this->lock);
		return;
	}
	peer = found->peer_cfg->get_ref(found->peer_cfg);
	child = found->child_sa->get_config(found->child_sa);
	child = child->get_ref(child);
	/* don't hold the lock while checking out the IKE_SA */
	this->lock->unlock(this->lock);

	if (wildcard)
	{	/* the peer config would match IKE_SAs with other peers */
		ike_sa = charon->ike_sa_manager->checkout_new(charon->ike_sa_manager,
											peer->get_ike_version(peer), TRUE);
		if (ike_sa)
		{
			ike_cfg_t *ike_cfg;
			uint16_t port;
			uint8_t mask;

			ike_sa->set_peer_cfg(ike_sa, peer);
			ike_cfg = ike_sa->get_ike_cfg(ike_sa);

			port = ike_cfg->get_other_port(ike_cfg);
			dst->to_subnet(dst, &host, &mask);
			host->set_port(host, port);
			ike_sa->set_other_host(ike_sa, host);

			port = ike_cfg->get_my_port(ike_cfg);
			src->to_subnet(src, &host, &mask);
			host->set_port(host, port);
			ike_sa->set_my_host(ike_sa, host);

			charon->bus->set_sa(charon->bus, ike_sa);
		}
	}
	else
	{
		ike_sa = charon->ike_sa_manager->checkout_by_config(
											charon->ike_sa_manager, peer);
	}
	if (ike_sa)
	{
		if (ike_sa->get_peer_cfg(ike_sa) == NULL)
		{
			ike_sa->set_peer_cfg(ike_sa, peer);
		}
		if (this->ignore_acquire_ts || ike_sa->get_version(ike_sa) == IKEV1)
		{	/* in IKEv1, don't prepend the acquiring packet TS, as we only
			 * have a single TS that we can establish in a Quick Mode. */
			src = dst = NULL;
		}

		this->mutex->lock(this->mutex);
		acquire->ike_sa = ike_sa;
		this->mutex->unlock(this->mutex);

		if (ike_sa->initiate(ike_sa, child, reqid, src, dst) != DESTROY_ME)
		{
			charon->ike_sa_manager->checkin(charon->ike_sa_manager, ike_sa);
		}
		else
		{
			charon->ike_sa_manager->checkin_and_destroy(charon->ike_sa_manager,
														ike_sa);
		}
	}
	else
	{
		this->mutex->lock(this->mutex);
		this->acquires->remove(this->acquires, acquire, NULL);
		this->mutex->unlock(this->mutex);
		destroy_acquire(acquire);
		child->destroy(child);
	}
	peer->destroy(peer);
}

/**
 * Complete the acquire, if successful or failed
 */
static void complete(private_trap_manager_t *this, ike_sa_t *ike_sa,
					 child_sa_t *child_sa)
{
	enumerator_t *enumerator;
	acquire_t *acquire;

	this->mutex->lock(this->mutex);
	enumerator = this->acquires->create_enumerator(this->acquires);
	while (enumerator->enumerate(enumerator, &acquire))
	{
		if (!acquire->ike_sa || acquire->ike_sa != ike_sa)
		{
			continue;
		}
		if (child_sa)
		{
			if (acquire->dst)
			{
				/* since every wildcard acquire results in a separate IKE_SA
				 * there is no need to compare the destination address */
			}
			else if (child_sa->get_reqid(child_sa) != acquire->reqid)
			{
				continue;
			}
		}
		this->acquires->remove_at(this->acquires, enumerator);
		destroy_acquire(acquire);
	}
	enumerator->destroy(enumerator);
	this->mutex->unlock(this->mutex);
}

METHOD(listener_t, ike_state_change, bool,
	trap_listener_t *listener, ike_sa_t *ike_sa, ike_sa_state_t state)
{
	switch (state)
	{
		case IKE_DESTROYING:
			complete(listener->traps, ike_sa, NULL);
			return TRUE;
		default:
			return TRUE;
	}
}

METHOD(listener_t, child_state_change, bool,
	trap_listener_t *listener, ike_sa_t *ike_sa, child_sa_t *child_sa,
	child_sa_state_t state)
{
	switch (state)
	{
		case CHILD_INSTALLED:
		case CHILD_DESTROYING:
			complete(listener->traps, ike_sa, child_sa);
			return TRUE;
		default:
			return TRUE;
	}
}

METHOD(trap_manager_t, flush, void,
	private_trap_manager_t *this)
{
	this->lock->write_lock(this->lock);
	while (this->installing)
	{
		this->condvar->wait(this->condvar, this->lock);
	}
	this->traps->destroy_function(this->traps, (void*)destroy_entry);
	this->traps = linked_list_create();
	this->installing = INSTALL_DISABLED;
	this->lock->unlock(this->lock);
}

METHOD(trap_manager_t, destroy, void,
	private_trap_manager_t *this)
{
	charon->bus->remove_listener(charon->bus, &this->listener.listener);
	this->traps->destroy_function(this->traps, (void*)destroy_entry);
	this->acquires->destroy_function(this->acquires, (void*)destroy_acquire);
	this->condvar->destroy(this->condvar);
	this->mutex->destroy(this->mutex);
	this->lock->destroy(this->lock);
	free(this);
}

/**
 * See header
 */
trap_manager_t *trap_manager_create(void)
{
	private_trap_manager_t *this;

	INIT(this,
		.public = {
			.install = _install,
			.uninstall = _uninstall,
			.create_enumerator = _create_enumerator,
			.find_reqid = _find_reqid,
			.acquire = _acquire,
			.flush = _flush,
			.destroy = _destroy,
		},
		.listener = {
			.traps = this,
			.listener = {
				.ike_state_change = _ike_state_change,
				.child_state_change = _child_state_change,
			},
		},
		.traps = linked_list_create(),
		.acquires = linked_list_create(),
		.mutex = mutex_create(MUTEX_TYPE_DEFAULT),
		.lock = rwlock_create(RWLOCK_TYPE_DEFAULT),
		.condvar = rwlock_condvar_create(),
		.ignore_acquire_ts = lib->settings->get_bool(lib->settings,
										"%s.ignore_acquire_ts", FALSE, lib->ns),
	);
	charon->bus->add_listener(charon->bus, &this->listener.listener);

	return &this->public;
}
