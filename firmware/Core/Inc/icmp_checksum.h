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
uint16_t icmp_checksum(const uint8_t *data, int length);

#endif /* INC_ICMP_CHECKSUM_H_ */
