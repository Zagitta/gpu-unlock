/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GD_AES_H
#define GD_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void gd_aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void gd_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
int gd_aes128_encrypt(const uint8_t key[16], uint8_t *data, size_t size);
int gd_aes128_decrypt(const uint8_t key[16], uint8_t *data, size_t size);
void gd_aes128_expand_key(const uint8_t key[16], uint8_t round_keys[176]);
void gd_aes128_reverse_key(const uint8_t last_round[16], uint8_t key[16]);
bool gd_aes128_selftest(void);

#endif
