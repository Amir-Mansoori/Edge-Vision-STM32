#ifndef PING_H
#define PING_H

#include <stdint.h>
#include "lwip/ip_addr.h"
#define host_ip_A 192
#define host_ip_B 168
#define host_ip_C 1
#define host_ip_D 100
#define HOST_IP "192.168.1.100"

void ping_init(void);
void ping_send(ip_addr_t ip_h);
void ping_start();
void ping_stop();
void ping_poll(void);
#endif
