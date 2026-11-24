/**
 * @file wake.c  Waking baresip's main thread from another thread, safely
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <re.h>
#include <stdlib.h>
#include <string.h>
#include "proof.h"
#include "wake.h"

#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

enum {
	TOKEN_BYTES = 16,
	PAIR_WAIT_MS = 2000,
};

struct cl_wake {
	SOCKET reader;
	SOCKET writer;
	struct re_fhs *fhs;
	cl_wake_h *h;
	void *arg;
};


static SOCKET new_socket(void)
{
	return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
			  WSA_FLAG_NO_HANDLE_INHERIT);
}


static bool same_address(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
	return a->sin_family == b->sin_family && a->sin_port == b->sin_port &&
		a->sin_addr.s_addr == b->sin_addr.s_addr;
}


static bool recv_exactly(SOCKET s, uint8_t *p, int n)
{
	while (n > 0) {
		const int k = recv(s, (char *)p, n, 0);
		if (k <= 0)
			return false;
		p += k;
		n -= k;
	}

	return true;
}


/**
 * A connected pair on 127.0.0.1 whose reader end is certainly connected to its writer end:
 * any other connection that reaches the listener first is closed, and the listener is closed
 * once the pair is made.
 */
static int make_pair(SOCKET *readerp, SOCKET *writerp)
{
	struct sockaddr_in sin, mine, peer;
	int len = sizeof(sin);
	uint8_t token[TOKEN_BYTES], heard[TOKEN_BYTES];
	SOCKET listener, writer = INVALID_SOCKET, reader = INVALID_SOCKET;
	BOOL yes = TRUE;
	u_long on = 1;
	DWORD start;
	int err = EIO;

	listener = new_socket();
	if (listener == INVALID_SOCKET)
		return EIO;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes,
		       sizeof(yes)) ||
	    bind(listener, (struct sockaddr *)&sin, sizeof(sin)) || listen(listener, 4) ||
	    getsockname(listener, (struct sockaddr *)&sin, &len))
		goto out;

	writer = new_socket();
	if (writer == INVALID_SOCKET || connect(writer, (struct sockaddr *)&sin, sizeof(sin)))
		goto out;
	len = sizeof(mine);
	if (getsockname(writer, (struct sockaddr *)&mine, &len))
		goto out;

	if (cl_random(token, sizeof(token)) ||
	    send(writer, (const char *)token, (int)sizeof(token), 0) != (int)sizeof(token))
		goto out;

	/* Take the writer's own connection; close any other that came first */
	(void)ioctlsocket(listener, FIONBIO, &on);
	start = GetTickCount();
	while (reader == INVALID_SOCKET && GetTickCount() - start < PAIR_WAIT_MS) {
		SOCKET s;

		len = sizeof(peer);
		s = accept(listener, (struct sockaddr *)&peer, &len);
		if (s == INVALID_SOCKET) {
			if (WSAGetLastError() != WSAEWOULDBLOCK)
				goto out;
			Sleep(1);
			continue;
		}
		if (!same_address(&peer, &mine)) {
			(void)closesocket(s);
			continue;
		}
		reader = s;
	}
	if (reader == INVALID_SOCKET)
		goto out;

	(void)SetHandleInformation((HANDLE)reader, HANDLE_FLAG_INHERIT, 0);
	on = 0;
	(void)ioctlsocket(reader, FIONBIO, &on);
	if (!recv_exactly(reader, heard, sizeof(heard)) ||
	    !cl_same(heard, token, sizeof(token)))
		goto out;

	on = 1;
	(void)ioctlsocket(reader, FIONBIO, &on);
	(void)ioctlsocket(writer, FIONBIO, &on);

	*readerp = reader;
	*writerp = writer;
	reader = writer = INVALID_SOCKET;
	err = 0;

 out:
	cl_wipe(token, sizeof(token));
	(void)closesocket(listener);
	if (reader != INVALID_SOCKET)
		(void)closesocket(reader);
	if (writer != INVALID_SOCKET)
		(void)closesocket(writer);

	return err;
}


static void readable(int flags, void *arg)
{
	struct cl_wake *w = arg;
	char buf[64];
	(void)flags;

	/* Drain every wake byte, then take what was posted */
	while (recv(w->reader, buf, sizeof(buf), 0) > 0)
		;

	w->h(w->arg);
}


int cl_wake_alloc(struct cl_wake **wp, cl_wake_h *h, void *arg)
{
	struct cl_wake *w;
	int err;

	if (!wp || !h)
		return EINVAL;

	w = calloc(1, sizeof(*w));
	if (!w)
		return ENOMEM;

	w->h = h;
	w->arg = arg;
	err = make_pair(&w->reader, &w->writer);
	if (err) {
		free(w);
		return err;
	}

	err = fd_listen(&w->fhs, (re_sock_t)w->reader, FD_READ, readable, w);
	if (err) {
		(void)closesocket(w->reader);
		(void)closesocket(w->writer);
		free(w);
		return err;
	}

	*wp = w;

	return 0;
}


void cl_wake_post(struct cl_wake *w)
{
	const char byte = 1;

	/* A full pipe has wakes waiting already */
	if (w)
		(void)send(w->writer, &byte, 1, 0);
}


void cl_wake_free(struct cl_wake *w)
{
	if (!w)
		return;

	w->fhs = fd_close(w->fhs);
	(void)closesocket(w->reader);
	(void)closesocket(w->writer);
	free(w);
}
