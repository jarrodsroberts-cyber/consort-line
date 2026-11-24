/**
 * @file link.h  One call's link to Consort: its listener, the proofs, the frames and the tick
 *
 * Plain C and Winsock, no baresip: the module's glue (consort.c) attaches baresip's source
 * and player, and test/unit.c drives a link with real sockets and stand-in audio.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef CONSORT_LINK_H
#define CONSORT_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "proto.h"

struct cl_link;

/** Hands a frame of Consort's audio (or silence) to baresip's source: on the link's thread. */
typedef void (cl_source_h)(const int16_t samples[CL_SAMPLES], void *arg);

/** Fills samples with the caller's next 20 ms from baresip's player: on the link's thread. */
typedef void (cl_player_h)(int16_t samples[CL_SAMPLES], void *arg);

/** Why a call's audio ended on the link's side */
enum cl_end {
	CL_END_NONE = 0,
	CL_END_HANGUP,       /**< Consort sent HANGUP                          */
	CL_END_CLOSED,       /**< Consort's connection closed                  */
	CL_END_NO_CONSORT,   /**< No right ID within 2 s of the answer         */
	CL_END_RULE,         /**< A frame of Consort's broke a rule: ERROR 2   */
	CL_END_NOT_TAKEN,    /**< Consort didn't take the line's frames in 2 s */
	CL_END_FAILED,       /**< The link itself failed: ERROR 4 if it could  */
};

/**
 * The call's audio ended on the link's side: called once, on the link's thread, without the
 * link's lock. The glue hangs the call up on baresip's main thread. Not called when the far
 * end hung up (cl_link_hangup) or the link is freed.
 */
typedef void (cl_ended_h)(enum cl_end why, void *arg);

/** What a link measured, for the closed event: figures only, never audio */
struct cl_link_stats {
	uint64_t ticks;          /**< Ticks run                                   */
	uint64_t late;           /**< Ticks that ran 2 ms or more after their time */
	uint32_t latest_us;      /**< The latest a tick ran, in microseconds      */
	uint32_t restarts;       /**< Times the clock started again (over 100 ms late) */
	uint64_t played;         /**< Frames of Consort's audio played            */
	uint64_t dry;            /**< Ticks with the buffer dry between two of Consort's frames */
	uint32_t most_queued;    /**< The most frames waiting after one played    */
	uint64_t sent;           /**< AUDIO frames sent to Consort                */
	uint32_t refused;        /**< Connections closed for a wrong or missing ID, or late */
};

/** A link for one call, ticking at once: before the answer it plays silence and drops the caller. */
int  cl_link_alloc(struct cl_link **lp, const uint8_t secret[CL_SECRET_BYTES],
		   cl_ended_h *endh, void *arg);

/** Stops the link's thread, closes every socket and wipes its keys. */
void cl_link_free(struct cl_link *l);

/** Attaches baresip's source (NULL detaches): waits for a tick that is running to finish. */
void cl_link_source(struct cl_link *l, cl_source_h *h, void *arg);

/** Attaches baresip's player (NULL detaches): waits for a tick that is running to finish. */
void cl_link_player(struct cl_link *l, cl_player_h *h, void *arg);

/**
 * The call is answered: binds the call's listener on 127.0.0.1 (a port Windows picks, bound
 * exclusively) and makes its nonce, for the answered event. Consort's right ID must come
 * within 2 seconds. Once only; 0 or an error number.
 */
int  cl_link_answer(struct cl_link *l, uint16_t *port, uint8_t nonce[CL_NONCE_BYTES]);

/** A key the far end pressed: one DTMF frame, once Consort's ID has checked out. */
void cl_link_key(struct cl_link *l, char key);

/** The far end hung up: HANGUP to Consort, then the link closes (no ended handler). */
void cl_link_hangup(struct cl_link *l);

/** Whether Consort's ID has checked out */
bool cl_link_proved(struct cl_link *l);

/** The link's figures so far */
void cl_link_stats(struct cl_link *l, struct cl_link_stats *stats);

#endif
