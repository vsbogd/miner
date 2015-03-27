/*
 * opencog/atomspace/AtomSpaceImpl.cc
 *
 * Copyright (c) 2008-2010 OpenCog Foundation
 * Copyright (c) 2009, 2013 Linas Vepstas
 * All Rights Reserved
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

#include <string>
#include <iostream>
#include <fstream>
#include <list>

#include <stdlib.h>

#include <boost/bind.hpp>

#include <opencog/atomspace/ClassServer.h>
#include <opencog/atomspace/Link.h>
#include <opencog/atomspace/Node.h>
#include <opencog/atomspace/types.h>
#include <opencog/util/Logger.h>
#include <opencog/util/oc_assert.h>

#include "AtomSpaceImpl.h"

//#define DPRINTF printf
#define DPRINTF(...)

using std::string;
using std::cerr;
using std::cout;
using std::endl;
using std::min;
using std::max;
using namespace opencog;

// ====================================================================

AtomSpaceImpl::AtomSpaceImpl(AtomSpaceImpl* parent) :
    atomTable(parent? &parent->atomTable : NULL),
    bank(atomTable)
{
    backing_store = NULL;
    DPRINTF("AtomSpaceImpl::Constructor AtomTable address: %p\n", &atomTable);
}

AtomSpaceImpl::~AtomSpaceImpl()
{
    // Be sure to disconnect the attention bank signals before the
    // atom table destructor runs. XXX FIXME yes this is an ugly hack.
    bank.shutdown();
}

// ====================================================================

void AtomSpaceImpl::registerBackingStore(BackingStore *bs)
{
    backing_store = bs;
}

void AtomSpaceImpl::unregisterBackingStore(BackingStore *bs)
{
    if (bs == backing_store) backing_store = NULL;
}

// ====================================================================

AtomSpaceImpl& AtomSpaceImpl::operator=(const AtomSpaceImpl& other)
{
    throw opencog::RuntimeException(TRACE_INFO,
            "AtomSpaceImpl - Cannot copy an object of this class");
}

AtomSpaceImpl::AtomSpaceImpl(const AtomSpaceImpl& other)
    : bank(atomTable)
{
    throw opencog::RuntimeException(TRACE_INFO,
            "AtomSpaceImpl - Cannot copy an object of this class");
}

Handle AtomSpaceImpl::addAtom(AtomPtr atom, bool async)
{
    // Is this atom already in the atom table?
    Handle hexist(atomTable.getHandle(atom));
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    Type t = atom->getType();
    if (backing_store and not backing_store->ignoreType(t)) {

        Handle ha(atom);
        Handle hb(backing_store->getAtom(ha));
        if (hb.value() != Handle::UNDEFINED.value()) {
            return atomTable.add(hb, async);
        }
    }

    // If we are here, neither the AtomTable nor backing store know about
    // this atom. Just add it.
    return atomTable.add(atom, async);
}

Handle AtomSpaceImpl::addNode(Type t, const string& name,
                              bool async)
{
    // Is this atom already in the atom table?
    Handle hexist(atomTable.getHandle(t, name));
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        NodePtr n(backing_store->getNode(t, name.c_str()));
        if (n) return atomTable.add(n, async);
    }

    // If we are here, neither the AtomTable nor backing store know about
    // this atom. Just add it.
    return atomTable.add(createNode(t, name), async);
}

Handle AtomSpaceImpl::getNode(Type t, const string& name)
{
    // Is this atom already in the atom table?
    Handle hexist = atomTable.getHandle(t, name);
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        NodePtr n(backing_store->getNode(t, name.c_str()));
        if (n) {
            return atomTable.add(n, false);
        }
    }

    // If we are here, nobody knows about this.
    return Handle::UNDEFINED;
}

Handle AtomSpaceImpl::addLink(Type t, const HandleSeq& outgoing,
                              bool async)
{
    // Is this atom already in the atom table?
    Handle hexist = atomTable.getHandle(t, outgoing);
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        // If any of the outgoing set is ignorable, we will not
        // fetch the thing from the backing store.
        if (not std::any_of(outgoing.begin(), outgoing.end(),
            [this](Handle ho) { return backing_store->ignoreAtom(ho); }))
        {
            LinkPtr l(backing_store->getLink(t, outgoing));
            if (l) {
                // Put the atom into the atomtable, so it gets placed
                // in indices, so we can find it quickly next time.
                return atomTable.add(l, async);
            }
        }
    }

    // If we are here, neither the AtomTable nor backing store know about
    // this atom. Just add it.
    return atomTable.add(createLink(t, outgoing), async);
}

Handle AtomSpaceImpl::getLink(Type t, const HandleSeq& outgoing)
{
    // Is this atom already in the atom table?
    Handle hexist = atomTable.getHandle(t, outgoing);
    if (hexist) return hexist;

    // If we are here, the AtomTable does not yet know about this atom.
    // Maybe the backing store knows about this atom.
    if (backing_store and not backing_store->ignoreType(t))
    {
        // If any of the outgoing set is ignorable, we will not
        // fetch the thing from the backing store.
        if (not std::any_of(outgoing.begin(), outgoing.end(),
            [this](Handle ho) { return backing_store->ignoreAtom(ho); }))
        {
            LinkPtr l(backing_store->getLink(t, outgoing));
            if (l) {
                // Register the atom with the atomtable (so it gets placed in
                // indices)
                return atomTable.add(l, false);
            }
        }
    }

    // If we are here, nobody knows about this.
    return Handle::UNDEFINED;
}

void AtomSpaceImpl::storeAtom(Handle h)
{
    if (NULL == backing_store)
        throw RuntimeException(TRACE_INFO, "No backing store");

    backing_store->storeAtom(h);
}

Handle AtomSpaceImpl::fetchAtom(Handle h)
{
    if (NULL == backing_store)
        throw RuntimeException(TRACE_INFO, "No backing store");

    // OK, we have to handle three distinct cases.
    // 1) If atom table already knows about this uuid or atom, then
    //    this function returns the atom-table's version of the atom.
    //    In particular, no attempt is made to reconcile the possibly
    //    differing truth values in the atomtable vs. backing store.
    // 2) If the handle h holds a UUID but no atom pointer, then get
    //    the corresponding atom from storage, and add it to the atom
    //    table.
    // 3) If the handle h contains a pointer to an atom (that is not
    //    in the atom table), then assume that atom is from some previous
    //    (recursive) query, and add it to the atomtable.
    // For both case 2 & 3, if the atom is a link, then it's outgoing
    // set is fetched as well, as currently, a link cannot be added to
    // the atomtable, unless all of its outgoing set already is in the
    // atomtable.

    // Case 1:
    Handle hb(atomTable.getHandle(h));
    if (atomTable.holds(hb))
        return hb;

    // Case 2 & 3:
    // If we don't have the atom for this UUID, then go get it.
    if (NULL == h.operator->()) {
        Handle hb(backing_store->getAtom(h));

        // If we still don't have an atom, then the requested UUID
        // was "insane", that is, unknown by either the atom table
        // (case 1) or the backend.
        if (NULL == hb.operator->())
            throw RuntimeException(TRACE_INFO,
                "Asked backend for an unknown handle; UUID=%lu\n",
                h.value());
        h = hb;
    }

    // For links, must perform a recursive fetch, as otherwise
    // the atomtable.add below will throw an error.
    LinkPtr l(LinkCast(h));
    if (l) {
       const HandleSeq& ogs = l->getOutgoingSet();
       size_t arity = ogs.size();
       for (size_t i=0; i<arity; i++)
       {
          Handle oh(fetchAtom(ogs[i]));
          if (oh != ogs[i]) throw RuntimeException(TRACE_INFO,
              "Unexpected handle mismatch! Expected %lu got %lu\n",
              ogs[i].value(), oh.value());
       }
    }

    return atomTable.add(h, false);
}

Handle AtomSpaceImpl::getAtom(Handle h)
{
    if (atomTable.holds(h)) return h;
    return fetchAtom(h);
}

Handle AtomSpaceImpl::fetchIncomingSet(Handle h, bool recursive)
{
    if (NULL == backing_store)
        throw RuntimeException(TRACE_INFO, "No backing store");

    h = getAtom(h);

    if (Handle::UNDEFINED == h) return Handle::UNDEFINED;

    // Get everything from the backing store.
    HandleSeq iset = backing_store->getIncomingSet(h);
    size_t isz = iset.size();
    for (size_t i=0; i<isz; i++) {
        Handle hi(iset[i]);
        if (recursive) {
            fetchIncomingSet(hi, true);
        } else {
            getAtom(hi);
        }
    }
    return h;
}

HandleSeq AtomSpaceImpl::getNeighbors(Handle h, bool fanin,
                                      bool fanout, Type desiredLinkType,
                                      bool subClasses) const
{
    if (h == NULL) {
        throw InvalidParamException(TRACE_INFO,
            "Handle %d doesn't refer to a Atom", h.value());
    }
    HandleSeq answer;

    for (const LinkPtr& link : h->getIncomingSet())
    {
        Type linkType = link->getType();
        DPRINTF("Atom::getNeighbors(): linkType = %d desiredLinkType = %d\n",
                linkType, desiredLinkType);
        if ((linkType == desiredLinkType)
            || (subClasses && classserver().isA(linkType, desiredLinkType))) {
            for (const Handle& handle : link->getOutgoingSet()) {
                if (handle == h) continue;
                if (!fanout && link->isSource(h)) continue;
                if (!fanin && link->isTarget(h)) continue;
                answer.push_back(handle);
            }
        }
    }
    return answer;
}

bool AtomSpaceImpl::removeAtom(Handle h, bool recursive)
{
    if (backing_store) {
// Under construction ....
        throw RuntimeException(TRACE_INFO, "Not Implemented!!!");
    }
    return 0 < atomTable.extract(h, recursive).size();
}

void AtomSpaceImpl::clear()
{
    std::vector<Handle> allAtoms;

    atomTable.getHandlesByType(back_inserter(allAtoms), ATOM, true);

    DPRINTF("atoms in allAtoms: %lu\n", allAtoms.size());

    Logger::Level save = logger().getLevel();
    logger().setLevel(Logger::DEBUG);

    // XXX FIXME TODO This is a stunningly inefficient way to clear the
    // atomspace! This will take minutes on any decent-sized atomspace!
    std::vector<Handle>::iterator i;
    for (i = allAtoms.begin(); i != allAtoms.end(); ++i) {
        purgeAtom(*i, true);
    }

    allAtoms.clear();
    atomTable.getHandlesByType(back_inserter(allAtoms), ATOM, true);
    assert(allAtoms.size() == 0);

    logger().setLevel(save);
}
