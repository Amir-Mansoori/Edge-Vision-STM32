/**
 * @file    ping.h
 * @brief   Public interface of the lwIP-based ICMP ping client (board -> PC).
 *
 * Typical use:
 *   - ping_init()                 once, after MX_LWIP_Init()
 *   - ping_poll()                 on every iteration of the main loop
 *   - ping_start() / ping_stop()  enable or disable the periodic ping
 *
 * Results are reported over UART, one line per ping:
 *   "PING seq=12 rtt=1 ms"   or   "PING seq=12 timeout"
 */

#ifndef PING_H
#define PING_H

#include <stdint.h>
#include "lwip/ip_addr.h"

/** IP address of the PC that the board pings. Must be on the board's subnet. */
#define HOST_IP "192.168.1.100"

/**
 * @brief Create the raw ICMP PCB and register the receive callback.
 * @note  Call once, after MX_LWIP_Init() (lwIP must already be running).
 */
void ping_init(void);

/**
 * @brief Send one ICMP echo request and start waiting for its reply.
 * @param ip_h destination address (normally the PC, see HOST_IP)
 */
void ping_send(ip_addr_t ip_h);

/** @brief Enable the periodic ping (one request per second, see ping_poll()). */
void ping_start();

/** @brief Disable the periodic ping. A request already in flight still completes. */
void ping_stop();

/**
 * @brief Non-blocking housekeeping: report timeouts and send the next request.
 * @note  Call from the main loop, next to MX_LWIP_Process().
 */
void ping_poll(void);
#endif
