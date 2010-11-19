/*
 * opencog/ubigraph/Ubigrapher.cc
 *
 * Copyright (C) 2008-2009 by Singularity Institute for Artificial Intelligence
 * All Rights Reserved
 *
 * Written by Jared Wigmore <jared.wigmore@gmail.com>
 * Adapted from code in DottyModule (which is by Trent Waddington <trent.waddington@gmail.com>)
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

#include <sstream>
#include <iomanip>
#include <string>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include <boost/shared_ptr.hpp>

#include <opencog/util/Logger.h>
#include <opencog/atomspace/Link.h>
#include <opencog/atomspace/Node.h>
#include <opencog/server/CogServer.h>

using namespace std;

extern "C" {
    #include "UbigraphAPI.h"
    #include <xmlrpc.h>
}
#include "Ubigrapher.h"

extern const char* ubigraph_url;

namespace opencog
{

std::string initials(std::string s)
{
    std::string ret;
    foreach (char c,  s) {
        if (toupper(c) == c) {
            ret += c;
        }
    }
    return ret;
}

Ubigrapher::Ubigrapher() : pushDelay(1), connected(false),
    withIncoming(false), compact(false)
{
    space = CogServer::getAtomSpace();
    serverIP = "";
    serverPort = 0;

    compactLabels = true;
    labelsOn = true;

}

void Ubigrapher::init(std::string server, int port)
{
    std::ostringstream os;
    serverIP = server;
    serverPort = port;
    connected = false;
    listening = false;
    os << "http://" << serverIP << ":" << serverPort << "/RPC2";
    serverString = os.str();
    logger().info("Ubigrapher will connect to " + serverString);
    ubigraph_url = serverString.c_str();

    if (ubigraph_clear() == UBIGRAPH_SUCCESS) {
        connected = true;
        setStyles();
    }
}

void Ubigrapher::watchSignals()
{
    if (isConnected()) {
        if (!listening) {
            c_add = space->addAtomSignal().connect(
                    boost::bind(&Ubigrapher::handleAddSignal, this, _1));
            c_remove = space->removeAtomSignal().connect(
                    boost::bind(&Ubigrapher::handleRemoveSignal, this, _1));
            assert(c_add.connected() && c_remove.connected());
            listening = true;
        } else {
            logger().error("[Ubigrapher] Couldn't watch signals, already watching!");
        }
    } else {
        logger().error("[Ubigrapher] Not connected, so won't watch signals!");
    }
}

void Ubigrapher::unwatchSignals()
{
    if (listening) {
        c_add.disconnect();
        c_remove.disconnect();
        assert(!(c_add.connected() || c_remove.connected()));
        listening = false;
    } else {
        logger().error("[Ubigrapher] Couldn't unwatch signals, none connected!");
    }
}

void Ubigrapher::setStyles()
{
    if (!isConnected()) return;
    //cout << "Ubigrapher setStyles" << endl;
    // Set the styles for various types of edges and vertexes in the graph
    nodeStyle = ubigraph_new_vertex_style(0);
    ubigraph_set_vertex_style_attribute(nodeStyle, "shape", "sphere");
    
    linkStyle = ubigraph_new_vertex_style(0);
    ubigraph_set_vertex_style_attribute(linkStyle, "shape", "octahedron");
    ubigraph_set_vertex_style_attribute(linkStyle, "color", "#ff0000");

    outgoingStyle = ubigraph_new_edge_style(0);
    compactLinkStyle = ubigraph_new_edge_style(outgoingStyle);

    outgoingStyleDirected = ubigraph_new_edge_style(outgoingStyle);
    ubigraph_set_edge_style_attribute(outgoingStyleDirected, "arrow", "true");
    // Makes it easier to see the direction of the arrows (cones),
    // but hides the number/type labels
    //ubigraph_set_edge_style_attribute(outgoingStyle, "arrow_radius", "1.5");
    ubigraph_set_edge_style_attribute(outgoingStyleDirected, "arrow_length",
            "2.0");

    compactLinkStyleDirected = ubigraph_new_edge_style(compactLinkStyle);
    ubigraph_set_edge_style_attribute(compactLinkStyleDirected, "arrow",
            "true");
    ubigraph_set_edge_style_attribute(compactLinkStyleDirected, "arrow_length",
            "2.0");
    
}

bool Ubigrapher::handleAddSignal(Handle h)
{
    if (!isConnected()) return false;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);
    usleep(pushDelay);
    if (space->isNode(a->getType()))
        return addVertex(h);
    else {
        if (compact) {
            // don't make nodes for binary links with no incoming
            boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
            if (l && l->getOutgoingSet().size() == 2 &&
                     l->getIncomingSet() == NULL)
                return addEdges(h);
        }
        return (addVertex(h) || addEdges(h));
    }
}

bool Ubigrapher::handleRemoveSignal(Handle h)
{
    if (!isConnected()) return false;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);
    usleep(pushDelay);
    if (space->isNode(a->getType()))
        return removeVertex(h);
    else {
        if (compact) {
            // don't make nodes for binary links with no incoming
            boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
            if (l && l->getOutgoingSet().size() == 2 &&
                     l->getIncomingSet() == NULL)
                return removeEdges(h);
        }
        return removeVertex(h);
    }

}

void Ubigrapher::updateSizeOfHandle(Handle h, property_t p, float multiplier, float baseline)
{
    if (!isConnected()) return;
    float scaler;
    std::ostringstream ost;
    switch (p) {
    case NONE:
        break;
    case TV_STRENGTH:
        scaler = space->getTV(h).getMean() * multiplier;
        break;
    case STI:
        scaler = space->getNormalisedZeroToOneSTI(h,false,true)
            * multiplier;
    }
    ost << baseline + scaler;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);
    if (space->inheritsType(a->getType(), LINK)) {
        boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
        const std::vector<Handle> &out = l->getOutgoingSet();
        if (compact && out.size() == 2 && l->getIncomingSet() == NULL) {
            ubigraph_set_edge_attribute(h.value(), "width", ost.str().c_str());
        } else
            ubigraph_set_vertex_attribute(h.value(), "size", ost.str().c_str());
    } else {
        ubigraph_set_vertex_attribute(h.value(), "size", ost.str().c_str());
    }

}

void Ubigrapher::updateSizeOfType(Type t, property_t p, float multiplier, float baseline)
{
    if (!isConnected()) return;
    HandleSeq hs;
    std::back_insert_iterator< HandleSeq > out_hi(hs);

    // Get all atoms (and subtypes) of type t
    space->getHandleSet(out_hi, t, true);
    // For each, get prop, scale... and 
    foreach (Handle h, hs) {
        updateSizeOfHandle(h, p, multiplier, baseline);
    }

}

void Ubigrapher::updateColourOfHandle(Handle h, property_t p, unsigned char startRGB[3],
        unsigned char endRGB[3], float hard)
{
    if (!isConnected()) return;
    unsigned char val[3];
    float scaler;
    int j;
    std::ostringstream ost, ost2;
    unsigned char diff[3];
    float multiplierForTV = 10.0f;

    // Find the range that colour changes over for each component
    for (j = 0; j < 3; j++)
        diff[j] = endRGB[j] - startRGB[j];

    ost << "#";
    switch (p) {
    case NONE:
        scaler=1.0f;
        break;
    case TV_STRENGTH:
        scaler = space->getTV(h).getMean();
        break;
    case STI:
        scaler = space->getNormalisedZeroToOneSTI(h,false,true);
    }
    if (hard == 0.0f) {
        if (p == TV_STRENGTH) scaler *= multiplierForTV;
        for (j = 0; j < 3; j++) val[j] = startRGB[j];
        if (scaler > 1.0f) scaler = 1.0f;
        for (j=0; j < 3; j++) {
            val[j] += (unsigned char) (scaler * diff[j]);
            ost << hex << setfill('0') << setw(2) << int(val[j]);
        }
    } else {
        if (scaler < hard) {
            for (j=0; j < 3; j++)
                ost << hex << setfill('0') << setw(2) << int(startRGB[j]);
        } else {
            for (j=0; j < 3; j++)
                ost << hex << setfill('0') << setw(2) << int(endRGB[j]);
        }
    }

    boost::shared_ptr<Atom> a = space->cloneAtom(h);
    if (space->inheritsType(a->getType(), LINK)) {
        boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
        const std::vector<Handle> &out = l->getOutgoingSet();
        if (compact && out.size() == 2 && l->getIncomingSet() == NULL) {
            //ubigraph_set_edge_attribute(h.value(), "color", "#ffffff");
            ubigraph_set_edge_attribute(h.value(), "color", ost.str().c_str());
            //ubigraph_set_edge_attribute(h.value(), "width", "2");
            ubigraph_set_edge_attribute(h.value(), "stroke", "solid");
        } else
            ubigraph_set_vertex_attribute(h.value(), "color", ost.str().c_str());
    } else {
        ubigraph_set_vertex_attribute(h.value(), "color", ost.str().c_str());
//            ost2 << 1.0 + 4 * space->getNormalisedZeroToOneSTI(h,false,true);
//            ubigraph_set_vertex_attribute(h.value(), "size", ost2.str().c_str());
    }
}

void Ubigrapher::updateColourOfType(Type t, property_t p, unsigned char startRGB[3],
        unsigned char endRGB[3], float hard)
{
    if (!isConnected()) return;
    // Ubigraph doesn't display color properly when set for individual
    // links. Instead, we have to use a style for each base color and change the
    // brightness.
    HandleSeq hs;
    std::back_insert_iterator< HandleSeq > out_hi(hs);
    
    // Get all atoms (and subtypes) of type t
    space->getHandleSet(out_hi, t, true);
    // For each, get prop, scale... and 
    foreach (Handle h, hs) {
        updateColourOfHandle(h, p, startRGB, endRGB, hard);
    }

}

void Ubigrapher::applyStyleToType(Type t, int style)
{
    if (!isConnected()) return;
    HandleSeq hs;
    std::back_insert_iterator< HandleSeq > out_hi(hs);
    // Get all atoms (and subtypes) of type t
    space->getHandleSet(out_hi, t, true);
    applyStyleToHandleSeq(hs, style);
}

void Ubigrapher::applyStyleToTypeGreaterThan(Type t, int style, property_t p, float limit)
{
    if (!isConnected()) return;
    HandleSeq hs;
    std::back_insert_iterator< HandleSeq > out_hi(hs);

    // Get all atoms (and subtypes) of type t
    space->getHandleSet(out_hi, t, true);
    // For each, get prop, scale... and 
    foreach (Handle h, hs) {
        bool okToApply = true;
        switch (p) {
        case NONE:
            break;
        case TV_STRENGTH:
            if (space->getTV(h).getMean() < limit) okToApply = false;
            break;
        case STI:
            if (space->getNormalisedZeroToOneSTI(h,false,true) < limit) okToApply = false;
        }
        if (okToApply) {
            boost::shared_ptr<Atom> a = space->cloneAtom(h);
            if (space->inheritsType(t, LINK)) {
                boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
                const std::vector<Handle> &out = l->getOutgoingSet();
                if (compact && out.size() == 2 && l->getIncomingSet() == NULL) {
                    ubigraph_change_edge_style(h.value(), style);
                } else
                    ubigraph_change_vertex_style(h.value(), style);
            } else 
                ubigraph_change_vertex_style(h.value(), style);
        }
    }
}

void Ubigrapher::applyStyleToHandleSeq(HandleSeq hs, int style)
{
    if (!isConnected()) return;
    // For each, get prop, scale... and 
    foreach (Handle h, hs) {
        boost::shared_ptr<Atom> a = space->cloneAtom(h);
        if (!a) continue;
        if (space->inheritsType(a->getType(), LINK)) {
            boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
            const std::vector<Handle> &out = l->getOutgoingSet();
            if (compact && out.size() == 2 && l->getIncomingSet() == NULL) {
                ubigraph_change_edge_style(h.value(), style);
            } else
                ubigraph_change_vertex_style(h.value(), style);
        } else 
            ubigraph_change_vertex_style(h.value(), style);
    }
}
bool Ubigrapher::addVertex(Handle h)
{
	// Policy: don't display PLN's FWVariableNodes, because it's annoying
	if (space->inheritsType(space->getType(h), FW_VARIABLE_NODE)) return false;

    if (!isConnected()) return false;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);
    bool isNode = space->isNode(a->getType());

    int id = (int)h.value();

    if (isNode) {
        int status = ubigraph_new_vertex_w_id(id);
        if (status)
            logger().error("Status was %d", status);
        ubigraph_change_vertex_style(id, nodeStyle);
    } else {
        boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
        if (l && compact && l->getOutgoingSet().size() == 2 && l->getIncomingSet() == NULL)
            return false;
        int status = ubigraph_new_vertex_w_id(id);
        if (status)
            logger().error("Status was %d", status);
        ubigraph_change_vertex_style(id, linkStyle);
    }

    if (labelsOn) {
        std::ostringstream ost;
        std::string type = classserver().getTypeName(a->getType());
        if (compactLabels) {
            ost << initials(type);
        } else {
            ost << type;
        }
        
        if (isNode) {
            boost::shared_ptr<Node> n = boost::shared_dynamic_cast<Node>(a);
            ost << " " << n->getName();
        } /*else {
            boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
            l = l; // TODO: anything to output for links?
        }*/
        ost << ":" << space->getTV(h).getMean();
        ubigraph_set_vertex_attribute(id, "label", ost.str().c_str());
    }
    return false;
}

/**
 * Outputs ubigraph links for an atom's outgoing connections.
 */
bool Ubigrapher::addEdges(Handle h)
{
    if (!isConnected()) return false;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);

    usleep(pushDelay);
    boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
    if (l)
    {
        const std::vector<Handle> &out = l->getOutgoingSet();
        
//            int id = ;// make IDs based on the type and outgoing set, in case
//            // it's later necessary to change this edge
//            int status = ubigraph_new_edge_w_id(id,x,y);

        if (compact && out.size() == 2 && l->getIncomingSet() == NULL)
        {
            int id = h.value();
            int status = ubigraph_new_edge_w_id(id, out[0].value(),out[1].value());
            if (status)
                logger().error("Status was %d", status);
            
            int style = compactLinkStyle;
            if (classserver().isA(a->getType(), ORDERED_LINK))
                style = compactLinkStyleDirected;
            ubigraph_change_edge_style(id, style);
            if (labelsOn) {
                std::string type = classserver().getTypeName(a->getType());
                std::ostringstream ost;
                if (compactLabels) {
                    ost << initials(type);
                } else {
                    ost << type;
                }
                ost << ":" << space->getTV(h).getMean();
                ubigraph_set_edge_attribute(id, "label", ost.str().c_str());
            }
            return false;
        } else {
            int style = outgoingStyle;
            if (classserver().isA(a->getType(), ORDERED_LINK))
                style = outgoingStyleDirected;
            for (size_t i = 0; i < out.size(); i++) {
                int id = ubigraph_new_edge(h.value(),out[i].value());
                ubigraph_change_edge_style(id, style);
                //ubigraph_set_edge_attribute(id, "label", toString(i).c_str());
            }
        }
    }

/*        if (withIncoming) {
        HandleEntry *he = a->getIncomingSet();
        int i = 0;
        while (he) {
//                ost << h << "->" << he->handle << " [style=\"dotted\" label=\"" << i << "\"];\n";
            he = he->next;
            i++;
        }
    }*/
    return false;
}

/**
 * Removes the ubigraph node for an atom.
 */
bool Ubigrapher::removeVertex(Handle h)
{
    if (!isConnected()) return false;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);

    if (compact)
    {
        // Won't have made a node for a binary link with no incoming
        boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
        if (l && l->getOutgoingSet().size() == 2 &&
                 l->getIncomingSet() == NULL)
            return false;
    }

    int id = (int)h.value();
    int status = ubigraph_remove_vertex(id);
    if (status)
        logger().error("Status was %d", status);

    return false;
}

bool Ubigrapher::removeEdges(Handle h)
{
    if (!isConnected()) return false;
    boost::shared_ptr<Atom> a = space->cloneAtom(h);

    // This method is only relevant to binary Links with no incoming.
    // Any other atoms will be represented by vertexes, and the edges
    // to them will be automatically deleted by ubigraph when the
    // vertexes are deleted.
    if (compact)
    {
        boost::shared_ptr<Link> l = boost::shared_dynamic_cast<Link>(a);
        if (l && l->getOutgoingSet().size() == 2 &&
                 l->getIncomingSet() == NULL)
        {                     
            int id = h.value();
            int status = ubigraph_remove_edge(id);
            if (status)
                logger().error("Status was %d", status);
        }
    }
    return false;
}

void Ubigrapher::graph()
{
    if (!isConnected()) return;
    ubigraph_clear();
    setStyles();
    space->foreach_handle_of_type((Type)ATOM, &Ubigrapher::addVertex, this, true);
    space->foreach_handle_of_type((Type)ATOM, &Ubigrapher::addEdges, this, true);
}


} // namespace opencog
