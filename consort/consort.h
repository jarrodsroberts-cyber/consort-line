/**
 * @file consort.h  What main.c asks of the add-on before baresip starts
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#ifndef CONSORT_LINE_H
#define CONSORT_LINE_H

#include <stdint.h>

#define CONSORT_LINE_VERSION "0.1.0"

/**
 * Takes standard output for the control channel (baresip's log goes to standard error),
 * starts the reader of standard input, and reads the first two lines: the secret, then the
 * settings. 0; EBADMSG when the first two lines weren't right; or another error number when
 * the line couldn't start. Either way the failed event is written first.
 */
int consort_line_open(void);

/** Writes {"event": "failed", "why": ...} */
void consort_line_failed(const char *why);

/** The settings: where SIP listens, the line's one account, how many calls at once */
const char *consort_line_listen(void);
const char *consort_line_account(void);
uint32_t consort_line_calls(void);

/**
 * Writes hello, the first line on standard output: main.c calls it once the account is
 * made, just before re_main. 0, or an error number when it couldn't be written.
 */
int consort_line_hello(void);

/** What main.c exits with once re_main returns: 1 when the channel broke a rule, else 0 */
int consort_line_code(void);

#endif
