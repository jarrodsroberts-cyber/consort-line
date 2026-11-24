/**
 * @file proof.h  Each end's proof, random bytes, and a compare that takes the same time
 *
 * Windows' own (BCrypt): HMAC-SHA256 and the system's random source. Never re's rand_bytes,
 * which without OpenSSL falls back to a weak generator.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef CONSORT_PROOF_H
#define CONSORT_PROOF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "proto.h"

/**
 * One end's proof for a call: HMAC-SHA256 keyed with the secret's 32 bytes, over the ASCII
 * side ("consort" or "line", nothing after it), the nonce's 16 bytes and the port as two
 * bytes, big-endian: the digest's first 16 bytes. 0, or an error number.
 */
int cl_proof(uint8_t out[CL_PROOF_BYTES], const uint8_t key[CL_SECRET_BYTES],
	     const uint8_t nonce[CL_NONCE_BYTES], const char *side, uint16_t port);

/** Fills out with bytes from the system's random source. 0, or an error number. */
int cl_random(uint8_t *out, size_t len);

/** Whether a and b hold the same len bytes, taking the same time wherever they differ. */
bool cl_same(const uint8_t *a, const uint8_t *b, size_t len);

/** Overwrites len bytes at p with zeros, in a way the compiler keeps. */
void cl_wipe(void *p, size_t len);

/** Reads 2 * len hex digits (either case) into out; false for anything else. */
bool cl_unhex(uint8_t *out, size_t len, const char *hex, size_t hexlen);

/** Writes len bytes as 2 * len lowercase hex digits and a NUL to out. */
void cl_hex(char *out, const uint8_t *in, size_t len);

#endif
