/**
 * @file proto.h  The frames between the line and Consort, and the line's player
 *
 * Plain C, no sockets and no baresip, so every rule here is tested alone (test/unit.c).
 * PROTOCOL.md describes the frames whole.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef CONSORT_PROTO_H
#define CONSORT_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	CL_RATE          = 8000,  /**< Samples a second, both ways             */
	CL_SAMPLES       = 160,   /**< A frame: 20 ms of samples               */
	CL_AUDIO_BYTES   = 320,   /**< A frame's payload: 16-bit little-endian */
	CL_BUFFER_FRAMES = 50,    /**< The injection buffer holds 1 s at most  */
	CL_MAX_MARKS     = 64,    /**< MARKs queued at once, at most           */
	CL_SECRET_BYTES  = 32,
	CL_NONCE_BYTES   = 16,
	CL_PROOF_BYTES   = 16,
	CL_HEADER_BYTES  = 3,     /**< Type, then the length (big-endian)      */
	CL_ID_FRAME_BYTES = CL_HEADER_BYTES + CL_PROOF_BYTES,
	CL_MAX_FRAME_BYTES = CL_HEADER_BYTES + CL_AUDIO_BYTES,
};

/** Frame types, in AudioSocket's form, with three of Consort's own */
enum cl_type {
	CL_HANGUP  = 0x00,
	CL_ID      = 0x01,
	CL_DTMF    = 0x03,
	CL_AUDIO   = 0x10,
	CL_FLUSH   = 0x20,
	CL_FLUSHED = 0x21,
	CL_MARK    = 0x22,
	CL_ERROR   = 0xff,
};

/** ERROR's codes, AudioSocket's own */
enum cl_error_code {
	CL_ERR_HUNG_UP = 1,  /**< The far end hung up             */
	CL_ERR_FRAME   = 2,  /**< A frame the line couldn't forward */
	CL_ERR_MEMORY  = 4,  /**< Out of memory                   */
};

/**
 * The payload size one of Consort's frames must have, or -1 for a type Consort never sends
 * once its ID has checked out (ID again, DTMF, FLUSHED, ERROR, 0x12 or any other).
 * Judged from the first three bytes, before the payload is read.
 */
int cl_consort_payload(uint8_t type);

/** Writes a frame's header and payload to out (CL_HEADER_BYTES + len bytes); returns its size. */
size_t cl_frame(uint8_t *out, uint8_t type, const uint8_t *payload, uint16_t len);

/** A key a DTMF frame may carry: 0-9, *, #, A-D */
bool cl_is_key(char key);

/**
 * The line's player: Consort's audio, a frame each tick from a buffer of 1 s at most, and
 * the MARKs queued in order with it. Silence when dry, and silence isn't counted.
 */
struct cl_player {
	int16_t frames[CL_BUFFER_FRAMES][CL_SAMPLES];
	unsigned head;           /**< The first frame queued              */
	unsigned count;          /**< Frames queued                       */
	uint64_t queued;         /**< Frames ever queued                  */
	uint64_t gone;           /**< Frames ever played or flushed       */
	struct {
		uint32_t id;
		uint64_t after;  /**< Due once this many frames are gone */
	} marks[CL_MAX_MARKS];
	unsigned mark_head;
	unsigned mark_count;
	uint32_t received;       /**< The last MARK id received (0: none) */
	uint32_t played;         /**< The last MARK played (0: none)      */
	uint32_t since;          /**< Samples of Consort's played since it */
};

void cl_player_init(struct cl_player *p);

/** AUDIO from Consort. CL_ERR_FRAME when the buffer is full, else 0. */
int cl_player_audio(struct cl_player *p, const uint8_t payload[CL_AUDIO_BYTES]);

/**
 * MARK from Consort: the next id (last received + 1) or CL_ERR_FRAME, as is a 65th waiting.
 * A MARK with nothing queued before it is due at once (cl_player_due).
 */
int cl_player_mark(struct cl_player *p, uint32_t id);

/** A tick's frame: the next queued (true) or silence (false: the buffer was dry). */
bool cl_player_play(struct cl_player *p, int16_t out[CL_SAMPLES]);

/**
 * The next MARK now due, as playout reached it (every frame before it gone), or 0 for none.
 * Each one returned is played: the count since it starts again from 0.
 */
uint32_t cl_player_due(struct cl_player *p);

/**
 * FLUSH: drops every frame and MARK queued; says where playout stood (the last MARK played,
 * 0 for none, and the samples of Consort's audio played since it). The count goes on: only a
 * MARK played starts it again.
 */
void cl_player_flush(struct cl_player *p, uint32_t *mark, uint32_t *samples);

/** Big-endian helpers */
void cl_put_u32(uint8_t *p, uint32_t v);
uint32_t cl_get_u32(const uint8_t *p);

#endif
