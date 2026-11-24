/**
 * @file wake.h  Waking baresip's main thread from another thread, safely
 *
 * re's mqueue on Windows is a loopback socket pair that takes whichever program connects
 * first, and its messages carry pointers. This one is a socket pair that checks its peer
 * (its own connection, by address and a random token) before it closes the listener, and
 * carries no message: only a byte that wakes the main thread, which then takes what was
 * posted from memory (consort.c's queue).
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef CONSORT_WAKE_H
#define CONSORT_WAKE_H

struct cl_wake;

/** Called on the main thread after one or more cl_wake_post calls */
typedef void (cl_wake_h)(void *arg);

/** On the main thread, inside re's main loop: 0 or an error number */
int  cl_wake_alloc(struct cl_wake **wp, cl_wake_h *h, void *arg);

/** From any thread */
void cl_wake_post(struct cl_wake *w);

/** On the main thread */
void cl_wake_free(struct cl_wake *w);

#endif
