/**
 * @file link.c  One call's link to Consort: its listener, the proofs, the frames and the tick
 *
 * One thread a call. It waits on three things at once: the tick's high-resolution timer, a
 * wake event for what other threads post (keys, a hang-up, the listener, stop), and one
 * event the link's sockets share. Only this thread touches the sockets and the player.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "link.h"
#include "proof.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

enum {
	TICK_US     = 20000,
	ID_US       = 1000000,  /**< Each connection's ID within 1 s           */
	PROVE_US    = 2000000,  /**< The right ID within 2 s of the answer      */
	TAKE_US     = 2000000,  /**< Consort takes the line's frames within 2 s */
	LATE_US     = 2000,     /**< A tick this late or later counts as late   */
	RESTART_US  = 100000,   /**< Later than this, the clock starts again    */
	MAX_PENDING = 16,       /**< Connections waiting to prove themselves    */
	MAX_ACCEPTS = 8,        /**< Connections taken a pass, at most          */
	OUT_BYTES   = 65536,    /**< The line's frames waiting for Consort      */
	SEND_BUFFER = 2048,     /**< The kernel's share of them: a few frames   */
	LAST_BUFFER = 1 << 20,  /**< The kernel's room once the last frame comes: all of them */
	LINGER_US   = 2000000,  /**< After it, Consort's bytes are read until it closes, or this */
	MAX_KEYS    = 32,
};

struct pending {
	SOCKET s;
	uint64_t since;
	uint8_t buf[CL_ID_FRAME_BYTES];
	size_t have;
};

struct cl_link {
	SRWLOCK lock;          /**< Guards what other threads post, and the stats */
	HANDLE thread;
	HANDLE timer;          /**< The tick                                       */
	bool coarse;           /**< No high-resolution timer: timeBeginPeriod(1)   */
	HANDLE wake;           /**< Auto-reset: something was posted               */
	WSAEVENT sockev;       /**< Manual-reset: the link's sockets share it      */

	cl_ended_h *endh;
	void *arg;
	uint8_t secret[CL_SECRET_BYTES];

	/* Posted, under the lock */
	cl_source_h *srch;
	void *srcarg;
	cl_player_h *playh;
	void *playarg;
	char keys[MAX_KEYS];
	unsigned nkeys;
	bool hangup;
	bool stop;
	bool answered;
	SOCKET listener;
	uint16_t port;
	uint8_t want[CL_PROOF_BYTES];   /**< Consort's proof */
	uint8_t mine[CL_PROOF_BYTES];   /**< The line's      */
	uint64_t answered_at;
	bool proved;
	struct cl_link_stats stats;

	/* The link's thread only */
	bool listening;
	struct pending pending[MAX_PENDING];
	unsigned npending;
	SOCKET conn;
	uint8_t in[CL_MAX_FRAME_BYTES];
	size_t inlen;
	size_t need;
	uint8_t out[OUT_BYTES];
	size_t outlen;
	uint64_t out_since;
	struct cl_player player;
	bool flushed;          /**< A FLUSH since the last MARK played */
	uint64_t sent;
	bool ended;
};


static uint64_t now_us(void)
{
	LARGE_INTEGER freq, count;

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);

	return (uint64_t)(count.QuadPart / freq.QuadPart) * 1000000u +
		(uint64_t)(count.QuadPart % freq.QuadPart) * 1000000u /
		(uint64_t)freq.QuadPart;
}


static void close_socket(SOCKET *s)
{
	if (*s != INVALID_SOCKET) {
		(void)closesocket(*s);
		*s = INVALID_SOCKET;
	}
}


static void close_all(struct cl_link *l)
{
	if (l->conn != INVALID_SOCKET) {
		(void)shutdown(l->conn, SD_SEND);
		close_socket(&l->conn);
	}

	for (unsigned i = 0; i < l->npending; i++)
		close_socket(&l->pending[i].s);
	l->npending = 0;

	AcquireSRWLockExclusive(&l->lock);
	close_socket(&l->listener);
	ReleaseSRWLockExclusive(&l->lock);
	l->listening = false;
}


/** Writes what is waiting; false when Consort's end is gone */
static bool flush_out(struct cl_link *l)
{
	while (l->outlen) {
		const int n = send(l->conn, (const char *)l->out, (int)l->outlen, 0);

		if (n == SOCKET_ERROR)
			return WSAGetLastError() == WSAEWOULDBLOCK;

		memmove(l->out, l->out + n, l->outlen - (size_t)n);
		l->outlen -= (size_t)n;
		l->out_since = now_us();
	}

	return true;
}


/**
 * The last of the line's frames (HANGUP or ERROR, after what waited for Consort): the
 * kernel is given room for all of it, as the small send buffer could leave it in the
 * line's own queue, which goes when the connection closes. The room is generous: Windows
 * Server's share moves over a call, and a raise to just the queue's size left frames behind
 * there. The close is graceful, so the kernel still sends what it holds, then the end.
 */
static void flush_last(struct cl_link *l)
{
	const int room = LAST_BUFFER;

	if (!l->outlen || l->conn == INVALID_SOCKET)
		return;

	(void)setsockopt(l->conn, SOL_SOCKET, SO_SNDBUF, (const char *)&room, sizeof(room));
	(void)flush_out(l);
}


/** The closer's thread: Consort's bytes read and dropped until it closes, or LINGER_US */
static DWORD WINAPI drain_close(LPVOID arg)
{
	const SOCKET s = (SOCKET)(UINT_PTR)arg;
	const uint64_t until = now_us() + LINGER_US;
	char sink[1024];

	while (now_us() < until) {
		struct timeval tv = {0, 100000};   /* the limit is looked at every 100 ms */
		fd_set readable;
		int n;

		FD_ZERO(&readable);
		FD_SET(s, &readable);
		n = select(0, &readable, NULL, NULL, &tv);
		if (n == SOCKET_ERROR)
			break;
		if (n == 0)
			continue;

		n = recv(s, sink, (int)sizeof(sink), 0);
		if (n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
			break;
	}

	(void)closesocket(s);

	return 0;
}


/**
 * Closes Consort's connection after the line's last frame (HANGUP or ERROR) without losing
 * what waits for Consort. On Windows a socket closed with Consort's bytes unread, or that
 * Consort's bytes reach once it is closed, ends in a reset, which drops every frame Consort
 * hasn't read: the usual case for a Consort that is behind and still speaking. So the line's
 * end is shut, and a thread of its own reads what Consort still sends until Consort closes
 * its end, or for LINGER_US, before the socket goes. Neither the link nor baresip waits for
 * it. With no thread to be had, close_all closes the socket at once, as before.
 */
static void close_last(struct cl_link *l)
{
	HANDLE t = NULL;

	if (l->conn == INVALID_SOCKET)
		return;

	(void)shutdown(l->conn, SD_SEND);

	/* Off the event the link's sockets share, which goes with the link */
	if (0 == WSAEventSelect(l->conn, NULL, 0))
		t = CreateThread(NULL, 0, drain_close, (LPVOID)(UINT_PTR)l->conn, 0, NULL);
	if (!t)
		return;

	CloseHandle(t);
	l->conn = INVALID_SOCKET;   /* the closer's now */
}


static void end(struct cl_link *l, enum cl_end why)
{
	if (l->ended)
		return;

	l->ended = true;

	if ((why == CL_END_RULE || why == CL_END_FAILED) &&
	    l->conn != INVALID_SOCKET) {
		const uint8_t code = why == CL_END_RULE ? CL_ERR_FRAME : CL_ERR_MEMORY;
		uint8_t frame[CL_HEADER_BYTES + 1];
		const size_t n = cl_frame(frame, CL_ERROR, &code, 1);

		if (l->outlen + n <= OUT_BYTES) {
			memcpy(l->out + l->outlen, frame, n);
			l->outlen += n;
			(void)flush_out(l);
			flush_last(l);
			close_last(l);
		}
	}

	close_all(l);

	if (l->endh)
		l->endh(why, l->arg);
}


static void send_frame(struct cl_link *l, uint8_t type, const uint8_t *payload, uint16_t len)
{
	uint8_t frame[CL_MAX_FRAME_BYTES];
	const size_t n = cl_frame(frame, type, payload, len);

	if (l->ended || l->conn == INVALID_SOCKET)
		return;

	if (l->outlen + n > OUT_BYTES) {
		end(l, CL_END_NOT_TAKEN);
		return;
	}

	if (!l->outlen)
		l->out_since = now_us();
	memcpy(l->out + l->outlen, frame, n);
	l->outlen += n;

	if (!flush_out(l))
		end(l, CL_END_CLOSED);
}


static void echo_due(struct cl_link *l)
{
	uint32_t id;

	while (!l->ended && (id = cl_player_due(&l->player)) != 0) {
		uint8_t p[4];

		l->flushed = false;
		cl_put_u32(p, id);
		send_frame(l, CL_MARK, p, sizeof(p));
	}
}


static void handle_frame(struct cl_link *l)
{
	const uint8_t *payload = l->in + CL_HEADER_BYTES;

	switch (l->in[0]) {

	case CL_AUDIO:
		if (cl_player_audio(&l->player, payload))
			end(l, CL_END_RULE);
		break;

	case CL_MARK:
		if (cl_player_mark(&l->player, cl_get_u32(payload))) {
			end(l, CL_END_RULE);
			break;
		}
		echo_due(l);
		break;

	case CL_FLUSH: {
		uint32_t mark, samples;
		uint8_t p[8];

		cl_player_flush(&l->player, &mark, &samples);
		l->flushed = true;
		cl_put_u32(p, mark);
		cl_put_u32(p + 4, samples);
		send_frame(l, CL_FLUSHED, p, sizeof(p));
		break;
	}

	case CL_HANGUP:
		end(l, CL_END_HANGUP);
		break;

	default:
		end(l, CL_END_RULE);
		break;
	}
}


/** Consort's frames, each judged by its first three bytes before its payload is read */
static void read_frames(struct cl_link *l)
{
	while (!l->ended && l->conn != INVALID_SOCKET) {
		const size_t want = l->inlen < CL_HEADER_BYTES ?
			CL_HEADER_BYTES - l->inlen :
			CL_HEADER_BYTES + l->need - l->inlen;
		const int n = recv(l->conn, (char *)l->in + l->inlen, (int)want, 0);

		if (n == 0) {
			end(l, CL_END_CLOSED);
			return;
		}
		if (n == SOCKET_ERROR) {
			if (WSAGetLastError() != WSAEWOULDBLOCK)
				end(l, CL_END_CLOSED);
			return;
		}

		l->inlen += (size_t)n;

		if (l->inlen == CL_HEADER_BYTES) {
			const int size = cl_consort_payload(l->in[0]);
			const unsigned len = ((unsigned)l->in[1] << 8) | l->in[2];

			if (size < 0 || len != (unsigned)size) {
				end(l, CL_END_RULE);
				return;
			}
			l->need = len;
		}

		if (l->inlen >= CL_HEADER_BYTES &&
		    l->inlen == CL_HEADER_BYTES + l->need) {
			handle_frame(l);
			l->inlen = 0;
			l->need = 0;
		}
	}
}


static void refuse(struct cl_link *l, struct pending *p)
{
	close_socket(&p->s);

	AcquireSRWLockExclusive(&l->lock);
	++l->stats.refused;
	ReleaseSRWLockExclusive(&l->lock);
}


/** Consort's ID checked out on this connection: it is the call's, and the line proves itself */
static void become_consort(struct cl_link *l, struct pending *p)
{
	const int sndbuf = SEND_BUFFER;
	BOOL yes = TRUE;

	l->conn = p->s;
	p->s = INVALID_SOCKET;

	for (unsigned i = 0; i < l->npending; i++) {
		if (l->pending[i].s != INVALID_SOCKET)
			refuse(l, &l->pending[i]);
	}

	(void)setsockopt(l->conn, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
	/* A small send buffer: frames Consort doesn't take soon wait in the line's own queue,
	 * where the 2 s it has to take them are counted */
	(void)setsockopt(l->conn, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
	if (WSAEventSelect(l->conn, l->sockev, FD_READ | FD_WRITE | FD_CLOSE)) {
		end(l, CL_END_FAILED);
		return;
	}

	AcquireSRWLockExclusive(&l->lock);
	l->proved = true;
	ReleaseSRWLockExclusive(&l->lock);

	send_frame(l, CL_ID, l->mine, CL_PROOF_BYTES);
	read_frames(l);
}


/** Reads a waiting connection's ID; true while it may still prove itself */
static bool read_id(struct cl_link *l, struct pending *p)
{
	for (;;) {
		const int n = recv(p->s, (char *)p->buf + p->have,
				   (int)(CL_ID_FRAME_BYTES - p->have), 0);

		if (n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
			refuse(l, p);
			return false;
		}
		if (n == SOCKET_ERROR)
			return true;

		p->have += (size_t)n;

		if ((p->have >= 1 && p->buf[0] != CL_ID) ||
		    (p->have >= 2 && p->buf[1] != 0) ||
		    (p->have >= 3 && p->buf[2] != CL_PROOF_BYTES)) {
			refuse(l, p);
			return false;
		}

		if (p->have < CL_ID_FRAME_BYTES)
			continue;

		if (cl_same(p->buf + CL_HEADER_BYTES, l->want, CL_PROOF_BYTES)) {
			become_consort(l, p);
			return false;
		}

		refuse(l, p);
		return false;
	}
}


static void compact_pending(struct cl_link *l);


/**
 * Makes room for one more waiting connection: the oldest goes, so a flood can't crowd out
 * Consort's. Its ID may have come since it was taken, so it is read first: Consort's
 * connection proves itself there rather than going unread.
 */
static void make_room(struct cl_link *l)
{
	unsigned oldest = 0;

	compact_pending(l);
	if (l->npending < MAX_PENDING)
		return;

	for (unsigned i = 1; i < l->npending; i++) {
		if (l->pending[i].since < l->pending[oldest].since)
			oldest = i;
	}
	if (read_id(l, &l->pending[oldest]))
		refuse(l, &l->pending[oldest]);
	compact_pending(l);
}


/**
 * Takes the connections waiting, MAX_ACCEPTS at most, so a flood can't hold the thread
 * here: each pass still reads stop and runs the tick and the deadlines. True when more may
 * be waiting.
 */
static bool accept_some(struct cl_link *l)
{
	for (unsigned n = 0; n < MAX_ACCEPTS; n++) {
		SOCKET s = accept(l->listener, NULL, NULL);
		struct pending *p;

		if (s == INVALID_SOCKET)
			return false;

		(void)SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);

		if (!l->proved && !l->ended && l->npending >= MAX_PENDING)
			make_room(l);

		if (l->proved || l->ended || l->npending >= MAX_PENDING ||
		    WSAEventSelect(s, l->sockev, FD_READ | FD_CLOSE)) {
			(void)closesocket(s);
			AcquireSRWLockExclusive(&l->lock);
			++l->stats.refused;
			ReleaseSRWLockExclusive(&l->lock);
			continue;
		}

		p = &l->pending[l->npending++];
		p->s = s;
		p->since = now_us();
		p->have = 0;

		/* It may have sent its ID already. Once it proved itself, the rest are closed */
		(void)read_id(l, p);
		if (l->ended)
			return false;
	}

	return true;
}


static void compact_pending(struct cl_link *l)
{
	unsigned kept = 0;

	for (unsigned i = 0; i < l->npending; i++) {
		if (l->pending[i].s != INVALID_SOCKET)
			l->pending[kept++] = l->pending[i];
	}
	l->npending = kept;
}


static void network(struct cl_link *l)
{
	WSANETWORKEVENTS ne;
	bool more = false;

	if (l->listening && l->listener != INVALID_SOCKET &&
	    0 == WSAEnumNetworkEvents(l->listener, l->sockev, &ne) &&
	    (ne.lNetworkEvents & FD_ACCEPT))
		more = accept_some(l);

	for (unsigned i = 0; i < l->npending && !l->proved && !l->ended; i++) {
		struct pending *p = &l->pending[i];

		if (p->s == INVALID_SOCKET)
			continue;
		if (0 == WSAEnumNetworkEvents(p->s, l->sockev, &ne) &&
		    (ne.lNetworkEvents & (FD_READ | FD_CLOSE)))
			(void)read_id(l, p);
	}
	compact_pending(l);

	if (l->conn != INVALID_SOCKET && !l->ended &&
	    0 == WSAEnumNetworkEvents(l->conn, l->sockev, &ne)) {

		if (ne.lNetworkEvents & (FD_READ | FD_CLOSE))
			read_frames(l);
		if (!l->ended && (ne.lNetworkEvents & FD_WRITE) && !flush_out(l))
			end(l, CL_END_CLOSED);
		if (!l->ended && (ne.lNetworkEvents & FD_CLOSE))
			end(l, CL_END_CLOSED);
	}

	/* The listener's FD_ACCEPT stays recorded, but the calls above reset the event the
	 * sockets share: set it again, so the rest are taken on the next pass, not a tick on */
	if (more && !l->ended)
		(void)WSASetEvent(l->sockev);
}


/**
 * One tick: the caller's next 20 ms to Consort, and with feed, Consort's next 20 ms (or
 * silence) to baresip's source. Ticks that catch up feed nothing: baresip's sender has let
 * those 20 ms go already, and frames handed it for them at once would wait in its buffer for
 * the rest of the call. Consort's frames wait in the player instead, where FLUSH and the
 * MARKs still reach them.
 */
static void tick(struct cl_link *l, bool feed)
{
	int16_t caller[CL_SAMPLES], voice[CL_SAMPLES];
	const bool mid = l->player.since > 0 && !l->flushed;
	bool heard = false, played = false;

	if (feed)
		played = cl_player_play(&l->player, voice);

	AcquireSRWLockExclusive(&l->lock);
	if (l->playh) {
		l->playh(caller, l->playarg);
		heard = true;
	}
	if (feed && l->srch)
		l->srch(voice, l->srcarg);

	++l->stats.ticks;
	if (played)
		++l->stats.played;
	else if (feed && mid)
		++l->stats.dry;
	if (l->player.count > l->stats.most_queued)
		l->stats.most_queued = l->player.count;
	l->stats.sent = l->sent;
	ReleaseSRWLockExclusive(&l->lock);

	echo_due(l);

	if (!l->ended && l->proved) {
		uint8_t bytes[CL_AUDIO_BYTES];

		/* Every 20 ms, whatever the network does: silence while no player is attached */
		for (unsigned i = 0; i < CL_SAMPLES; i++) {
			const uint16_t v = (uint16_t)(heard ? caller[i] : 0);
			bytes[2 * i] = (uint8_t)(v & 0xff);
			bytes[2 * i + 1] = (uint8_t)(v >> 8);
		}
		send_frame(l, CL_AUDIO, bytes, CL_AUDIO_BYTES);
		++l->sent;
	}
}


static void ticks(struct cl_link *l, uint64_t *next, uint64_t now)
{
	if (now < *next)
		return;

	if (now - *next >= RESTART_US) {
		AcquireSRWLockExclusive(&l->lock);
		++l->stats.restarts;
		ReleaseSRWLockExclusive(&l->lock);
		*next = now;
	}

	while (now >= *next && !l->ended) {
		const uint64_t late = now - *next;

		AcquireSRWLockExclusive(&l->lock);
		if (late >= LATE_US)
			++l->stats.late;
		if (late > l->stats.latest_us)
			l->stats.latest_us = (uint32_t)late;
		ReleaseSRWLockExclusive(&l->lock);

		/* Only the tick for the 20 ms now running feeds baresip's source */
		tick(l, late < TICK_US);
		*next += TICK_US;
	}
}


static void deadlines(struct cl_link *l, uint64_t now)
{
	if (l->listening && !l->proved && now - l->answered_at >= PROVE_US) {
		end(l, CL_END_NO_CONSORT);
		return;
	}

	for (unsigned i = 0; i < l->npending; i++) {
		if (l->pending[i].s != INVALID_SOCKET && now - l->pending[i].since >= ID_US)
			refuse(l, &l->pending[i]);
	}
	compact_pending(l);

	if (l->outlen && now - l->out_since >= TAKE_US)
		end(l, CL_END_NOT_TAKEN);
}


/** Milliseconds until the next deadline (rounded up), or INFINITE */
static DWORD until_deadline(const struct cl_link *l, uint64_t now)
{
	uint64_t at = UINT64_MAX;

	if (l->ended)
		return INFINITE;
	if (l->listening && !l->proved)
		at = l->answered_at + PROVE_US;
	for (unsigned i = 0; i < l->npending; i++) {
		if (l->pending[i].since + ID_US < at)
			at = l->pending[i].since + ID_US;
	}
	if (l->outlen && l->out_since + TAKE_US < at)
		at = l->out_since + TAKE_US;

	if (at == UINT64_MAX)
		return INFINITE;
	if (at <= now)
		return 0;

	return (DWORD)((at - now + 999) / 1000);
}


static DWORD WINAPI run(LPVOID arg)
{
	struct cl_link *l = arg;
	uint64_t next = now_us() + TICK_US;
	HANDLE waits[3];

	waits[0] = l->timer;
	waits[1] = l->wake;
	waits[2] = l->sockev;

	for (;;) {
		char keys[MAX_KEYS];
		unsigned nkeys;
		bool stop, hangup, take;
		uint64_t now = now_us();

		if (!l->ended) {
			LARGE_INTEGER due;
			const uint64_t ahead = next > now ? next - now : 0;

			due.QuadPart = -(LONGLONG)(ahead ? ahead * 10 : 1);
			(void)SetWaitableTimer(l->timer, &due, 0, NULL, NULL, FALSE);
		}

		(void)WaitForMultipleObjects(l->ended ? 2 : 3, waits, FALSE,
					     until_deadline(l, now));

		AcquireSRWLockExclusive(&l->lock);
		stop = l->stop;
		hangup = l->hangup;
		l->hangup = false;
		nkeys = l->nkeys;
		memcpy(keys, l->keys, nkeys);
		l->nkeys = 0;
		take = l->answered && !l->listening && l->listener != INVALID_SOCKET;
		ReleaseSRWLockExclusive(&l->lock);

		/* Keys, then the far end's hang-up, then stop: the glue frees a link right after
		 * the hang-up, and Consort still gets every key pressed before it and HANGUP */
		for (unsigned i = 0; i < nkeys && !l->ended && l->proved; i++) {
			const uint8_t key = (uint8_t)keys[i];
			send_frame(l, CL_DTMF, &key, 1);
		}

		if (hangup && !l->ended) {
			/* The far end hung up: what waited for Consort, HANGUP, then nothing more */
			if (l->proved)
				send_frame(l, CL_HANGUP, NULL, 0);
			if (!l->ended) {
				flush_last(l);
				close_last(l);
			}
			l->ended = true;
			close_all(l);
		}

		if (stop)
			break;

		if (take && !l->ended) {
			if (WSAEventSelect(l->listener, l->sockev, FD_ACCEPT))
				end(l, CL_END_FAILED);
			else
				l->listening = true;
		}

		if (l->ended)
			continue;

		network(l);
		now = now_us();
		if (!l->ended)
			ticks(l, &next, now);
		if (!l->ended)
			deadlines(l, now_us());
	}

	close_all(l);

	return 0;
}


static void destructor(struct cl_link *l)
{
	if (l->timer)
		CloseHandle(l->timer);
	if (l->coarse)
		(void)timeEndPeriod(1);
	if (l->wake)
		CloseHandle(l->wake);
	if (l->sockev != WSA_INVALID_EVENT)
		WSACloseEvent(l->sockev);
	close_socket(&l->listener);

	cl_wipe(l->secret, sizeof(l->secret));
	cl_wipe(l->want, sizeof(l->want));
	cl_wipe(l->mine, sizeof(l->mine));
	free(l);
}


int cl_link_alloc(struct cl_link **lp, const uint8_t secret[CL_SECRET_BYTES],
		  cl_ended_h *endh, void *arg)
{
	struct cl_link *l;

	if (!lp || !secret)
		return EINVAL;

	l = calloc(1, sizeof(*l));
	if (!l)
		return ENOMEM;

	InitializeSRWLock(&l->lock);
	l->listener = INVALID_SOCKET;
	l->conn = INVALID_SOCKET;
	l->sockev = WSA_INVALID_EVENT;
	l->endh = endh;
	l->arg = arg;
	memcpy(l->secret, secret, CL_SECRET_BYTES);
	cl_player_init(&l->player);

	l->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
					  TIMER_ALL_ACCESS);
	if (!l->timer) {
		/* Before Windows 10 1803: an ordinary timer, at 1 ms */
		l->timer = CreateWaitableTimerW(NULL, FALSE, NULL);
		l->coarse = l->timer && timeBeginPeriod(1) == TIMERR_NOERROR;
	}
	l->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	l->sockev = WSACreateEvent();

	if (!l->timer || !l->wake || l->sockev == WSA_INVALID_EVENT) {
		destructor(l);
		return ENOMEM;
	}

	l->thread = CreateThread(NULL, 0, run, l, 0, NULL);
	if (!l->thread) {
		destructor(l);
		return ENOMEM;
	}

	*lp = l;

	return 0;
}


void cl_link_free(struct cl_link *l)
{
	if (!l)
		return;

	AcquireSRWLockExclusive(&l->lock);
	l->stop = true;
	ReleaseSRWLockExclusive(&l->lock);
	SetEvent(l->wake);

	(void)WaitForSingleObject(l->thread, INFINITE);
	CloseHandle(l->thread);

	destructor(l);
}


void cl_link_source(struct cl_link *l, cl_source_h *h, void *arg)
{
	if (!l)
		return;

	AcquireSRWLockExclusive(&l->lock);
	l->srch = h;
	l->srcarg = h ? arg : NULL;
	ReleaseSRWLockExclusive(&l->lock);
}


void cl_link_player(struct cl_link *l, cl_player_h *h, void *arg)
{
	if (!l)
		return;

	AcquireSRWLockExclusive(&l->lock);
	l->playh = h;
	l->playarg = h ? arg : NULL;
	ReleaseSRWLockExclusive(&l->lock);
}


int cl_link_answer(struct cl_link *l, uint16_t *port, uint8_t nonce[CL_NONCE_BYTES])
{
	struct sockaddr_in sin;
	int len = sizeof(sin);
	BOOL yes = TRUE;
	uint8_t n[CL_NONCE_BYTES], want[CL_PROOF_BYTES], mine[CL_PROOF_BYTES];
	uint16_t bound;
	bool already;
	SOCKET s;
	int err;

	if (!l || !port || !nonce)
		return EINVAL;

	AcquireSRWLockExclusive(&l->lock);
	already = l->answered;
	ReleaseSRWLockExclusive(&l->lock);
	if (already)
		return EALREADY;

	s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
	if (s == INVALID_SOCKET)
		return EIO;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = 0;

	/* A long backlog: a flood of connections can't keep Consort's from being queued */
	if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&yes, sizeof(yes)) ||
	    bind(s, (struct sockaddr *)&sin, sizeof(sin)) ||
	    listen(s, SOMAXCONN) ||
	    getsockname(s, (struct sockaddr *)&sin, &len)) {
		(void)closesocket(s);
		return EIO;
	}
	bound = ntohs(sin.sin_port);

	err = cl_random(n, sizeof(n));
	if (!err)
		err = cl_proof(want, l->secret, n, "consort", bound);
	if (!err)
		err = cl_proof(mine, l->secret, n, "line", bound);
	if (err) {
		(void)closesocket(s);
		cl_wipe(want, sizeof(want));
		cl_wipe(mine, sizeof(mine));
		return err;
	}

	AcquireSRWLockExclusive(&l->lock);
	l->listener = s;
	l->port = bound;
	memcpy(l->want, want, sizeof(want));
	memcpy(l->mine, mine, sizeof(mine));
	l->answered_at = now_us();
	l->answered = true;
	ReleaseSRWLockExclusive(&l->lock);
	SetEvent(l->wake);

	cl_wipe(want, sizeof(want));
	cl_wipe(mine, sizeof(mine));

	*port = bound;
	memcpy(nonce, n, sizeof(n));

	return 0;
}


void cl_link_key(struct cl_link *l, char key)
{
	if (!l || !cl_is_key(key))
		return;

	AcquireSRWLockExclusive(&l->lock);
	if (l->nkeys < MAX_KEYS)
		l->keys[l->nkeys++] = key;
	ReleaseSRWLockExclusive(&l->lock);
	SetEvent(l->wake);
}


void cl_link_hangup(struct cl_link *l)
{
	if (!l)
		return;

	AcquireSRWLockExclusive(&l->lock);
	l->hangup = true;
	ReleaseSRWLockExclusive(&l->lock);
	SetEvent(l->wake);
}


bool cl_link_proved(struct cl_link *l)
{
	bool proved;

	if (!l)
		return false;

	AcquireSRWLockExclusive(&l->lock);
	proved = l->proved;
	ReleaseSRWLockExclusive(&l->lock);

	return proved;
}


void cl_link_stats(struct cl_link *l, struct cl_link_stats *stats)
{
	if (!l || !stats)
		return;

	AcquireSRWLockExclusive(&l->lock);
	*stats = l->stats;
	ReleaseSRWLockExclusive(&l->lock);
}
