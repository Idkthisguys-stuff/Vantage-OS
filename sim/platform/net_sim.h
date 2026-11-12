#pragma once
#include <vantage/kernel/net.h>

/* The virtual network (net_peer.h) as the simulator's Wi-Fi link. */
const NetLink* sim_net_link(void);
/* The same network through a wired port: 1000 Mbit/s while the cable is in. */
const NetLink* sim_net_ethernet_link(void);
/* Resets the virtual network and attaches both links, cable unplugged. */
void sim_net_attach(void);
void sim_net_set_cable(int up);
/* Simulates losing the access point; a later join may succeed again. */
void sim_net_drop_link(void);
