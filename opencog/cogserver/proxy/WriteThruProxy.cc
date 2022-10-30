/*
 * opencog/cogserver/proxy/WriteThruProxy.cc
 *
 * Simple WriteThru proxy agent
 * Copyright (c) 2008, 2020, 2022 Linas Vepstas <linas@linas.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <cstdio>
#include <functional>

#include <opencog/atomspace/AtomSpace.h>
#include <opencog/atoms/base/Atom.h>
#include <opencog/persist/sexpr/Commands.h>
#include <opencog/persist/sexpr/Sexpr.h>

#include <opencog/cogserver/server/CogServer.h>
#include "WriteThruProxy.h"

using namespace opencog;
using namespace std::placeholders;  // for _1, _2, _3...

DECLARE_MODULE(WriteThruProxy);

WriteThruProxy::WriteThruProxy(CogServer& cs) : Proxy(cs)
{
}

void WriteThruProxy::init(void)
{
	AtomSpacePtr as = _cogserver.getAtomSpace();

	// Tell the command decoder what AtomSpace to use
	_decoder.set_base_space(as);

	// Get all of the StorageNodes to which we will be
	// forwarding writes.
	HandleSeq hsns;
	as->get_handles_by_type(hsns, STORAGE_NODE, true);

	_targets.clear();
	for (const Handle& hsn : hsns)
	{
		StorageNodePtr snp = StorageNodeCast(hsn);

		// Check to see if the StorageNode is actually open;
		// We cannot write to nodes that are closed.
		if (snp->connected())
		{
			_targets.push_back(snp);
			logger().info("[Write-Thru Proxy] Will write-thru to %s\n",
				snp->to_short_string().c_str());
		}

		// TODO: check if the StorageNode is read-only.
	}

	if (0 == _targets.size())
		logger().info("[Write-Thru Proxy] There aren't any targets to write to!");
}

WriteThruProxy::~WriteThruProxy()
{
}

bool WriteThruProxy::config(const char* cfg)
{
printf("duuuude write-thru proxy cfg %s\n", cfg);
	return false;
}

// TODO:
// * Need space-frame support!

void WriteThruProxy::setup(SexprEval* sev)
{
	// Read-only atomspace ... should check earlier!?
	AtomSpacePtr as = _cogserver.getAtomSpace();
	if (as->get_read_only())
	{
		logger().info("Read-only atomspace; no write-through proxying!");
		return;
	}

	using namespace std::placeholders;  // for _1, _2, _3...

	// Install dispatch handlers.
	sev->install_handler("cog-extract!",
		std::bind(&WriteThruProxy::cog_extract, this, _1));
	sev->install_handler("cog-extract-recursive!",
		std::bind(&WriteThruProxy::cog_extract_recursive, this, _1));

	sev->install_handler("cog-set-value!",
		std::bind(&WriteThruProxy::cog_set_value, this, _1));
	sev->install_handler("cog-set-values!",
		std::bind(&WriteThruProxy::cog_set_values, this, _1));
	sev->install_handler("cog-set-tv!",
		std::bind(&WriteThruProxy::cog_set_tv, this, _1));
	sev->install_handler("cog-update-value!",
		std::bind(&WriteThruProxy::cog_update_value, this, _1));
}

std::string WriteThruProxy::cog_extract_helper(const std::string& arg,
                                               bool flag)
{
	// XXX FIXME Handle space frames
	AtomSpacePtr as = _cogserver.getAtomSpace();
	size_t pos = 0;
	// Handle h = _base_space->get_atom(Sexpr::decode_atom(arg, pos, _space_map));
	Handle h = as->get_atom(Sexpr::decode_atom(arg, pos));
	if (nullptr == h) return "#t";
	// if (not _base_space->extract_atom(h, flag)) return "#f";
	if (not as->extract_atom(h, flag)) return "#f";

	// Loop over all targets, and extract there as well.
	for (const StorageNodePtr& snp : _targets)
		snp->remove_atom(as, h, flag);

	return "#t";
}

// ------------------------------------------------------------------

std::string WriteThruProxy::cog_set_value(const std::string& arg)
{
	return _decoder.cog_set_value(arg,
		std::bind(&WriteThruProxy::set_value_cb, this, _1, _2, _3));
}

std::string WriteThruProxy::cog_set_values(const std::string& arg)
{
	return _decoder.cog_set_values(arg,
		std::bind(&WriteThruProxy::set_values_cb, this, _1));
}

std::string WriteThruProxy::cog_set_tv(const std::string& arg)
{
	return _decoder.cog_set_tv(arg,
		std::bind(&WriteThruProxy::set_tv_cb, this, _1, _2));
}

std::string WriteThruProxy::cog_update_value(const std::string& arg)
{
	return _decoder.cog_update_value(arg,
		std::bind(&WriteThruProxy::update_value_cb, this, _1, _2, _3));
}

// ------------------------------------------------------------------

// This is called after the string is decoded.
void WriteThruProxy::set_value_cb(const Handle& atom, const Handle& key,
                                  const ValuePtr& v)
{
	// Loop over all targets, and send them the new value.
	for (const StorageNodePtr& snp : _targets)
		snp->store_value(atom, key);
}

void WriteThruProxy::set_values_cb(const Handle& atom)
{
	// Loop over all targets, and store everything.
	// In principle, we should be selective, and only pass
	// on the values we were given... this would require
	// Sexpr::decode_slist to return a list of keys,
	// and then we'd have to store one key at a time,
	// which seems inefficient. But still ... XXX FIXME ?
	for (const StorageNodePtr& snp : _targets)
		snp->store_atom(atom);
}

void WriteThruProxy::set_tv_cb(const Handle& ha, const TruthValuePtr& tv)
{
	// XXX FIXME what if user changed the AS recnetly? Other code (above)
	// will be using the old AS!!
	AtomSpacePtr as = _cogserver.getAtomSpace();

	// Make sure we can store truth values!
	if (nullptr == _truth_key)
		_truth_key = as->add_atom(
			createNode(PREDICATE_NODE, "*-TruthValueKey-*"));

	// Loop over all targets, and send them the new truth value.
	for (const StorageNodePtr& snp : _targets)
		snp->store_value(ha, _truth_key);
}

void WriteThruProxy::update_value_cb(const Handle& atom, const Handle& key,
                                     const ValuePtr& delta)
{
	// Loop over all targets, and send them the delta value.
	for (const StorageNodePtr& snp : _targets)
		snp->update_value(atom, key, delta);
}

/* ===================== END OF FILE ============================ */
