/**
 * @file main.c  consort-line: baresip and Consort's add-on, as one program
 *
 * Standard input and output are the control channel from the first byte: nothing else is
 * printed there, no configuration file is read or written, and only two modules are ever
 * loaded, g711 and the add-on. Consort writes two lines first,
 *
 *   {"line": 1, "secret": "<64 hex>"}
 *   {"listen": "127.0.0.1:<port>", "account": "sip:<user>@127.0.0.1", "calls": <1 to 16>}
 *
 * then commands, and the add-on's events come back (PROTOCOL.md). It takes no arguments.
 * Exit codes: 0 when Consort said quit or closed its end, 2 when the first lines weren't
 * right, 1 when a later line broke a rule, or for anything else.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#include <winsock2.h>
#include <errno.h>
#include <re.h>
#include <baresip.h>
#include <stdlib.h>
#include <string.h>
#include "consort.h"


/** baresip's settings, from Consort's few: everything else is fixed here */
static int configure(void)
{
	char conf[1024];
	const int n = re_snprintf(conf, sizeof(conf),
		"sip_listen\t\t%s\n"
		"sip_transports\t\tudp\n"
		"net_interface\t\t127.0.0.1\n"
		"call_accept\t\tyes\n"
		"call_max_calls\t\t%u\n"
		"call_hold_other_calls\tno\n"
		"audio_player\t\tconsort,none\n"
		"audio_source\t\tconsort,none\n"
		"audio_alert\t\tconsort,none\n"
		"ausrc_srate\t\t8000\n"
		"auplay_srate\t\t8000\n"
		"ausrc_channels\t\t1\n"
		"auplay_channels\t\t1\n",
		consort_line_listen(), consort_line_calls());

	if (n <= 0 || (size_t)n >= sizeof(conf))
		return ENOMEM;

	return conf_configure_buf((const uint8_t *)conf, (size_t)n);
}


/** The line's one account: never registers, never answers by itself, never transfers */
static int account(struct ua **uap)
{
	char aor[256];
	const int n = re_snprintf(aor, sizeof(aor),
		"<%s>;regint=0;answermode=manual;sip_autoanswer=no;call_transfer=no;"
		"mwi=no;audio_codecs=PCMU,PCMA;ptime=20",
		consort_line_account());

	if (n <= 0 || (size_t)n >= sizeof(aor))
		return ENOMEM;

	return ua_alloc(uap, aor);
}


int main(void)
{
	struct ua *ua = NULL;
	int err, code = 1;

	err = libre_init();
	if (err)
		return 1;

	err = consort_line_open();
	if (err) {
		/* 2 when the first two lines weren't right, 1 when the line couldn't start */
		code = err == EBADMSG ? 2 : 1;
		goto out;
	}

	err = configure();
	if (!err)
		err = baresip_init(conf_config());
	if (!err)
		err = ua_init("consort-line/" CONSORT_LINE_VERSION, true, false, false);
	if (!err)
		err = module_preload("g711.so");
	if (!err)
		err = module_preload("consort.so");
	if (!err)
		err = account(&ua);
	if (err) {
		warning("consort-line: couldn't start (%m)\n", err);
		consort_line_failed("the line couldn't start");
		goto out;
	}

	/* hello is the first line on standard output: commands and calls are taken only
	 * inside re_main. EPIPE: Consort closed its end before reading it, so 0 */
	err = consort_line_hello();
	if (err) {
		code = err == EPIPE ? 0 : 1;
		goto out;
	}

	err = re_main(NULL);
	code = err ? 1 : consort_line_code();

 out:
	ua_stop_all(true);
	ua_close();
	conf_close();
	baresip_close();
	mod_close();
	libre_close();

	return code;
}
