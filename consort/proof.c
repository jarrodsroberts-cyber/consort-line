/**
 * @file proof.c  Each end's proof, random bytes, and a compare that takes the same time
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <string.h>
#include "proof.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif


int cl_proof(uint8_t out[CL_PROOF_BYTES], const uint8_t key[CL_SECRET_BYTES],
	     const uint8_t nonce[CL_NONCE_BYTES], const char *side, uint16_t port)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	UCHAR digest[32];
	UCHAR portv[2];
	NTSTATUS st;
	int err = EIO;

	if (!out || !key || !nonce || !side)
		return EINVAL;

	portv[0] = (UCHAR)(port >> 8);
	portv[1] = (UCHAR)(port & 0xff);

	st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL,
					 BCRYPT_ALG_HANDLE_HMAC_FLAG);
	if (!NT_SUCCESS(st))
		return EIO;

	st = BCryptCreateHash(alg, &hash, NULL, 0, (PUCHAR)key, CL_SECRET_BYTES, 0);
	if (!NT_SUCCESS(st))
		goto out;

	st = BCryptHashData(hash, (PUCHAR)side, (ULONG)strlen(side), 0);
	if (NT_SUCCESS(st))
		st = BCryptHashData(hash, (PUCHAR)nonce, CL_NONCE_BYTES, 0);
	if (NT_SUCCESS(st))
		st = BCryptHashData(hash, portv, sizeof(portv), 0);
	if (NT_SUCCESS(st))
		st = BCryptFinishHash(hash, digest, sizeof(digest), 0);
	if (!NT_SUCCESS(st))
		goto out;

	memcpy(out, digest, CL_PROOF_BYTES);
	err = 0;

 out:
	cl_wipe(digest, sizeof(digest));
	if (hash)
		BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(alg, 0);

	return err;
}


int cl_random(uint8_t *out, size_t len)
{
	NTSTATUS st;

	if (!out || len > 0xffffffffu)
		return EINVAL;

	st = BCryptGenRandom(NULL, out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);

	return NT_SUCCESS(st) ? 0 : EIO;
}


bool cl_same(const uint8_t *a, const uint8_t *b, size_t len)
{
	volatile uint8_t diff = 0;

	for (size_t i = 0; i < len; i++)
		diff |= (uint8_t)(a[i] ^ b[i]);

	return diff == 0;
}


void cl_wipe(void *p, size_t len)
{
	if (p && len)
		SecureZeroMemory(p, len);
}


static int nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}


bool cl_unhex(uint8_t *out, size_t len, const char *hex, size_t hexlen)
{
	if (!out || !hex || hexlen != 2 * len)
		return false;

	for (size_t i = 0; i < len; i++) {
		const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			cl_wipe(out, len);
			return false;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}

	return true;
}


void cl_hex(char *out, const uint8_t *in, size_t len)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 0xf];
	}
	out[2 * len] = '\0';
}
