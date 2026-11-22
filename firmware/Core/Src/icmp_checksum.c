/*
 * icmp_checksum.c
 *
 *  Created on: Nov 20, 2026
 *      Author: moham
 */

#include "icmp_checksum.h"
uint16_t icmp_checksum(const uint8_t *data, int length){
    uint32_t sum = 0;
    uint16_t data_word = 0;
    bool odd_length = (length & 1);
    for (int i = 0; i < length; i++){
        if (i%2 == 0){
            data_word = (uint16_t)(data[i]) << 8;
            if(odd_length && i == length - 1){
                sum += data_word;
            }
        } else {
            data_word |= (uint16_t)(data[i]);
            sum += data_word;
        }
    }

    while (sum >> 16){
        sum  = (sum>>16) + (sum & 0xFFFF);
    }
    return (uint16_t)(~sum);

}
