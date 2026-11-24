/**
 * @file unit.c  The add-on's own tests, without baresip
 *
 * The frames, the player, the proofs (against PROTOCOL.md's check value), and a call's link
 * over real sockets on 127.0.0.1, with this test as Consort and stand-in audio for baresip.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "proto.h"
#include "proof.h"
#include "link.h"

static int checks, failures;

#define CHECK(c) do {                                                         \
		++checks;                                                     \
		if (!(c)) {                                                   \
			++failures;                                           \
			fprintf(stderr, "%s:%d: failed: %s\n", __FILE__,      \
				__LINE__, #c);                                \
		}                                                             \
	} while (0)


static void hex_is(const uint8_t *bytes, size_t len, const char *want, int line)
{
	char got[129];

	cl_hex(got, bytes, len);
	++checks;
	if (strcmp(got, want)) {
		++failures;
		fprintf(stderr, "unit.c:%d: got %s, want %s\n", line, got, want);
	}
}


/*
 * The proofs, the frames and the player
 */

static void test_the_proofs_check_value(void)
{
	uint8_t key[CL_SECRET_BYTES], nonce[CL_NONCE_BYTES], proof[CL_PROOF_BYTES];
	uint8_t frame[CL_ID_FRAME_BYTES];

	for (unsigned i = 0; i < sizeof(key); i++)
		key[i] = (uint8_t)i;
	for (unsigned i = 0; i < sizeof(nonce); i++)
		nonce[i] = (uint8_t)i;

	CHECK(0 == cl_proof(proof, key, nonce, "consort", 40000));
	hex_is(proof, sizeof(proof), "2167cf0b7445558125097303dee3f210", __LINE__);
	CHECK(cl_frame(frame, CL_ID, proof, CL_PROOF_BYTES) == CL_ID_FRAME_BYTES);
	hex_is(frame, sizeof(frame), "0100102167cf0b7445558125097303dee3f210", __LINE__);

	CHECK(0 == cl_proof(proof, key, nonce, "line", 40000));
	hex_is(proof, sizeof(proof), "f1d51df5fa017fd659d37b2b62c483d9", __LINE__);

	/* The port is in it: another port, another proof */
	CHECK(0 == cl_proof(proof, key, nonce, "line", 40001));
	{
		char got[33];
		cl_hex(got, proof, sizeof(proof));
		CHECK(strcmp(got, "f1d51df5fa017fd659d37b2b62c483d9") != 0);
	}
}


static void test_hex_and_same(void)
{
	uint8_t out[4];
	const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};

	CHECK(cl_unhex(out, 4, "0aFf10e9", 8));
	CHECK(out[0] == 0x0a && out[1] == 0xff && out[2] == 0x10 && out[3] == 0xe9);
	CHECK(!cl_unhex(out, 4, "0aFf10e", 7));
	CHECK(!cl_unhex(out, 4, "0aFf10eg", 8));
	CHECK(!cl_unhex(out, 4, "0aFf10e9aa", 10));
	CHECK(cl_same(a, a, 4));
	CHECK(!cl_same(a, b, 4));
	CHECK(cl_random(out, sizeof(out)) == 0);
}


static void test_consorts_frame_sizes(void)
{
	CHECK(cl_consort_payload(CL_HANGUP) == 0);
	CHECK(cl_consort_payload(CL_AUDIO) == 320);
	CHECK(cl_consort_payload(CL_FLUSH) == 0);
	CHECK(cl_consort_payload(CL_MARK) == 4);
	CHECK(cl_consort_payload(CL_ID) == -1);
	CHECK(cl_consort_payload(CL_DTMF) == -1);
	CHECK(cl_consort_payload(CL_FLUSHED) == -1);
	CHECK(cl_consort_payload(CL_ERROR) == -1);
	CHECK(cl_consort_payload(0x12) == -1);
	CHECK(cl_consort_payload(0x7f) == -1);
	CHECK(cl_is_key('0') && cl_is_key('9') && cl_is_key('*') && cl_is_key('#'));
	CHECK(cl_is_key('A') && cl_is_key('D') && !cl_is_key('E') && !cl_is_key('a'));
}


static void audio_of(uint8_t payload[CL_AUDIO_BYTES], int16_t value)
{
	for (unsigned i = 0; i < CL_SAMPLES; i++) {
		const uint16_t v = (uint16_t)value;
		payload[2 * i] = (uint8_t)(v & 0xff);
		payload[2 * i + 1] = (uint8_t)(v >> 8);
	}
}


static struct cl_player player;  /* 16 kB: not on the stack */

static void test_the_player_plays_in_order_and_marks_follow_their_audio(void)
{
	uint8_t payload[CL_AUDIO_BYTES];
	int16_t out[CL_SAMPLES];

	cl_player_init(&player);

	/* Dry: silence, not counted */
	CHECK(!cl_player_play(&player, out));
	CHECK(out[0] == 0 && out[CL_SAMPLES - 1] == 0);
	CHECK(player.since == 0);

	/* A MARK with nothing before it is due at once */
	CHECK(0 == cl_player_mark(&player, 1));
	CHECK(cl_player_due(&player) == 1);
	CHECK(cl_player_due(&player) == 0);

	audio_of(payload, 7);
	CHECK(0 == cl_player_audio(&player, payload));
	audio_of(payload, -8);
	CHECK(0 == cl_player_audio(&player, payload));
	CHECK(0 == cl_player_mark(&player, 2));
	CHECK(cl_player_due(&player) == 0);

	CHECK(cl_player_play(&player, out));
	CHECK(out[0] == 7 && out[CL_SAMPLES - 1] == 7);
	CHECK(cl_player_due(&player) == 0);
	CHECK(player.since == 160);

	CHECK(cl_player_play(&player, out));
	CHECK(out[0] == -8);
	CHECK(player.since == 320);
	CHECK(cl_player_due(&player) == 2);
	CHECK(player.since == 0 && player.played == 2);
}


static void test_the_players_rules(void)
{
	uint8_t payload[CL_AUDIO_BYTES];

	cl_player_init(&player);
	audio_of(payload, 1);

	/* 1 s at most */
	for (unsigned i = 0; i < CL_BUFFER_FRAMES; i++)
		CHECK(0 == cl_player_audio(&player, payload));
	CHECK(cl_player_audio(&player, payload) == CL_ERR_FRAME);

	/* Marks: from 1, each the next, 64 waiting at most */
	cl_player_init(&player);
	CHECK(cl_player_mark(&player, 0) == CL_ERR_FRAME);
	CHECK(cl_player_mark(&player, 2) == CL_ERR_FRAME);
	CHECK(0 == cl_player_mark(&player, 1));
	CHECK(cl_player_mark(&player, 1) == CL_ERR_FRAME);
	CHECK(cl_player_mark(&player, 3) == CL_ERR_FRAME);

	cl_player_init(&player);
	CHECK(0 == cl_player_audio(&player, payload));
	for (uint32_t id = 1; id <= CL_MAX_MARKS; id++)
		CHECK(0 == cl_player_mark(&player, id));
	CHECK(cl_player_mark(&player, CL_MAX_MARKS + 1) == CL_ERR_FRAME);
}


static void test_flush_drops_whats_queued_and_says_where_playout_stood(void)
{
	uint8_t payload[CL_AUDIO_BYTES];
	int16_t out[CL_SAMPLES];
	uint32_t mark, samples;

	cl_player_init(&player);
	audio_of(payload, 3);

	cl_player_flush(&player, &mark, &samples);
	CHECK(mark == 0 && samples == 0);

	CHECK(0 == cl_player_audio(&player, payload));
	CHECK(0 == cl_player_mark(&player, 1));
	CHECK(cl_player_play(&player, out));
	CHECK(cl_player_due(&player) == 1);

	for (unsigned i = 0; i < 5; i++)
		CHECK(0 == cl_player_audio(&player, payload));
	CHECK(0 == cl_player_mark(&player, 2));
	CHECK(cl_player_play(&player, out));
	CHECK(cl_player_play(&player, out));

	cl_player_flush(&player, &mark, &samples);
	CHECK(mark == 1 && samples == 320);
	CHECK(player.count == 0 && player.mark_count == 0);

	/* The mark the flush dropped never comes back; the count isn't reset by a flush */
	CHECK(!cl_player_play(&player, out));
	CHECK(cl_player_due(&player) == 0);
	cl_player_flush(&player, &mark, &samples);
	CHECK(mark == 1 && samples == 320);

	/* The next MARK carries the order on, and is due at once with nothing before it */
	CHECK(cl_player_mark(&player, 2) == CL_ERR_FRAME);
	CHECK(0 == cl_player_mark(&player, 3));
	CHECK(cl_player_due(&player) == 3);
	cl_player_flush(&player, &mark, &samples);
	CHECK(mark == 3 && samples == 0);
}


/*
 * A call's link over real sockets
 */

struct audio {
	SRWLOCK lock;
	int16_t pulled;          /* the value of the next frame the player hands */
	unsigned pushed;         /* frames the source took                     */
	int16_t heard[256];      /* the first sample of each non-silent frame  */
	unsigned nheard;
};

static void source(const int16_t samples[CL_SAMPLES], void *arg)
{
	struct audio *a = arg;

	AcquireSRWLockExclusive(&a->lock);
	++a->pushed;
	if (samples[0] != 0 && a->nheard < 256)
		a->heard[a->nheard++] = samples[0];
	ReleaseSRWLockExclusive(&a->lock);
}


static void player_h(int16_t samples[CL_SAMPLES], void *arg)
{
	struct audio *a = arg;

	AcquireSRWLockExclusive(&a->lock);
	++a->pulled;
	for (unsigned i = 0; i < CL_SAMPLES; i++)
		samples[i] = a->pulled;
	ReleaseSRWLockExclusive(&a->lock);
}


struct ending {
	HANDLE ev;
	volatile LONG why;
	volatile LONG times;
};

static void on_ended(enum cl_end why, void *arg)
{
	struct ending *e = arg;

	InterlockedExchange(&e->why, (LONG)why);
	InterlockedIncrement(&e->times);
	SetEvent(e->ev);
}


static uint8_t secret[CL_SECRET_BYTES];

static SOCKET dial(uint16_t port)
{
	struct sockaddr_in sin;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	DWORD tmo = 3000;
	BOOL yes = TRUE;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);

	(void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
	(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
	if (connect(s, (struct sockaddr *)&sin, sizeof(sin))) {
		closesocket(s);
		return INVALID_SOCKET;
	}

	return s;
}


static bool send_all(SOCKET s, const uint8_t *p, size_t n)
{
	while (n) {
		const int k = send(s, (const char *)p, (int)n, 0);
		if (k <= 0)
			return false;
		p += k;
		n -= (size_t)k;
	}

	return true;
}


static bool send_frame(SOCKET s, uint8_t type, const uint8_t *payload, uint16_t len)
{
	uint8_t frame[CL_MAX_FRAME_BYTES];

	return send_all(s, frame, cl_frame(frame, type, payload, len));
}


static int recv_all(SOCKET s, uint8_t *p, size_t n)
{
	while (n) {
		const int k = recv(s, (char *)p, (int)n, 0);
		if (k == 0)
			return 0;
		if (k < 0)
			return -1;
		p += k;
		n -= (size_t)k;
	}

	return 1;
}


/** The next frame: 1, 0 when the line closed, -1 on a timeout or error */
static int read_frame(SOCKET s, uint8_t *type, uint8_t *payload, uint16_t *len)
{
	uint8_t header[CL_HEADER_BYTES];
	int r = recv_all(s, header, sizeof(header));

	if (r != 1)
		return r;

	*type = header[0];
	*len = (uint16_t)((header[1] << 8) | header[2]);
	if (*len > CL_AUDIO_BYTES)
		return -1;

	return *len ? recv_all(s, payload, *len) : 1;
}


/** The next frame of this type, AUDIO skipped (and counted): 1, 0 closed, -1 not in time */
static int expect(SOCKET s, uint8_t want, uint8_t *payload, uint16_t *len, unsigned *audio)
{
	for (unsigned i = 0; i < 500; i++) {
		uint8_t type;
		int r = read_frame(s, &type, payload, len);

		if (r != 1)
			return r;
		if (type == want)
			return 1;
		if (type == CL_AUDIO && audio)
			++*audio;
		else if (type != CL_AUDIO) {
			fprintf(stderr, "unit.c: frame 0x%02x while waiting for 0x%02x\n",
				type, want);
			return -1;
		}
	}

	return -1;
}


static void consort_id(uint8_t frame[CL_ID_FRAME_BYTES], const uint8_t nonce[CL_NONCE_BYTES],
		       uint16_t port, const char *side)
{
	uint8_t proof[CL_PROOF_BYTES];

	(void)cl_proof(proof, secret, nonce, side, port);
	(void)cl_frame(frame, CL_ID, proof, CL_PROOF_BYTES);
}


struct call {
	struct cl_link *link;
	struct audio audio;
	struct ending ending;
	uint16_t port;
	uint8_t nonce[CL_NONCE_BYTES];
};

static void call_start(struct call *c)
{
	memset(c, 0, sizeof(*c));
	InitializeSRWLock(&c->audio.lock);
	c->ending.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(0 == cl_link_alloc(&c->link, secret, on_ended, &c->ending));
	cl_link_source(c->link, source, &c->audio);
	cl_link_player(c->link, player_h, &c->audio);
}


static void call_end(struct call *c)
{
	cl_link_source(c->link, NULL, NULL);
	cl_link_player(c->link, NULL, NULL);
	cl_link_free(c->link);
	CloseHandle(c->ending.ev);
}


/** Proves Consort on a new connection and checks the line's ID back */
static SOCKET prove(struct call *c)
{
	uint8_t id[CL_ID_FRAME_BYTES], want[CL_ID_FRAME_BYTES], back[CL_ID_FRAME_BYTES];
	SOCKET s = dial(c->port);

	CHECK(s != INVALID_SOCKET);
	consort_id(id, c->nonce, c->port, "consort");
	CHECK(send_all(s, id, sizeof(id)));
	CHECK(recv_all(s, back, sizeof(back)) == 1);
	consort_id(want, c->nonce, c->port, "line");
	CHECK(memcmp(back, want, sizeof(want)) == 0);

	return s;
}


static void test_a_call_proves_both_ways_and_its_audio_flows(void)
{
	struct call c;
	uint8_t payload[CL_AUDIO_BYTES], type;
	uint16_t len;
	unsigned audio = 0, frames = 0;
	int16_t last = 0;
	bool consecutive = true;
	struct sockaddr_in sin;
	SOCKET s, twin;
	DWORD start;

	call_start(&c);
	Sleep(100);   /* ticking before the answer: the caller's audio is read and dropped */
	CHECK(0 == cl_link_answer(c.link, &c.port, c.nonce));
	CHECK(c.port != 0);
	CHECK(cl_link_answer(c.link, &c.port, c.nonce) == EALREADY);

	/* Bound exclusively: nothing else binds the port */
	twin = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(c.port);
	CHECK(bind(twin, (struct sockaddr *)&sin, sizeof(sin)) == SOCKET_ERROR);
	closesocket(twin);

	s = prove(&c);
	CHECK(cl_link_proved(c.link));

	/* The caller's audio: a frame every 20 ms, in order, none from before the proof */
	start = GetTickCount();
	while (GetTickCount() - start < 1000) {
		if (read_frame(s, &type, payload, &len) != 1)
			break;
		if (type != CL_AUDIO)
			continue;
		{
			const int16_t v = (int16_t)(payload[0] | (payload[1] << 8));
			if (frames && v != (int16_t)(last + 1))
				consecutive = false;
			last = v;
			++frames;
		}
	}
	CHECK(frames >= 45 && frames <= 56);
	CHECK(consecutive);

	/* Consort's audio plays in order, and its MARK comes back once it has played */
	for (int16_t v = 1000; v < 1003; v++) {
		audio_of(payload, v);
		CHECK(send_frame(s, CL_AUDIO, payload, CL_AUDIO_BYTES));
	}
	{
		uint8_t m[4];
		cl_put_u32(m, 1);
		CHECK(send_frame(s, CL_MARK, m, 4));
	}
	CHECK(expect(s, CL_MARK, payload, &len, &audio) == 1);
	CHECK(len == 4 && cl_get_u32(payload) == 1);
	AcquireSRWLockExclusive(&c.audio.lock);
	CHECK(c.audio.nheard == 3);
	CHECK(c.audio.heard[0] == 1000 && c.audio.heard[1] == 1001 && c.audio.heard[2] == 1002);
	ReleaseSRWLockExclusive(&c.audio.lock);

	/* FLUSH with nothing queued: FLUSHED names MARK 1 and no samples since */
	CHECK(send_frame(s, CL_FLUSH, NULL, 0));
	CHECK(expect(s, CL_FLUSHED, payload, &len, &audio) == 1);
	CHECK(len == 8 && cl_get_u32(payload) == 1 && cl_get_u32(payload + 4) == 0);

	/* Ten frames, then FLUSH at once: the count is whole frames, no more than were sent */
	for (unsigned i = 0; i < 10; i++) {
		audio_of(payload, 2000);
		CHECK(send_frame(s, CL_AUDIO, payload, CL_AUDIO_BYTES));
	}
	CHECK(send_frame(s, CL_FLUSH, NULL, 0));
	CHECK(expect(s, CL_FLUSHED, payload, &len, &audio) == 1);
	CHECK(cl_get_u32(payload) == 1);
	CHECK(cl_get_u32(payload + 4) % CL_SAMPLES == 0 && cl_get_u32(payload + 4) <= 1600);

	/* After a FLUSH the next MARK carries the order on, due at once */
	{
		uint8_t m[4];
		cl_put_u32(m, 2);
		CHECK(send_frame(s, CL_MARK, m, 4));
	}
	CHECK(expect(s, CL_MARK, payload, &len, &audio) == 1);
	CHECK(cl_get_u32(payload) == 2);

	/* A key: one DTMF frame */
	cl_link_key(c.link, '5');
	cl_link_key(c.link, 'x');   /* not a key: never sent */
	cl_link_key(c.link, '#');
	CHECK(expect(s, CL_DTMF, payload, &len, &audio) == 1);
	CHECK(len == 1 && payload[0] == '5');
	CHECK(expect(s, CL_DTMF, payload, &len, &audio) == 1);
	CHECK(len == 1 && payload[0] == '#');

	/* The far end hangs up: HANGUP, then the line closes; no ended handler */
	cl_link_hangup(c.link);
	CHECK(expect(s, CL_HANGUP, payload, &len, &audio) == 1);
	CHECK(read_frame(s, &type, payload, &len) == 0);
	CHECK(WaitForSingleObject(c.ending.ev, 300) == WAIT_TIMEOUT);

	{
		struct cl_link_stats st;
		cl_link_stats(c.link, &st);
		CHECK(st.ticks >= 50);
		CHECK(st.played >= 3);
		CHECK(st.sent >= 45);
		printf("  ticks %llu, late %llu, latest %u us, restarts %u, most queued %u\n",
		       (unsigned long long)st.ticks, (unsigned long long)st.late,
		       st.latest_us, st.restarts, st.most_queued);
	}

	closesocket(s);
	call_end(&c);
}


static void test_wrong_late_and_extra_connections_are_closed_and_the_call_goes_on(void)
{
	struct call c;
	uint8_t id[CL_ID_FRAME_BYTES], buf[64];
	SOCKET wrong, silent, side, s, extra;
	struct cl_link_stats st;

	call_start(&c);
	CHECK(0 == cl_link_answer(c.link, &c.port, c.nonce));

	/* The line's proof sent back as Consort's: refused */
	wrong = dial(c.port);
	consort_id(id, c.nonce, c.port, "line");
	CHECK(send_all(wrong, id, sizeof(id)));
	CHECK(recv(wrong, (char *)buf, sizeof(buf), 0) <= 0);
	closesocket(wrong);

	/* A proof for another port: refused */
	side = dial(c.port);
	consort_id(id, c.nonce, (uint16_t)(c.port + 1), "consort");
	CHECK(send_all(side, id, sizeof(id)));
	CHECK(recv(side, (char *)buf, sizeof(buf), 0) <= 0);
	closesocket(side);

	/* Nothing within a second: closed */
	silent = dial(c.port);
	{
		const DWORD start = GetTickCount();
		CHECK(recv(silent, (char *)buf, sizeof(buf), 0) <= 0);
		CHECK(GetTickCount() - start >= 800);
	}
	closesocket(silent);

	/* The call went on: the right ID still works */
	s = prove(&c);

	/* A connection after the right one: closed at once */
	extra = dial(c.port);
	CHECK(extra != INVALID_SOCKET);
	consort_id(id, c.nonce, c.port, "consort");
	(void)send_all(extra, id, sizeof(id));
	CHECK(recv(extra, (char *)buf, sizeof(buf), 0) <= 0);
	closesocket(extra);

	CHECK(WaitForSingleObject(c.ending.ev, 200) == WAIT_TIMEOUT);
	cl_link_stats(c.link, &st);
	CHECK(st.refused >= 4);

	closesocket(s);
	CHECK(WaitForSingleObject(c.ending.ev, 2000) == WAIT_OBJECT_0);
	CHECK(c.ending.why == CL_END_CLOSED && c.ending.times == 1);
	call_end(&c);
}


static void test_no_right_id_within_2_seconds_ends_the_calls_audio(void)
{
	struct call c;
	DWORD start;

	call_start(&c);
	CHECK(0 == cl_link_answer(c.link, &c.port, c.nonce));
	start = GetTickCount();
	CHECK(WaitForSingleObject(c.ending.ev, 4000) == WAIT_OBJECT_0);
	CHECK(GetTickCount() - start >= 1900);
	CHECK(c.ending.why == CL_END_NO_CONSORT);

	/* The listener is gone with it */
	CHECK(dial(c.port) == INVALID_SOCKET);
	call_end(&c);
}


/** A frame of Consort's that breaks a rule: ERROR 2, then the line closes */
static void broken_by(const uint8_t *bytes, size_t n, const char *what)
{
	struct call c;
	uint8_t payload[CL_AUDIO_BYTES];
	uint16_t len;
	SOCKET s;
	int r;

	call_start(&c);
	CHECK(0 == cl_link_answer(c.link, &c.port, c.nonce));
	s = prove(&c);
	CHECK(send_all(s, bytes, n));

	r = expect(s, CL_ERROR, payload, &len, NULL);
	++checks;
	if (r != 1 || len != 1 || payload[0] != CL_ERR_FRAME) {
		++failures;
		fprintf(stderr, "unit.c: %s: no ERROR 2 (%d)\n", what, r);
	}
	CHECK(WaitForSingleObject(c.ending.ev, 2000) == WAIT_OBJECT_0);
	CHECK(c.ending.why == CL_END_RULE);

	closesocket(s);
	call_end(&c);
}


static void test_a_frame_that_breaks_a_rule_is_error_2(void)
{
	uint8_t bytes[60 * (CL_HEADER_BYTES + CL_AUDIO_BYTES)];
	uint8_t payload[CL_AUDIO_BYTES] = {0};
	size_t n = 0;

	/* A type Consort doesn't send */
	n = cl_frame(bytes, CL_FLUSHED, payload, 8);
	broken_by(bytes, n, "FLUSHED from Consort");

	n = cl_frame(bytes, CL_ID, payload, CL_PROOF_BYTES);
	broken_by(bytes, n, "a second ID");

	n = cl_frame(bytes, 0x12, payload, 0);
	broken_by(bytes, n, "0x12");

	/* A payload not its type's size, judged from the header alone */
	bytes[0] = CL_AUDIO;
	bytes[1] = 0x01;
	bytes[2] = 0x3f;   /* 319 */
	broken_by(bytes, CL_HEADER_BYTES, "AUDIO of 319 bytes");

	n = cl_frame(bytes, CL_FLUSH, payload, 1);
	broken_by(bytes, n, "FLUSH with a payload");

	/* A MARK out of order */
	cl_put_u32(payload, 2);
	n = cl_frame(bytes, CL_MARK, payload, 4);
	broken_by(bytes, n, "MARK 2 first");

	/* AUDIO that finds the buffer full: 60 frames at once */
	n = 0;
	for (unsigned i = 0; i < 60; i++) {
		audio_of(payload, 5);
		n += cl_frame(bytes + n, CL_AUDIO, payload, CL_AUDIO_BYTES);
	}
	broken_by(bytes, n, "a full buffer");
}


static void test_consorts_hangup_and_close_end_the_calls_audio(void)
{
	struct call c;
	SOCKET s;

	call_start(&c);
	CHECK(0 == cl_link_answer(c.link, &c.port, c.nonce));
	s = prove(&c);
	CHECK(send_frame(s, CL_HANGUP, NULL, 0));
	CHECK(WaitForSingleObject(c.ending.ev, 2000) == WAIT_OBJECT_0);
	CHECK(c.ending.why == CL_END_HANGUP && c.ending.times == 1);
	closesocket(s);
	call_end(&c);

	call_start(&c);
	CHECK(0 == cl_link_answer(c.link, &c.port, c.nonce));
	s = prove(&c);
	closesocket(s);
	CHECK(WaitForSingleObject(c.ending.ev, 2000) == WAIT_OBJECT_0);
	CHECK(c.ending.why == CL_END_CLOSED && c.ending.times == 1);
	call_end(&c);
}


int main(void)
{
	WSADATA wsa;

	if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
		fprintf(stderr, "no Winsock\n");
		return 2;
	}
	(void)cl_random(secret, sizeof(secret));

	test_the_proofs_check_value();
	test_hex_and_same();
	test_consorts_frame_sizes();
	test_the_player_plays_in_order_and_marks_follow_their_audio();
	test_the_players_rules();
	test_flush_drops_whats_queued_and_says_where_playout_stood();
	test_a_call_proves_both_ways_and_its_audio_flows();
	test_wrong_late_and_extra_connections_are_closed_and_the_call_goes_on();
	test_no_right_id_within_2_seconds_ends_the_calls_audio();
	test_a_frame_that_breaks_a_rule_is_error_2();
	test_consorts_hangup_and_close_end_the_calls_audio();

	WSACleanup();
	printf("%d checks, %d failed\n", checks, failures);

	return failures ? 1 : 0;
}
