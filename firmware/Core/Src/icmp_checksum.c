/**
 * @file    icmp_checksum.c
 * @brief   Internet checksum (RFC 1071): 16-bit one's complement of the
 *          one's complement sum of all 16-bit words.
 *
 *  Created on: Nov 20, 2026
 *      Author: moham
 */

#include "icmp_checksum.h"
uint16_t icmp_checksum(const uint8_t *data, int length){
    uint32_t sum = 0; /* 32 bits so the carries of many 16-bit additions are not lost */
    uint16_t data_word = 0; /* the 16-bit word being assembled from two bytes */
    bool odd_length = (length & 1);
    for (int i = 0; i < length; i++){
        if (i%2 == 0){
             /* First byte of a pair = HIGH byte of the word (network byte order:
             * bytes 08 00 form the word 0x0800, not 0x0008). */
            data_word = (uint16_t)(data[i]) << 8;
            /* Odd length: the last byte has no partner. It is added as the high
             * byte of a word padded with a zero low byte. */
            if(odd_length && i == length - 1){
                sum += data_word;
            }
        } else {
            /* Second byte of the pair = LOW byte: the word is complete, add it. */
            data_word |= (uint16_t)(data[i]);
            sum += data_word;
        }
    }
    /* Fold the carries back in: add the bits above 16 to the low 16 bits and
     * repeat until nothing is left above bit 15 (one's complement addition). */
    while (sum >> 16){
        sum  = (sum>>16) + (sum & 0xFFFF);
    }
    /* The checksum is the bitwise inverse of the folded sum. */
    return (uint16_t)(~sum);

}
