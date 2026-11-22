/**
 * @file    ping.c
 * @brief   ICMP echo ("ping") client built on the lwIP raw API.
 *
 * Board -> PC direction of the Ethernet monitor. The firmware sends an ICMP
 * echo request to the PC once per second, waits for the matching echo reply
 * and reports the round-trip time (RTT) over UART in a line format that the
 * PySide6 monitor app parses:
 *
 *      PING seq=12 rtt=1 ms
 *      PING seq=12 timeout
 *
 * How it works (NO_SYS=1: everything runs in the main loop, no RTOS yet):
 *   - ping_init()  creates one raw PCB for IP protocol ICMP and registers
 *                  ping_recv() as its receive callback.
 *   - ping_poll()  is called from the main loop: it detects timeouts and
 *                  starts the next ping while pinging is enabled.
 *   - ping_send()  builds and sends one echo request.
 *   - ping_recv()  is called by lwIP (from inside MX_LWIP_Process()) for every
 *                  ICMP packet that reaches the board, and picks out the reply
 *                  that belongs to the last request.
 *   - ping_start() Enable the periodic ping (picked up by ping_poll() in the main loop).
 *   - ping_stop()  Disable the periodic ping. A request already in flight still completes.
 *
 * Checksum note: this code does NOT write the ICMP checksum. The Ethernet MAC
 * inserts it in hardware (see the comment in ping_send()).
 *
 *  Created on: Nov 15, 2026
 *      Author: moham
 */
#include "ping.h"
#include "main.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "icmp_checksum.h"
#include <stdio.h>
#include <string.h>

/* UART3 is wired to the ST-LINK virtual COM port; used for the log lines. */
extern UART_HandleTypeDef huart3;

#define PING_DATA_SIZE 32 /* payload bytes per request (same size as Windows ping) */
#define PING_ID 0x1234 /* ICMP identifier: lets us recognise our own replies    */
/* ---- module state (private to this file) ---- */
static struct raw_pcb *pcb;  		/* raw PCB for ICMP, created in ping_init()           */
static u16_t sqn = 0;		 		/* seqno of the LAST request sent (host byte order)   */
static uint32_t send_time=0, 		/* HAL_GetTick() when the last request was sent [ms]  */
				rtt;				/* last measured round-trip time [ms]                 */
static ip_addr_t ip_host;			/* target address (the PC), set from HOST_IP in init  */
static uint8_t s_waiting = 0,		/* 1 = request sent, reply not received yet           */
			 s_poll_enabled=0;		/* 1 = ping_poll() sends a request every second       */
/**
 * @brief  Receive callback of the ICMP raw PCB (registered with raw_recv()).
 *
 * lwIP calls this for EVERY ICMP packet addressed to the board, including the
 * PC's own echo requests to the board, so most calls must simply be ignored.
 * A packet is accepted only if it is the echo reply to our last request:
 * right source address, ICMP type 0 (echo reply), code 0, our id, expected seqno.
 *
 * Callback contract of the raw API:
 *   - return 0: "not mine". lwIP keeps the pbuf and carries on normally, so its
 *               own ICMP code still answers the PC's pings.
 *   - return 1: "eaten". We MUST have freed the pbuf ourselves (pbuf_free()).
 *   - p->payload starts at the IP header (the Ethernet header is already
 *     stripped). We reach the ICMP header with a pointer offset and never
 *     modify the pbuf: moving p->payload (pbuf_remove_header()) would corrupt
 *     lwIP's own processing whenever we return 0.
 *
 * @param arg   user pointer given to raw_recv() (unused)
 * @param pcb   PCB that received the packet (unused, we only have one)
 * @param p     received packet, starting at the IP header
 * @param addr  source IP of the packet (lwIP read it from the IP header)
 * @return 1 if the packet was consumed and freed, 0 otherwise
 */
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr){
	(void)arg; (void)pcb;  /* silence "unused parameter" warnings */
	char msg[64];
	struct icmp_echo_hdr *icmp_reply;

	/* Not waiting for anything: a late reply (after a timeout) or foreign traffic. */
	if(!s_waiting) return 0;

	/* Length checks come before every read, so a short or malformed packet
	 * can never make us read past its end. */
	if (p->len < IP_HLEN) return 0;

	/* Only accept packets coming from the host we pinged. */
	if(ip_addr_cmp(addr,&ip_host)){
		struct ip_hdr* ip_reply = (struct ip_hdr*)(p->payload); /* getting the ip header fro the received packet */

		/* The IP header may be longer than 20 bytes (options): use its real length. */
		u8_t iph_len = IPH_HL_BYTES(ip_reply);
		if(iph_len < IP_HLEN) return 0;

		/* ICMP header = start of packet + IP header length. The cast to u8_t*
		 * first makes the "+" count bytes, not struct sizes. */
		u8_t *icmp_ptr = (u8_t *) (p-> payload) + iph_len;
		if(p->len < (iph_len + sizeof(struct icmp_echo_hdr))) return 0;
		icmp_reply = (struct icmp_echo_hdr*)(icmp_ptr);
		
		/* A complete reply carries the full echoed data as well. */
		if(p->tot_len < (iph_len + sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE )) return 0;

		/* Is it OUR echo reply? Any mismatch means "not mine": return 0 silently
		 * (the PC's own echo requests also reach this callback, that is normal).
		 * id and seqno travel in network byte order, so we compare them with htons(). */
		if (!(ICMP_ER == icmp_reply->type)) return 0;
		if (!(icmp_reply->code == 0)) return 0;
		if (!(icmp_reply->seqno == lwip_htons(sqn))) return 0;
		if (!(icmp_reply->id == lwip_htons(PING_ID))) return 0;

		/* Valid reply: RTT = now - time of sending (1 ms resolution of HAL_GetTick). */
		rtt = HAL_GetTick() - send_time;
		snprintf(msg, sizeof(msg), "PING seq=%lu rtt=%lu ms\r\n", (unsigned long)(lwip_ntohs(icmp_reply->seqno)), (unsigned long)(rtt));
		HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 20);

		/* We now own the packet: free it, tell ping_poll() the request is done,
		 * and return 1 so lwIP does not touch the freed pbuf. */
		pbuf_free(p);
		s_waiting = 0;
		return 1;
	}
	return 0; /* different source address: not for us */

    
}

/**
 * @brief  Create the ICMP raw PCB and register ping_recv() on it.
 * @note   Call once, after MX_LWIP_Init().
 */
void ping_init(void){
	/* Convert the "a.b.c.d" string of HOST_IP into an lwIP address.
	 * (ipaddr_aton() returns 0 on a malformed string: worth checking.) */	
	ipaddr_aton(HOST_IP, &ip_host);
	/* One raw PCB that receives all ICMP packets (IP protocol 1). */
	pcb = raw_new(IP_PROTO_ICMP);
	if(pcb == NULL) Error_Handler();
	/* registering the callback function (ping_recv) */
	raw_recv(pcb, ping_recv, NULL); 
	/* Bind to any local address: we accept ICMP sent to any of our addresses. */
	raw_bind(pcb, IP_ADDR_ANY);
}

/**
 * @brief  Build and send one ICMP echo request, then start waiting for the reply.
 * @param  ip_d destination address
 *
 * Frame layout produced here (lwIP adds the IP and Ethernet headers below it):
 *   ICMP header (8 bytes): type=8, code=0, checksum, id, seqno
 *   data (PING_DATA_SIZE bytes): 00 01 02 ... 1F
 */
void ping_send(ip_addr_t ip_d){ //, uint8_t  a, uint8_t  b, uint8_t  c , uint8_t d
	struct pbuf *pbuf;
	u16_t inet_chck_result;
	u16_t icmp_checksum_result;
	/* Debug counters for failed raw_sendto() calls (look at them in the debugger).
	 * These are function-level statics: they keep their value between calls. */
	static volatile uint32_t send_errors;   /* at file level, outside any function */
	static volatile err_t last_err;
	char msg[64];

	/* PBUF_IP reserves room in front of the payload for the IP + Ethernet headers.
	 * PBUF_RAM = one contiguous block from the lwIP heap. */
	pbuf = pbuf_alloc(PBUF_IP,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE,PBUF_RAM);
	if(pbuf == NULL) return;
	struct icmp_echo_hdr *icmp_request;
	icmp_request = (struct icmp_echo_hdr*)(pbuf->payload);
	icmp_request->type = ICMP_ECHO;
	icmp_request->code = 0;
	/* The checksum field stays 0 on purpose. The Ethernet MAC computes and inserts
	 * the ICMP checksum in hardware (TxConfig.ChecksumCtrl =
	 * ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC in ethernetif.c). It sums the
	 * whole ICMP message INCLUDING this field, so writing a valid software
	 * checksum here would make the sum 0xFFFF and the MAC would transmit 0x0000. */
	icmp_request->chksum = 0;
	icmp_request->id = lwip_htons(PING_ID); /* multi-byte fields: network byte order */

	/* Test pattern 00 01 02 ... 1F (the reply must echo it back unchanged). */
	for(u16_t i = 0;i<PING_DATA_SIZE;i++){
		*((u8_t*)icmp_request + sizeof(struct icmp_echo_hdr) + i) = (u8_t) i;
	}
	++sqn;// pre-increment: first ping is seqno 1, and sqn always holds the last seqno sent,
	// so ping_recv can match a reply with: reply seqno == sqn
	icmp_request->seqno = lwip_htons(sqn);

	/* Self-check of my own RFC 1071 checksum (icmp_checksum.c) against lwIP's
	 * inet_chksum() on the same bytes (the checksum field is still 0 here).
	 * Both must print the same value. Mine returns a plain number, so
	 * lwip_htons() puts it in the same byte order as inet_chksum()'s result.
	 * The values are only printed, never stored in the packet (see above).
	 * TODO: remove, or guard with a DEBUG macro, once no longer needed. */
	inet_chck_result = inet_chksum(pbuf->payload,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE);
	icmp_checksum_result = lwip_htons(icmp_checksum(pbuf->payload,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE));
	snprintf(msg, sizeof msg, "icmp_checksum=0x%04X inet=0x%04X\r\n",
         (unsigned)icmp_checksum_result,
         (unsigned)inet_chck_result);
	HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 20);
	
	/* Take the timestamp as late as possible (just before sending) so the RTT
	 * does not include the self-check and its UART print above. */
	send_time = HAL_GetTick();
	err_t err = raw_sendto(pcb, pbuf, &ip_d);
	if (err != ERR_OK){
		send_errors++; last_err = err;
	}
	if(err == ERR_OK){
		s_waiting = 1;
	}

	/* raw_sendto() does not take ownership of the pbuf: we free our reference.
	 * (If lwIP had to queue the packet, e.g. while waiting for the ARP reply,
	 * it holds its own reference, so this free is safe.) */
	pbuf_free(pbuf);
}

/**
 * @brief  Periodic work of the ping client. Non-blocking: call it from the main loop.
 *
 * 1) Timeout: a request still unanswered after 1000 ms is reported and dropped.
 * 2) Next ping: while enabled and idle, send a new request once per second.
 *
 * All timing uses (now - then) on HAL_GetTick(), which stays correct when the
 * 32-bit millisecond counter wraps around.
 */
void ping_poll(void){
	char msg[64];
	uint32_t now = HAL_GetTick();
	/* 1) No reply within 1 s: report a timeout and stop waiting. */
	if(s_waiting && (now - send_time)>=1000){
		snprintf(msg, sizeof(msg), "PING seq = %lu timeout\r\n", (unsigned long)(sqn));
		HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 20);
		s_waiting = 0;
	}
	/* 2) Idle and enabled: send the next request one second after the last one. */	
	if(s_poll_enabled && !s_waiting){
		if((now - send_time)>=1000){
			ping_send(ip_host);
		}
	}
}

/** @brief Enable the periodic ping (picked up by ping_poll() in the main loop). */
void ping_start(void){
	s_poll_enabled = 1;
}

/** @brief Disable the periodic ping. A request already in flight still completes. */
void ping_stop(void){
	s_poll_enabled = 0;
}


