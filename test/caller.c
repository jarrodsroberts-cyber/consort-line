/**
 * @file caller.c  The tests' far end: a plain baresip from a config file and an account
 *
 * Usage: consort-line-caller CONFIG_FILE ACCOUNT
 *
 * Built only for the tests and never released. It loads whatever its config names (aufile,
 * aubridge and echo, menu, ctrl_tcp bound to 127.0.0.1), as baresip itself would; the line
 * (main.c) loads only g711 and the add-on. baresip's own program can't serve here: built with
 * MSVC it takes no options, so every copy would share one config folder.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#include <winsock2.h>
#include <re.h>
#include <baresip.h>
#include <stdio.h>
#include <stdlib.h>

enum { CONFIG_MAX = 65536 };


int main(int argc, char *argv[])
{
	static uint8_t conf[CONFIG_MAX];
	struct ua *ua = NULL;
	size_t n;
	FILE *f;
	int err;

	if (argc != 3) {
		fprintf(stderr, "usage: consort-line-caller CONFIG_FILE ACCOUNT\n");
		return 2;
	}

	f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "consort-line-caller: can't read %s\n", argv[1]);
		return 2;
	}
	n = fread(conf, 1, sizeof(conf), f);
	fclose(f);
	if (!n || n == sizeof(conf)) {
		fprintf(stderr, "consort-line-caller: %s is empty or too long\n", argv[1]);
		return 2;
	}

	err = libre_init();
	if (!err)
		err = conf_configure_buf(conf, n);
	if (!err)
		err = baresip_init(conf_config());
	if (!err)
		err = ua_init("consort-line-caller", true, false, false);
	if (!err)
		err = conf_modules();
	if (!err)
		err = ua_alloc(&ua, argv[2]);
	if (!err)
		err = re_main(NULL);

	ua_stop_all(true);
	ua_close();
	module_app_unload();
	conf_close();
	baresip_close();
	mod_close();
	libre_close();

	return err ? 1 : 0;
}
