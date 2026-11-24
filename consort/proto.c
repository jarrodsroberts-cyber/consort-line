/**
 * @file proto.c  The frames between the line and Consort, and the line's player
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#include <string.h>
#include "proto.h"


int cl_consort_payload(uint8_t type)
{
	switch (type) {

	case CL_HANGUP: return 0;
	case CL_AUDIO:  return CL_AUDIO_BYTES;
	case CL_FLUSH:  return 0;
	case CL_MARK:   return 4;
	default:        return -1;
	}
}


size_t cl_frame(uint8_t *out, uint8_t type, const uint8_t *payload, uint16_t len)
{
	out[0] = type;
	out[1] = (uint8_t)(len >> 8);
	out[2] = (uint8_t)(len & 0xff);
	if (len)
		memcpy(out + CL_HEADER_BYTES, payload, len);

	return CL_HEADER_BYTES + (size_t)len;
}


bool cl_is_key(char key)
{
	return (key >= '0' && key <= '9') || key == '*' || key == '#' ||
		(key >= 'A' && key <= 'D');
}


void cl_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}


uint32_t cl_get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | (uint32_t)p[3];
}


void cl_player_init(struct cl_player *p)
{
	memset(p, 0, sizeof(*p));
}


int cl_player_audio(struct cl_player *p, const uint8_t payload[CL_AUDIO_BYTES])
{
	unsigned at;
	int16_t *frame;

	if (p->count >= CL_BUFFER_FRAMES)
		return CL_ERR_FRAME;

	at = (p->head + p->count) % CL_BUFFER_FRAMES;
	frame = p->frames[at];
	for (unsigned i = 0; i < CL_SAMPLES; i++) {
		const uint16_t lo = payload[2 * i], hi = payload[2 * i + 1];
		frame[i] = (int16_t)(uint16_t)(lo | (hi << 8));
	}

	++p->count;
	++p->queued;

	return 0;
}


int cl_player_mark(struct cl_player *p, uint32_t id)
{
	unsigned at;

	if (id == 0 || id != p->received + 1)
		return CL_ERR_FRAME;
	if (p->mark_count >= CL_MAX_MARKS)
		return CL_ERR_FRAME;

	at = (p->mark_head + p->mark_count) % CL_MAX_MARKS;
	p->marks[at].id = id;
	p->marks[at].after = p->queued;
	++p->mark_count;
	p->received = id;

	return 0;
}


bool cl_player_play(struct cl_player *p, int16_t out[CL_SAMPLES])
{
	if (!p->count) {
		memset(out, 0, CL_SAMPLES * sizeof(int16_t));
		return false;
	}

	memcpy(out, p->frames[p->head], CL_SAMPLES * sizeof(int16_t));
	p->head = (p->head + 1) % CL_BUFFER_FRAMES;
	--p->count;
	++p->gone;
	p->since += CL_SAMPLES;

	return true;
}


uint32_t cl_player_due(struct cl_player *p)
{
	uint32_t id;

	if (!p->mark_count || p->marks[p->mark_head].after > p->gone)
		return 0;

	id = p->marks[p->mark_head].id;
	p->mark_head = (p->mark_head + 1) % CL_MAX_MARKS;
	--p->mark_count;
	p->played = id;
	p->since = 0;

	return id;
}


void cl_player_flush(struct cl_player *p, uint32_t *mark, uint32_t *samples)
{
	p->gone += p->count;
	p->head = 0;
	p->count = 0;
	p->mark_head = 0;
	p->mark_count = 0;

	*mark = p->played;
	*samples = p->since;
}
