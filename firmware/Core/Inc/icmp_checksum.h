/*
 * icmp_checksum.h
 *
 *  Created on: Nov 20, 2026
 *      Author: moham
 */

#ifndef INC_ICMP_CHECKSUM_H_
#define INC_ICMP_CHECKSUM_H_
#include <stdint.h>
#include <stdbool.h>


/**
 * @brief  Compute the 16-bit Internet checksum of a buffer.
 *
 * @param  data    bytes to sum (for ICMP: the whole ICMP message, header + data,
 *                 with the checksum field set to 0; NOT the IP header). Not modified.
 * @param  length  number of BYTES in @p data (odd lengths are allowed)
 * @return checksum as a plain 16-bit number.
 *
 * @note   lwIP's inet_chksum() returns the value ready to be stored in the packet
 *         (byte-swapped on the little-endian STM32). To compare the two, use
 *         lwip_htons(icmp_checksum(...)) == inet_chksum(...).
 */
uint16_t icmp_checksum(const uint8_t *data, int length);

#endif /* INC_ICMP_CHECKSUM_H_ */
