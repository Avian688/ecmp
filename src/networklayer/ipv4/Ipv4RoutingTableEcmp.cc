//
// Copyright (C) 2004-2006 Andras Varga
// Copyright (C) 2000 Institut fuer Telematik, Universitaet Karlsruhe
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

//  Cleanup and rewrite: Andras Varga, 2004

#include <algorithm>
#include <sstream>

#include <inet/common/INETUtils.h>
#include <inet/common/ModuleAccess.h>
#include <inet/common/PatternMatcher.h>
#include <inet/common/Simsignals.h>
#include <inet/common/lifecycle/ModuleOperations.h>
#include <inet/common/lifecycle/NodeStatus.h>
#include <inet/networklayer/contract/IInterfaceTable.h>
#include <inet/networklayer/ipv4/Ipv4InterfaceData.h>
#include <inet/networklayer/ipv4/Ipv4Route.h>
#include <inet/networklayer/ipv4/RoutingTableParser.h>
#include <inet/common/packet/Packet.h>
#include <inet/networklayer/ipv4/Ipv4Header_m.h>
#include <inet/networklayer/common/L3Tools.h>

#ifdef WITH_IPv4
#  include "inet/networklayer/ipv4/Ipv4Header_m.h"
#endif
#ifdef WITH_IPv6
#  include "inet/networklayer/ipv6/Ipv6Header.h"
#endif
#ifdef WITH_TCP_COMMON
#  include "inet/transportlayer/tcp_common/TcpHeader.h"
#endif
#ifdef WITH_UDP
#  include "inet/transportlayer/udp/UdpHeader_m.h"
#endif

#include "Ipv4RoutingTableEcmp.h"

namespace inet {

using namespace utils;

Define_Module(Ipv4RoutingTableEcmp);

std::ostream& operator<<(std::ostream &os, const Ipv4Route &e)
{
    os << e.str();
    return os;
}
;

std::ostream& operator<<(std::ostream &os, const Ipv4MulticastRoute &e)
{
    os << e.str();
    return os;
}
;

Ipv4RoutingTableEcmp::~Ipv4RoutingTableEcmp()
{
    for (auto &elem : routes)
        delete elem;
    for (auto &elem : multicastRoutes)
        delete elem;
}

void Ipv4RoutingTableEcmp::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        // get a pointer to the host module and IInterfaceTable
        cModule *host = getContainingNode(this);
        host->subscribe(interfaceCreatedSignal, this);
        host->subscribe(interfaceDeletedSignal, this);
        host->subscribe(interfaceStateChangedSignal, this);
        host->subscribe(interfaceConfigChangedSignal, this);
        host->subscribe(interfaceIpv4ConfigChangedSignal, this);

        ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);

        netmaskRoutes = par("netmaskRoutes");
        forwarding = par("forwarding");
        multicastForward = par("multicastForwarding");
        useAdminDist = par("useAdminDist");
        perFlowEcmp = par("perFlowEcmp");

        WATCH_PTRVECTOR(routes);
        WATCH_PTRVECTOR(multicastRoutes);
        WATCH(netmaskRoutes);
        WATCH(forwarding);
        WATCH(multicastForward);
        WATCH(routerId);
    }
    else if (stage == INITSTAGE_ROUTER_ID_ASSIGNMENT) {
        cModule *node = findContainingNode(this);
        NodeStatus *nodeStatus = node ? check_and_cast_nullable<NodeStatus*>(node->getSubmodule("status")) : nullptr;
        isNodeUp = !nodeStatus || nodeStatus->getState() == NodeStatus::UP;
        if (isNodeUp) {
            // set routerId if param is not "" (==no routerId) or "auto" (in which case we'll
            // do it later in a later stage, after network configurators configured the interfaces)
            const char *routerIdStr = par("routerId");
            if (strcmp(routerIdStr, "") && strcmp(routerIdStr, "auto"))
                routerId = Ipv4Address(routerIdStr);
            // read routing table file (and interface configuration)
            const char *filename = par("routingFile");
            RoutingTableParser parser(ift, this);
            if (*filename && parser.readRoutingTableFromFile(filename) == -1)
                throw cRuntimeError("Error reading routing table file %s", filename);
            // routerID selection must be after network autoconfiguration assigned interface addresses
            configureRouterId();
        }
    }
    else if (stage == INITSTAGE_NETWORK_LAYER) {
        // we don't use notifications during initialize(), so we do it manually.
        updateNetmaskRoutes();
    }
}

void Ipv4RoutingTableEcmp::configureRouterId()
{
    if (routerId.isUnspecified()) {    // not yet configured
        const char *routerIdStr = par("routerId");
        if (!strcmp(routerIdStr, "auto")) {    // non-"auto" cases already handled earlier
            // choose highest interface address as routerId
            for (int i = 0; i < ift->getNumInterfaces(); ++i) {
                NetworkInterface *ie = ift->getInterface(i);
                if (!ie->isLoopback()) {
                    auto ipv4Data = ie->findProtocolData<Ipv4InterfaceData>();
                    if (ipv4Data && ipv4Data->getIPAddress().getInt() > routerId.getInt()) {
                        routerId = ipv4Data->getIPAddress();
                    }
                }
            }
        }
    }
    else {    // already configured
              // if there is no interface with routerId yet, assign it to the loopback address;
              // TODO find out if this is a good practice, in which situations it is useful etc.
        if (getInterfaceByAddress(routerId) == nullptr) {
            NetworkInterface *lo0 = CHK(ift->findFirstLoopbackInterface());
            auto ipv4Data = lo0->getProtocolDataForUpdate<Ipv4InterfaceData>();
            ipv4Data->setIPAddress(routerId);
            ipv4Data->setNetmask(Ipv4Address::ALLONES_ADDRESS);
        }
    }
}

void Ipv4RoutingTableEcmp::refreshDisplay() const
{
    char buf[80];
    if (routerId.isUnspecified())
        sprintf(buf, "%d+%d routes", (int) routes.size(), (int) multicastRoutes.size());
    else
        sprintf(buf, "routerId: %s\n%d+%d routes", routerId.str().c_str(), (int) routes.size(), (int) multicastRoutes.size());
    getDisplayString().setTagArg("t", 0, buf);
}

void Ipv4RoutingTableEcmp::handleMessage(cMessage *msg)
{
    throw cRuntimeError("This module doesn't process messages");
}

void Ipv4RoutingTableEcmp::receiveSignal(cComponent *source, simsignal_t signalID, cObject *obj, cObject *details)
{
    if (getSimulation()->getContextType() == CTX_INITIALIZE)
        return; // ignore notifications during initialize

    Enter_Method("%s", cComponent::getSignalName(signalID));

    printSignalBanner(signalID, obj, details);

    if (signalID == interfaceCreatedSignal) {
        // add netmask route for the new interface
        updateNetmaskRoutes();
        invalidateCache();
    }
    else if (signalID == interfaceDeletedSignal) {
        // remove all routes that point to that interface
        const NetworkInterface *entry = check_and_cast<const NetworkInterface*>(obj);
        deleteInterfaceRoutes(entry);
        invalidateCache();
    }
    else if (signalID == interfaceStateChangedSignal) {
        invalidateCache();
        const auto *ieChangeDetails = check_and_cast<const NetworkInterfaceChangeDetails*>(obj);
        auto fieldId = ieChangeDetails->getFieldId();
        if (fieldId == NetworkInterface::F_STATE || fieldId == NetworkInterface::F_CARRIER) {
            const auto *entry = ieChangeDetails->getNetworkInterface();
            updateNetmaskRoutes();
            if (!entry->isUp())
                deleteInterfaceRoutes(entry);
            invalidateCache();
        }
    }
    else if (signalID == interfaceConfigChangedSignal) {
        invalidateCache();
    }
    else if (signalID == interfaceIpv4ConfigChangedSignal) {
        // if anything Ipv4-related changes in the interfaces, interface netmask
        // based routes have to be re-built.
        updateNetmaskRoutes();
        invalidateCache();
    }
}

cModule* Ipv4RoutingTableEcmp::getHostModule()
{
    return findContainingNode(this);
}

void Ipv4RoutingTableEcmp::deleteInterfaceRoutes(const NetworkInterface *entry)
{
    // delete unicast routes using this interface
    for (auto it = routes.begin(); it != routes.end();) {
        Ipv4Route *route = *it;
        if (route->getInterface() == entry) {
            it = routes.erase(it);
            invalidateCache();
            ASSERT(route->getRoutingTable() == this);    // still filled in, for the listeners' benefit
            emit(routeDeletedSignal, route);
            delete route;
        }
        else
            ++it;
    }

    // delete or update multicast routes:
    //   1. delete routes has entry as input interface
    //   2. remove entry from output interface list
    for (auto it = multicastRoutes.begin(); it != multicastRoutes.end();) {
        Ipv4MulticastRoute *route = *it;
        if (route->getInInterface() && route->getInInterface()->getInterface() == entry) {
            it = multicastRoutes.erase(it);
            invalidateCache();
            ASSERT(route->getRoutingTable() == this);    // still filled in, for the listeners' benefit
            emit(mrouteDeletedSignal, route);
            delete route;
        }
        else {
            if (route->removeOutInterface(entry)) {
                invalidateCache();
                emit(mrouteChangedSignal, route);
            }
            ++it;
        }
    }
}

void Ipv4RoutingTableEcmp::invalidateCache()
{
    routingCache.clear();
    localBroadcastAddresses.clear();
}

void Ipv4RoutingTableEcmp::printRoutingTable() const
{
    EV << "-- Routing table --\n";
    EV << stringf("%-16s %-16s %-16s %-4s %-16s %s\n", "Destination", "Netmask", "Gateway", "Iface", "", "Metric");

    for (int i = 0; i < getNumRoutes(); i++) {
        Ipv4Route *route = getRoute(i);
        NetworkInterface *interfacePtr = route->getInterface();
        EV
                << stringf("%-16s %-16s %-16s %-4s %-16s %6d\n", route->getDestination().isUnspecified() ? "*" : route->getDestination().str().c_str(), route->getNetmask().isUnspecified() ? "*" : route->getNetmask().str().c_str(),
                        route->getGateway().isUnspecified() ? "*" : route->getGateway().str().c_str(), !interfacePtr ? "*" : interfacePtr->getInterfaceName(),
                        !interfacePtr ? "(*)" : (std::string("(") + interfacePtr->getProtocolData<Ipv4InterfaceData>()->getIPAddress().str() + ")").c_str(), route->getMetric());
    }
    EV << "\n";
}

void Ipv4RoutingTableEcmp::printMulticastRoutingTable() const
{
    EV << "-- Multicast routing table --\n";
    EV << stringf("%-16s %-16s %-16s %-6s %-6s %s\n", "Source", "Netmask", "Group", "Metric", "In", "Outs");

    for (int i = 0; i < getNumMulticastRoutes(); i++) {
        Ipv4MulticastRoute *route = getMulticastRoute(i);
        EV
                << stringf("%-16s %-16s %-16s %-6d %-6s ", route->getOrigin().isUnspecified() ? "*" : route->getOrigin().str().c_str(), route->getOriginNetmask().isUnspecified() ? "*" : route->getOriginNetmask().str().c_str(),
                        route->getMulticastGroup().isUnspecified() ? "*" : route->getMulticastGroup().str().c_str(), route->getMetric(), !route->getInInterface() ? "*" : route->getInInterface()->getInterface()->getInterfaceName());
        for (unsigned int i = 0; i < route->getNumOutInterfaces(); i++) {
            if (i != 0)
                EV << ",";
            EV << route->getOutInterface(i)->getInterface()->getInterfaceName();
        }
        EV << "\n";
    }
    EV << "\n";
}

std::vector<Ipv4Address> Ipv4RoutingTableEcmp::gatherAddresses() const
{
    std::vector<Ipv4Address> addressvector;

    for (int i = 0; i < ift->getNumInterfaces(); ++i)
        addressvector.push_back(ift->getInterface(i)->getProtocolData<Ipv4InterfaceData>()->getIPAddress());
    return addressvector;
}

//---

NetworkInterface* Ipv4RoutingTableEcmp::getInterfaceByAddress(const Ipv4Address &addr) const
{
    Enter_Method
    ("getInterfaceByAddress(%u.%u.%u.%u)", addr.getDByte(0), addr.getDByte(1), addr.getDByte(2), addr.getDByte(3));    // note: str().c_str() too slow here

    if (addr.isUnspecified())
        return nullptr;
    for (int i = 0; i < ift->getNumInterfaces(); ++i) {
        NetworkInterface *ie = ift->getInterface(i);
        if (ie->hasNetworkAddress(addr))
            return ie;
    }
    return nullptr;
}

//---

bool Ipv4RoutingTableEcmp::isLocalAddress(const Ipv4Address &dest) const
{
    Enter_Method
    ("isLocalAddress(%u.%u.%u.%u)", dest.getDByte(0), dest.getDByte(1), dest.getDByte(2), dest.getDByte(3));    // note: str().c_str() too slow here

    for (int i = 0; i < ift->getNumInterfaces(); i++) {
        if (ift->getInterface(i)->hasNetworkAddress(dest))
            return true;
    }
    return false;
}

// JcM add: check if the dest addr is local network broadcast
bool Ipv4RoutingTableEcmp::isLocalBroadcastAddress(const Ipv4Address &dest) const
{
    Enter_Method
    ("isLocalBroadcastAddress(%u.%u.%u.%u)", dest.getDByte(0), dest.getDByte(1), dest.getDByte(2), dest.getDByte(3));    // note: str().c_str() too slow here

    if (localBroadcastAddresses.empty()) {
        // collect interface addresses if not yet done
        for (int i = 0; i < ift->getNumInterfaces(); i++) {
            NetworkInterface *ie = ift->getInterface(i);
            if (!ie->isBroadcast())
                continue;
            Ipv4Address interfaceAddr = ie->getProtocolData<Ipv4InterfaceData>()->getIPAddress();
            Ipv4Address broadcastAddr = interfaceAddr.makeBroadcastAddress(ie->getProtocolData<Ipv4InterfaceData>()->getNetmask());
            if (!broadcastAddr.isUnspecified()) {
                localBroadcastAddresses.insert(broadcastAddr);
            }
        }
    }

    auto it = localBroadcastAddresses.find(dest);
    return it != localBroadcastAddresses.end();
}

NetworkInterface* Ipv4RoutingTableEcmp::findInterfaceByLocalBroadcastAddress(const Ipv4Address &dest) const
{
    for (int i = 0; i < ift->getNumInterfaces(); i++) {
        NetworkInterface *ie = ift->getInterface(i);
        if (!ie->isBroadcast())
            continue;
        Ipv4Address interfaceAddr = ie->getProtocolData<Ipv4InterfaceData>()->getIPAddress();
        Ipv4Address broadcastAddr = interfaceAddr.makeBroadcastAddress(ie->getProtocolData<Ipv4InterfaceData>()->getNetmask());
        if (broadcastAddr == dest)
            return ie;
    }
    return nullptr;
}

bool Ipv4RoutingTableEcmp::isLocalMulticastAddress(const Ipv4Address &dest) const
{
    Enter_Method
    ("isLocalMulticastAddress(%u.%u.%u.%u)", dest.getDByte(0), dest.getDByte(1), dest.getDByte(2), dest.getDByte(3));    // note: str().c_str() too slow here

    for (int i = 0; i < ift->getNumInterfaces(); i++) {
        NetworkInterface *ie = ift->getInterface(i);
        if (ie->getProtocolData<Ipv4InterfaceData>()->isMemberOfMulticastGroup(dest))
            return true;
    }
    return false;
}

void Ipv4RoutingTableEcmp::purge()
{
    // purge unicast routes
    for (auto it = routes.begin(); it != routes.end();) {
        Ipv4Route *route = *it;
        if (route->isValid())
            ++it;
        else {
            it = routes.erase(it);
            invalidateCache();
            ASSERT(route->getRoutingTable() == this);    // still filled in, for the listeners' benefit
            emit(routeDeletedSignal, route);
            delete route;
        }
    }

    // purge multicast routes
    for (auto it = multicastRoutes.begin(); it != multicastRoutes.end();) {
        Ipv4MulticastRoute *route = *it;
        if (route->isValid())
            ++it;
        else {
            it = multicastRoutes.erase(it);
            invalidateCache();
            ASSERT(route->getRoutingTable() == this);    // still filled in, for the listeners' benefit
            emit(mrouteDeletedSignal, route);
            delete route;
        }
    }
}

Ipv4Route* Ipv4RoutingTableEcmp::findBestMatchingRoute(const Ipv4Address &dest) const
{
    Ipv4Route *bestRoute = nullptr;
    return bestRoute;
}

Ipv4Route* Ipv4RoutingTableEcmp::findBestMatchingRouteEcmp(Packet *packet, const Ipv4Address &dest)
{
    Enter_Method
    ("findBestMatchingRoute(%u.%u.%u.%u)", dest.getDByte(0), dest.getDByte(1), dest.getDByte(2), dest.getDByte(3)); // note: str().c_str() too slow here

    if(perFlowEcmp){
        return perFlowBestMatchingRouteEcmp(packet, dest);
    }
    else{
        return perPacketBestMatchingRouteEcmp(packet, dest);
    }
}

Ipv4Route* Ipv4RoutingTableEcmp::perPacketBestMatchingRouteEcmp(Packet *packet, const Ipv4Address &dest)
{
    //Added MOH ==> 5 tuple hashing
    int ipProtocol = -1;
    b ipHeaderLength = b(-1);
    unsigned int srcPort = 0;
    unsigned int destPort = 0;
    #ifdef WITH_IPv4
        if (packet->getTag<PacketProtocolTag>()->getProtocol() == &Protocol::ipv4) {
            const auto& ipv4Header = packet->peekAtFront<Ipv4Header>();
            ipProtocol = ipv4Header->getProtocolId();
            ipHeaderLength = ipv4Header->getChunkLength();
        }
    #endif
    #ifdef WITH_IPv6
        if (packet->getTag<PacketProtocolTag>()->getProtocol() == &Protocol::ipv6) {
            const auto& ipv6Header = packet->peekAtFront<Ipv6Header>();
            ipProtocol = ipv6Header->getProtocolId();
            ipHeaderLength = ipv6Header->getChunkLength();
        }
    #endif
    #ifdef WITH_UDP
        if (ipProtocol == IP_PROT_UDP) {
            const auto& udpHeader = packet->peekDataAt<UdpHeader>(ipHeaderLength);
            srcPort = udpHeader->getSourcePort();
            destPort = udpHeader->getDestinationPort();
        }
    #endif
    #ifdef WITH_TCP_COMMON
        if (ipProtocol == IP_PROT_TCP) {
            const auto& tcpHeader = packet->peekDataAt<tcp::TcpHeader>(ipHeaderLength);
            srcPort = tcpHeader->getSourcePort();
            destPort = tcpHeader->getDestinationPort();
        }
    #endif
    const auto& ipv4Header = packet->peekAtFront<Ipv4Header>();
    Ipv4Address destAddr = ipv4Header->getDestAddress();
    Ipv4Address srcAddr = ipv4Header->getSrcAddress();
    std::string routerName = getSimulation()->getContextModule()->getFullPath();
    std::string packetName = packet->getFullName();
    std::hash<std::string> hash;
    //concatenation
    std::string key = srcAddr.str() + destAddr.str() + std::to_string(ipProtocol) + routerName + packetName;
    size_t hashValue = hash(key);




    //std::string routerName = getSimulation()->getContextModule()->getFullPath();
    Ipv4Route *bestRoute = nullptr;
    range = routingCache.equal_range(dest);                     // slower
    int numPossibleEcmpRoutesCaches = std::distance(range.first, range.second);                     // faster
    //std::cout << "numPossible routes " << numPossibleEcmpRoutesCaches << endl;
    if (numPossibleEcmpRoutesCaches == 0) {
        EV_DETAIL << "Finding best match" << endl;
        for (auto e : routes) {
            if (e->isValid()) {
                EV_DETAIL << e->getDestination() << endl;
                if (Ipv4Address::maskedAddrAreEqual(dest, e->getDestination(), e->getNetmask())) { // match
                    bestRoute = const_cast<Ipv4Route*>(e);
                    routingCache.insert(std::pair<Ipv4Address, Ipv4Route*>(dest, bestRoute));
                }
            }
        }
        range = routingCache.equal_range(dest);
        numPossibleEcmpRoutesCaches = routingCache.count(dest);
    }
    if (numPossibleEcmpRoutesCaches == 1) {
        auto it = range.first;
        return it->second; // or return range.first->second;
    }
    if (numPossibleEcmpRoutesCaches > 1) {
        //std::cout << packet->getFullName() << endl;
        unsigned int hashValueIII = static_cast<int>(hashValue);
        //std::cout << "hash value " << hashValueIII << endl;
        int selected= hashValueIII%numPossibleEcmpRoutesCaches ; // per-flow ECMP
        //std::cout << "selected value " << selected << endl;
        //int selected = ((rand() % numPossibleEcmpRoutesCaches)); // per-packet ECMP spraying pkts
        int i = 0;
        for (auto it = range.first; it != range.second; ++it) {
            if (i == selected) {
                return it->second;
            }
            i++;
        }
    }
    return bestRoute; // shouldn't be reached
}

Ipv4Route* Ipv4RoutingTableEcmp::perFlowBestMatchingRouteEcmp(Packet *packet, const Ipv4Address &dest)
{
    //Added MOH ==> 5 tuple hashing
    int ipProtocol = -1;
    b ipHeaderLength = b(-1);
    unsigned int srcPort = 0;
    unsigned int destPort = 0;
    #ifdef WITH_IPv4
        if (packet->getTag<PacketProtocolTag>()->getProtocol() == &Protocol::ipv4) {
            const auto& ipv4Header = packet->peekAtFront<Ipv4Header>();
            ipProtocol = ipv4Header->getProtocolId();
            ipHeaderLength = ipv4Header->getChunkLength();
        }
    #endif
    #ifdef WITH_IPv6
        if (packet->getTag<PacketProtocolTag>()->getProtocol() == &Protocol::ipv6) {
            const auto& ipv6Header = packet->peekAtFront<Ipv6Header>();
            ipProtocol = ipv6Header->getProtocolId();
            ipHeaderLength = ipv6Header->getChunkLength();
        }
    #endif
    #ifdef WITH_UDP
        if (ipProtocol == IP_PROT_UDP) {
            const auto& udpHeader = packet->peekDataAt<UdpHeader>(ipHeaderLength);
            srcPort = udpHeader->getSourcePort();
            destPort = udpHeader->getDestinationPort();
        }
    #endif
    #ifdef WITH_TCP_COMMON
        if (ipProtocol == IP_PROT_TCP) {
            const auto& tcpHeader = packet->peekDataAt<tcp::TcpHeader>(ipHeaderLength);
            srcPort = tcpHeader->getSourcePort();
            destPort = tcpHeader->getDestinationPort();
        }
    #endif
    const auto& ipv4Header = packet->peekAtFront<Ipv4Header>();
    Ipv4Address destAddr = ipv4Header->getDestAddress();
    Ipv4Address srcAddr = ipv4Header->getSrcAddress();
    std::string routerName = getSimulation()->getContextModule()->getFullPath();
    std::hash<std::string> hash;
    //concatenation
    std::string key = srcAddr.str() + destAddr.str() + std::to_string(srcPort)+ std::to_string(destPort) + std::to_string(ipProtocol)+routerName;
    size_t hashValue = hash(key);
    Ipv4Route *bestRoute = nullptr;
    range = routingCache.equal_range(dest);                 // slower
    int numPossibleEcmpRoutesCaches = std::distance(range.first, range.second);// faster
    if (numPossibleEcmpRoutesCaches == 0) {
        for (auto e : routes) {
            EV_DETAIL << "hiiii find best match" << e->getDestination() << "\n";
            if (e->isValid()) {
                if (Ipv4Address::maskedAddrAreEqual(dest, e->getDestination(), e->getNetmask())) { // match
                    bestRoute = const_cast<Ipv4Route *>(e);
                    routingCache.insert (std::pair<Ipv4Address, Ipv4Route *>(dest,bestRoute));
                    // MOH: at the edge router  the routing table has two options either to route to the client ip address(10.0.0.1) or to the client's subnet address (10.0.0.0)
                    // both routes are through the same interface and they have unspecified gateway
                    // so if the gateway is unspecified just consider one of the above routes as we need don't want  this case to be considered as a multiple paths
                    // TODO: can be improved
                    if (e->getGateway().isUnspecified()) break;// just take the already found route (e.g. dest addrs= 10.0.0.1, gateway=unspecified , Iface = ppp1 )
                }
            }
        }
        range = routingCache.equal_range(dest);
        numPossibleEcmpRoutesCaches= routingCache.count(dest);
    }
    if(numPossibleEcmpRoutesCaches==1) {
         auto it = range.first;
         return it->second; // or return range.first->second;
    }
    if (numPossibleEcmpRoutesCaches > 1) {
        unsigned  int hashValueIII = static_cast<int>(hashValue) ;
        int selected= hashValueIII%numPossibleEcmpRoutesCaches ; // per-flow ECMP
        int i=0;
        for (auto it = range.first; it != range.second; ++it) {
            if ( i == selected) {
                return it->second;
            }
            i++;
        }
    }
    return bestRoute; // shouldn't be reached
}

NetworkInterface* Ipv4RoutingTableEcmp::getInterfaceForDestAddr(const Ipv4Address &dest) const
{
    Enter_Method
    ("getInterfaceForDestAddr(%u.%u.%u.%u)", dest.getDByte(0), dest.getDByte(1), dest.getDByte(2), dest.getDByte(3));    // note: str().c_str() too slow here

    const Ipv4Route *e = findBestMatchingRoute(dest);
    return e ? e->getInterface() : nullptr;
}

Ipv4Address Ipv4RoutingTableEcmp::getGatewayForDestAddr(const Ipv4Address &dest) const
{
    Enter_Method
    ("getGatewayForDestAddr(%u.%u.%u.%u)", dest.getDByte(0), dest.getDByte(1), dest.getDByte(2), dest.getDByte(3));    // note: str().c_str() too slow here

    const Ipv4Route *e = findBestMatchingRoute(dest);
    return e ? e->getGateway() : Ipv4Address();
}

const Ipv4MulticastRoute* Ipv4RoutingTableEcmp::findBestMatchingMulticastRoute(const Ipv4Address &origin, const Ipv4Address &group) const
{
    Enter_Method
    ("getMulticastRoutesFor(%u.%u.%u.%u, %u.%u.%u.%u)", origin.getDByte(0), origin.getDByte(1), origin.getDByte(2), origin.getDByte(3), group.getDByte(0), group.getDByte(1), group.getDByte(2), group.getDByte(3)); // note: str().c_str() too slow here here

    // TODO caching?

    for (auto e : multicastRoutes) {
        if (e->isValid() && e->matches(origin, group))
            return e;
    }

    return nullptr;
}

Ipv4Route* Ipv4RoutingTableEcmp::getRoute(int k) const
{
    if (k < (int) routes.size())
        return routes[k];
    return nullptr;
}

Ipv4Route* Ipv4RoutingTableEcmp::getDefaultRoute() const
{
    // if exists default route entry, it is the last valid entry
    for (RouteVector::const_reverse_iterator i = routes.rbegin(); i != routes.rend() && (*i)->getNetmask().isUnspecified(); ++i) {
        if ((*i)->isValid())
            return *i;
    }
    return nullptr;
}

// The 'routes' vector stores the routes in this order.
// The best matching route should precede the other matching routes,
// so the method should return true if a is better the b.
bool Ipv4RoutingTableEcmp::routeLessThan(const Ipv4Route *a, const Ipv4Route *b) const
{
    // longer prefixes are better, because they are more specific
    if (a->getNetmask() != b->getNetmask())
        return a->getNetmask() > b->getNetmask();

    if (a->getDestination() != b->getDestination())
        return a->getDestination() < b->getDestination();

    // for the same destination/netmask:

    // smaller administration distance is better (if useAdminDist)
    if (useAdminDist && (a->getAdminDist() != b->getAdminDist()))
        return a->getAdminDist() < b->getAdminDist();

    // smaller metric is better
    return a->getMetric() < b->getMetric();
}

void Ipv4RoutingTableEcmp::setRouterId(Ipv4Address a)
{
    routerId = a;
}

void Ipv4RoutingTableEcmp::internalAddRoute(Ipv4Route *entry)
{
    if (!entry->getNetmask().isValidNetmask())
        throw cRuntimeError("addRoute(): wrong netmask %s in route", entry->getNetmask().str().c_str());

    if (entry->getNetmask().getInt() != 0 && (entry->getDestination().getInt() & entry->getNetmask().getInt()) == 0)
        throw cRuntimeError("addRoute(): all bits of destination address %s is 0 inside non zero netmask %s", entry->getDestination().str().c_str(), entry->getNetmask().str().c_str());

    if ((entry->getDestination().getInt() & ~entry->getNetmask().getInt()) != 0)
        throw cRuntimeError("addRoute(): suspicious route: destination IP address %s has bits set outside netmask %s", entry->getDestination().str().c_str(), entry->getNetmask().str().c_str());

    // check that the interface exists
    if (!entry->getInterface())
        throw cRuntimeError("addRoute(): interface cannot be nullptr");

    // if this is a default route, remove old default route (we're replacing it)
    if (entry->getNetmask().isUnspecified()) {
        Ipv4Route *oldDefaultRoute = getDefaultRoute();
        if (oldDefaultRoute != nullptr)
            deleteRoute(oldDefaultRoute);
    }

    // The 'routes' vector may contain multiple routes with the same destination/netmask.
    // Routes are stored in descending netmask length and ascending administrative_distance/metric order,
    // so the first matching is the best one.
    // XXX Should only the route with the best metic be stored? Then the worse route should be deleted and
    //     internalAddRoute() should return a bool indicating if it was successful.

    // add to tables
    // we keep entries sorted by netmask desc, metric asc in routeList, so that we can
    // stop at the first match when doing the longest netmask matching
    auto pos = upper_bound(routes.begin(), routes.end(), entry, RouteLessThan(*this));
    routes.insert(pos, entry);
    invalidateCache();
    entry->setRoutingTable(this);
}

void Ipv4RoutingTableEcmp::addRoute(Ipv4Route *entry)
{
    Enter_Method
    ("addRoute(...)");
    // This method should be called before calling entry->str()
    internalAddRoute(entry);
    EV_INFO << "add route " << entry->str() << "\n";
    emit(routeAddedSignal, entry);
}

Ipv4Route* Ipv4RoutingTableEcmp::internalRemoveRoute(Ipv4Route *entry)
{
    auto i = std::find(routes.begin(), routes.end(), entry);
    if (i != routes.end()) {
        routes.erase(i);
        invalidateCache();
        return entry;
    }
    return nullptr;
}

Ipv4Route* Ipv4RoutingTableEcmp::removeRoute(Ipv4Route *entry)
{
    Enter_Method
    ("removeRoute(...)");

    entry = internalRemoveRoute(entry);

    if (entry != nullptr) {
        EV_INFO << "remove route " << entry->str() << "\n";
        ASSERT(entry->getRoutingTable() == this);    // still filled in, for the listeners' benefit
        emit(routeDeletedSignal, entry);
        entry->setRoutingTable(nullptr);
    }
    return entry;
}

bool Ipv4RoutingTableEcmp::deleteRoute(Ipv4Route *entry)    //TODO this is almost duplicate of removeRoute()
{
    Enter_Method
    ("deleteRoute(...)");

    entry = internalRemoveRoute(entry);

    if (entry != nullptr) {
        EV_INFO << "delete route " << entry->str() << "\n";
        ASSERT(entry->getRoutingTable() == this);    // still filled in, for the listeners' benefit
        emit(routeDeletedSignal, entry);
        delete entry;
    }
    return entry != nullptr;
}

bool Ipv4RoutingTableEcmp::multicastRouteLessThan(const Ipv4MulticastRoute *a, const Ipv4MulticastRoute *b)
{
    // We want routes with longer
    // prefixes to be at front, so we compare them as "less".
    if (a->getOriginNetmask() != b->getOriginNetmask())
        return a->getOriginNetmask() > b->getOriginNetmask();

    // For metric, a smaller value is better (we report that as "less").
    if (a->getOrigin() != b->getOrigin())
        return a->getOrigin() < b->getOrigin();

    // put the unspecified group after the specified ones
    if (a->getMulticastGroup() != b->getMulticastGroup())
        return a->getMulticastGroup() > b->getMulticastGroup();

    return a->getMetric() < b->getMetric();
}

void Ipv4RoutingTableEcmp::internalAddMulticastRoute(Ipv4MulticastRoute *entry)
{
    if (!entry->getOriginNetmask().isValidNetmask())
        throw cRuntimeError("addMulticastRoute(): wrong netmask %s in multicast route", entry->getOriginNetmask().str().c_str());

    if ((entry->getOrigin().getInt() & ~entry->getOriginNetmask().getInt()) != 0)
        throw cRuntimeError("addMulticastRoute(): suspicious route: origin IP address %s has bits set outside netmask %s", entry->getOrigin().str().c_str(), entry->getOriginNetmask().str().c_str());

    if (!entry->getMulticastGroup().isUnspecified() && !entry->getMulticastGroup().isMulticast())
        throw cRuntimeError("addMulticastRoute(): group address (%s) is not a multicast address", entry->getMulticastGroup().str().c_str());

    // check that the interface exists
    if (entry->getInInterface() && !entry->getInInterface()->getInterface()->isMulticast())
        throw cRuntimeError("addMulticastRoute(): input interface must be multicast capable");

    for (unsigned int i = 0; i < entry->getNumOutInterfaces(); i++) {
        Ipv4MulticastRoute::OutInterface *outInterface = entry->getOutInterface(i);
        if (!outInterface)
            throw cRuntimeError("addMulticastRoute(): output interface cannot be nullptr");
        else if (!outInterface->getInterface()->isMulticast())
            throw cRuntimeError("addMulticastRoute(): output interface must be multicast capable");
        else if (entry->getInInterface() && outInterface->getInterface() == entry->getInInterface()->getInterface())
            throw cRuntimeError("addMulticastRoute(): output interface cannot be the same as the input interface");
    }

    // add to tables
    // we keep entries sorted by netmask desc, metric asc in routeList, so that we can
    // stop at the first match when doing the longest netmask matching
    auto pos = upper_bound(multicastRoutes.begin(), multicastRoutes.end(), entry, multicastRouteLessThan);
    multicastRoutes.insert(pos, entry);
    invalidateCache();
    entry->setRoutingTable(this);
}

void Ipv4RoutingTableEcmp::addMulticastRoute(Ipv4MulticastRoute *entry)
{
    Enter_Method
    ("addMulticastRoute(...)");
    internalAddMulticastRoute(entry);
    emit(mrouteAddedSignal, entry);
}

Ipv4MulticastRoute* Ipv4RoutingTableEcmp::internalRemoveMulticastRoute(Ipv4MulticastRoute *entry)
{
    auto i = std::find(multicastRoutes.begin(), multicastRoutes.end(), entry);
    if (i != multicastRoutes.end()) {
        multicastRoutes.erase(i);
        invalidateCache();
        return entry;
    }
    return nullptr;
}

Ipv4MulticastRoute* Ipv4RoutingTableEcmp::removeMulticastRoute(Ipv4MulticastRoute *entry)
{
    Enter_Method
    ("removeMulticastRoute(...)");

    entry = internalRemoveMulticastRoute(entry);

    if (entry != nullptr) {
        ASSERT(entry->getRoutingTable() == this);    // still filled in, for the listeners' benefit
        emit(mrouteDeletedSignal, entry);
        entry->setRoutingTable(nullptr);
    }
    return entry;
}

bool Ipv4RoutingTableEcmp::deleteMulticastRoute(Ipv4MulticastRoute *entry)
{
    Enter_Method
    ("deleteMulticastRoute(...)");
    entry = internalRemoveMulticastRoute(entry);
    if (entry != nullptr) {
        ASSERT(entry->getRoutingTable() == this);    // still filled in, for the listeners' benefit
        emit(mrouteDeletedSignal, entry);
        delete entry;
    }
    return entry != nullptr;
}

void Ipv4RoutingTableEcmp::routeChanged(Ipv4Route *entry, int fieldCode)
{
    if (fieldCode == Ipv4Route::F_DESTINATION || fieldCode == Ipv4Route::F_PREFIX_LENGTH || fieldCode == Ipv4Route::F_METRIC) {    // our data structures depend on these fields
        entry = internalRemoveRoute(entry);
        ASSERT(entry != nullptr);    // failure means inconsistency: route was not found in this routing table
        internalAddRoute(entry);
    }
    emit(routeChangedSignal, entry);    // TODO include fieldCode in the notification
}

void Ipv4RoutingTableEcmp::multicastRouteChanged(Ipv4MulticastRoute *entry, int fieldCode)
{
    if (fieldCode == Ipv4MulticastRoute::F_ORIGIN || fieldCode == Ipv4MulticastRoute::F_ORIGINMASK || fieldCode == Ipv4MulticastRoute::F_MULTICASTGROUP || fieldCode == Ipv4MulticastRoute::F_METRIC) // our data structures depend on these fields
            {
        entry = internalRemoveMulticastRoute(entry);
        ASSERT(entry != nullptr);    // failure means inconsistency: route was not found in this routing table
        internalAddMulticastRoute(entry);
    }
    emit(mrouteChangedSignal, entry);    // TODO include fieldCode in the notification
}

void Ipv4RoutingTableEcmp::updateNetmaskRoutes()
{
    // first, delete all routes with src=IFACENETMASK
    for (unsigned int k = 0; k < routes.size(); k++) {
        if (routes[k]->getSourceType() == IRoute::IFACENETMASK) {
            auto it = routes.begin() + (k--);    // '--' is necessary because indices shift down
            Ipv4Route *route = *it;
            routes.erase(it);
            invalidateCache();
            ASSERT(route->getRoutingTable() == this);    // still filled in, for the listeners' benefit
            emit(routeDeletedSignal, route);
            delete route;
        }
    }

    // then re-add them, according to actual interface configuration
    // TODO: say there's a node somewhere in the network that belongs to the interface's subnet
    // TODO: and it is not on the same link, and the gateway does not use proxy ARP, how will packets reach that node?
    PatternMatcher interfaceNameMatcher(netmaskRoutes, false, true, true);
    for (int i = 0; i < ift->getNumInterfaces(); i++) {
        NetworkInterface *ie = ift->getInterface(i);
        if (ie->isUp() && interfaceNameMatcher.matches(ie->getFullName())) {
            auto ipv4Data = ie->findProtocolData<Ipv4InterfaceData>();
            if (ipv4Data && ipv4Data->getNetmask() != Ipv4Address::ALLONES_ADDRESS) {
                Ipv4Route *route = createNewRoute();
                route->setSourceType(IRoute::IFACENETMASK);
                route->setSource(ie);
                route->setDestination(ipv4Data->getIPAddress().doAnd(ipv4Data->getNetmask()));
                route->setNetmask(ipv4Data->getNetmask());
                route->setGateway(Ipv4Address());
                route->setAdminDist(Ipv4Route::dDirectlyConnected);
                route->setMetric(ipv4Data->getMetric());
                route->setInterface(ie);
                route->setRoutingTable(this);
                auto pos = upper_bound(routes.begin(), routes.end(), route, RouteLessThan(*this));
                routes.insert(pos, route);
                invalidateCache();
                emit(routeAddedSignal, route);
            }
        }
    }
}

bool Ipv4RoutingTableEcmp::handleOperationStage(LifecycleOperation *operation, IDoneCallback *doneCallback)
{
    Enter_Method_Silent
    ();
    int stage = operation->getCurrentStage();
    if (dynamic_cast<ModuleStartOperation*>(operation)) {
        if (static_cast<ModuleStartOperation::Stage>(stage) == ModuleStartOperation::STAGE_NETWORK_LAYER) {
            // read routing table file (and interface configuration)
            const char *filename = par("routingFile");
            RoutingTableParser parser(ift, this);
            if (*filename && parser.readRoutingTableFromFile(filename) == -1)
                throw cRuntimeError("Error reading routing table file %s", filename);
        }
        else if (static_cast<ModuleStartOperation::Stage>(stage) == ModuleStartOperation::STAGE_TRANSPORT_LAYER) {
            configureRouterId();
            updateNetmaskRoutes();
            isNodeUp = true;
        }
    }
    else if (dynamic_cast<ModuleStopOperation*>(operation)) {
        if (static_cast<ModuleStopOperation::Stage>(stage) == ModuleStopOperation::STAGE_NETWORK_LAYER) {
            while (!routes.empty())
                delete removeRoute(routes[0]);
            isNodeUp = false;
        }
    }
    else if (dynamic_cast<ModuleCrashOperation*>(operation)) {
        if (static_cast<ModuleCrashOperation::Stage>(stage) == ModuleCrashOperation::STAGE_CRASH) {
            while (!routes.empty())
                delete removeRoute(routes[0]);
            isNodeUp = false;
        }
    }
    return true;
}

Ipv4Route* Ipv4RoutingTableEcmp::createNewRoute()
{
    return new Ipv4Route();
}

} // namespace inet

