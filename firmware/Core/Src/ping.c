/*
 * ping.c
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
extern UART_HandleTypeDef huart3;

#define PING_DATA_SIZE 32
#define PING_ID 0x1234
static struct raw_pcb *pcb;
static u16_t sqn = 0;
static uint32_t send_time=0, rtt;
static ip_addr_t ip_host;
static uint8_t s_waiting = 0, s_poll_enabled=0;

/*
 Function prototype for raw pcb receive callback functions.
 * @param arg user supplied argument (raw_pcb.recv_arg)
 * @param pcb the raw_pcb which received data
 * @param p the packet buffer that was received
 * @param addr the remote IP address from which the packet was received
 * @return 1 if the packet was 'eaten' (aka. deleted),
 *         0 if the packet lives on
 * If returning 1, the callback is responsible for freeing the pbuf
 * if it's not used any more.
typedef u8_t (*raw_recv_fn)(void *arg, struct raw_pcb *pcb, struct pbuf *p,
    const ip_addr_t *addr);
 */
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr){
	(void)arg; (void)pcb;  /* silence "unused parameter" warnings */
	char msg[64];
	struct icmp_echo_hdr *icmp_reply;
	if(!s_waiting) return 0;
	if (p->len < IP_HLEN) return 0;
	if(ip_addr_cmp(addr,&ip_host)){
		struct ip_hdr* ip_reply = (struct ip_hdr*)(p->payload);
		u8_t iph_len = IPH_HL_BYTES(ip_reply);
		if(iph_len < IP_HLEN) return 0;
		u8_t *icmp_ptr = (u8_t *) (p-> payload) + iph_len;
		if(p->len < (iph_len + sizeof(struct icmp_echo_hdr))) return 0;
		icmp_reply = (struct icmp_echo_hdr*)(icmp_ptr);
		if(p->tot_len < (iph_len + sizeof(struct icmp_echo_hdr) + PING_DATA_SIZE )) return 0;
		if (!(ICMP_ER == icmp_reply->type)) return 0;
		if (!(icmp_reply->code == 0)) return 0;
		if (!(icmp_reply->seqno == lwip_htons(sqn))) return 0;
		if (!(icmp_reply->id == lwip_htons(PING_ID))) return 0;
		rtt = HAL_GetTick() - send_time;
		snprintf(msg, sizeof(msg), "PING seq=%lu rtt=%lu ms\r\n", (unsigned long)(icmp_reply->seqno), (unsigned long)(rtt));
		HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 20);
		pbuf_free(p);
		s_waiting = 0;
		return 1;
	}
	return 0;

    
}
void ping_init(void){	
	ipaddr_aton(HOST_IP, &ip_host);
	pcb = raw_new(IP_PROTO_ICMP);
	if(pcb == NULL) Error_Handler();
	raw_recv(pcb, ping_recv, NULL); // registering the callback function (ping_recv)
	raw_bind(pcb, IP_ADDR_ANY);
}
void ping_send(ip_addr_t ip_d){ //, uint8_t  a, uint8_t  b, uint8_t  c , uint8_t d
	struct pbuf *pbuf;
	u16_t inet_chck_result;
	u16_t icmp_checksum_result;
	static volatile uint32_t send_errors;   /* at file level, outside any function */
	static volatile err_t last_err;
	char msg[64];
	pbuf = pbuf_alloc(PBUF_IP,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE,PBUF_RAM);
	if(pbuf == NULL) return;
	struct icmp_echo_hdr *icmp_request;
	icmp_request = (struct icmp_echo_hdr*)(pbuf->payload);
	icmp_request->type = ICMP_ECHO;
	icmp_request->code = 0;
	icmp_request->chksum = 0;
	icmp_request->id = lwip_htons(PING_ID);
	for(u16_t i = 0;i<PING_DATA_SIZE;i++){
		*((u8_t*)icmp_request + sizeof(struct icmp_echo_hdr) + i) = (u8_t) i;
	}
	++sqn;// pre-increment: first ping is seqno 1, and sqn always holds the last seqno sent,
	// so ping_recv can match a reply with: reply seqno == sqn
	icmp_request->seqno = lwip_htons(sqn);
	inet_chck_result = inet_chksum(pbuf->payload,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE);
	icmp_checksum_result = lwip_htons(icmp_checksum(pbuf->payload,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE));
	snprintf(msg, sizeof msg, "icmp_checksum=0x%04X inet=0x%04X\r\n",
         (unsigned)icmp_checksum_result,
         (unsigned)inet_chck_result);
	HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 20);
//	icmp_request->chksum = inet_chksum(pbuf->payload,sizeof(struct icmp_echo_hdr)+PING_DATA_SIZE);
	// ip_addr_t ip_d;
	// IP4_ADDR(&ip_d,a,b,c,d);
	send_time = HAL_GetTick();
	err_t err = raw_sendto(pcb, pbuf, &ip_d);
	if (err != ERR_OK){
		send_errors++; last_err = err;
	}
	s_waiting = 1;
	pbuf_free(pbuf);
}
void ping_poll(void){
	char msg[64];
	uint32_t now = HAL_GetTick();
	if(s_waiting && (now - send_time)>=1000){
		snprintf(msg, sizeof(msg), "PING seq = %lu timeout\r\n", (unsigned long)(sqn));
		HAL_UART_Transmit(&huart3, (uint8_t *)msg, strlen(msg), 20);
		s_waiting = 0;
	}
	if(s_poll_enabled && !s_waiting){
		if((now - send_time)>=1000){
			ping_send(ip_host);
		}
	}
}

void ping_start(void){
	s_poll_enabled = 1;
}

void ping_stop(void){
	s_poll_enabled = 0;
}


