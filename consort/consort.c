/**
 * @file consort.c  Consort's line: baresip's calls, carried to Consort a connection a call
 *
 * The control channel is the program's standard input and output, which only Consort holds:
 * the secret and the settings first (read for main.c), then commands one way and events the
 * other, one JSON object a line. Each call's audio goes through its link (link.c) once the
 * call is answered. PROTOCOL.md has the whole contract.
 *
 * Copyright (C) 2026 The consort-line authors. BSD-3-Clause, see LICENSE.
 */
#include <winsock2.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <stdlib.h>
#include <string.h>
#include "proto.h"
#include "proof.h"
#include "link.h"
#include "wake.h"
#include "consort.h"

enum {
	LINE_MAX      = 4096,   /**< A control line's bytes, at most         */
	FIRST_WAIT    = 10000,  /**< The first two lines within 10 s (ms)    */
	ID_BYTES      = 8,      /**< A call's own id: 8 random bytes, as hex */
	ID_MAX        = 2 * ID_BYTES,
	TEXT_MAX      = 256,    /**< A far end's text in an event, at most   */
	URI_MAX       = 512,
	KEYS_A_SECOND = 10,
};

/**
 * A call, by an id of the add-on's own: never baresip's call_id, which for an incoming call
 * is the far end's Call-ID, chosen by whoever calls (two calls could share one).
 */
struct ccall {
	struct le le;
	struct call *call;       /**< baresip's, until it closes               */
	char id[ID_MAX + 1];
	struct cl_link *link;
	const char *why;         /**< Why the add-on hung it up, if it did     */
	uint64_t keys[KEYS_A_SECOND];  /**< When the last keys went on (ms)   */
	unsigned nkeys;
	bool incoming;
	bool named;              /**< Its source and player are its own        */
	bool accepted;
	bool answered;           /**< The answered event was written          */
};

struct ausrc_st {
	struct ccall *cc;
	ausrc_read_h *rh;
	void *arg;
	uint64_t ts;
};

struct auplay_st {
	struct ccall *cc;
	auplay_write_h *wh;
	void *arg;
};

/** What other threads post for the main thread: kept in memory, never sent over a socket */
enum post_kind { POST_LINE, POST_TOO_LONG, POST_EOF, POST_ENDED };

struct post {
	struct post *next;
	enum post_kind kind;
	char id[ID_MAX + 1];
	enum cl_end why;
	char *line;
};

static struct {
	uint8_t secret[CL_SECRET_BYTES];
	char listen[32];
	char account[URI_MAX + 1];
	uint32_t calls;

	struct list calls_list;
	struct ausrc *ausrc;
	struct auplay *auplay;
	struct tmr hello;
	struct log log;
	struct cl_wake *wake;

	SRWLOCK lock;            /**< Guards the posts                        */
	struct post *posts, *last;

	HANDLE reader;
	HANDLE got;              /**< Set when a first line came, or input ended */
	HANDLE ready;            /**< Set once posts can be taken             */
	char *lines[2];          /**< The first two lines, for main.c         */
	volatile LONG nlines;
	volatile LONG input_ended;

	struct ccall *dialed;    /**< The call the dial in progress made      */
	bool quitting;
	bool broken;             /**< Standard output can't be written        */
} g;


/*
 * Standard output: the add-on's lines only
 */

static int print_handler(const char *p, size_t size, void *arg)
{
	return mbuf_write_mem(arg, (const uint8_t *)p, size);
}


static void quit_all(const char *why);


static void write_line(const struct odict *od)
{
	struct mbuf *mb = mbuf_alloc(256);
	struct re_printf pf = {print_handler, mb};
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD done = 0;

	if (!mb || g.broken)
		goto out;

	if (json_encode_odict(&pf, od) || mbuf_write_u8(mb, '\n'))
		goto out;

	if (!WriteFile(out, mb->buf, (DWORD)mb->end, &done, NULL) || done != mb->end) {
		g.broken = true;
		quit_all("Consort's end of the control channel is gone");
	}

 out:
	mem_deref(mb);
}


/** A far end's text, made safe for an event: printable ASCII, '?' for anything else, capped */
static void ascii(char *out, size_t size, const char *in)
{
	size_t n = 0;

	for (; in && *in && n + 1 < size && n < TEXT_MAX; in++) {
		const unsigned char c = (unsigned char)*in;
		out[n++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
	}
	out[n] = '\0';
}


static void event_str(const char *event, const char *k1, const char *v1,
		      const char *k2, const char *v2, const char *k3, const char *v3)
{
	struct odict *od = NULL;
	char a[TEXT_MAX + 1], b[TEXT_MAX + 1], c[TEXT_MAX + 1];

	if (odict_alloc(&od, 8))
		return;

	(void)odict_entry_add(od, "event", ODICT_STRING, event);
	if (k1 && v1) {
		ascii(a, sizeof(a), v1);
		(void)odict_entry_add(od, k1, ODICT_STRING, a);
	}
	if (k2 && v2) {
		ascii(b, sizeof(b), v2);
		(void)odict_entry_add(od, k2, ODICT_STRING, b);
	}
	if (k3 && v3) {
		ascii(c, sizeof(c), v3);
		(void)odict_entry_add(od, k3, ODICT_STRING, c);
	}

	write_line(od);
	mem_deref(od);
}


void consort_line_failed(const char *why)
{
	event_str("failed", "why", why, NULL, NULL, NULL, NULL);
}


static void reply(uint64_t id, bool ok, const char *call, const char *why)
{
	struct odict *od = NULL;

	if (odict_alloc(&od, 8))
		return;

	(void)odict_entry_add(od, "event", ODICT_STRING, "reply");
	(void)odict_entry_add(od, "id", ODICT_INT, (int64_t)id);
	(void)odict_entry_add(od, "ok", ODICT_BOOL, ok);
	if (call)
		(void)odict_entry_add(od, "call", ODICT_STRING, call);
	if (why)
		(void)odict_entry_add(od, "why", ODICT_STRING, why);

	write_line(od);
	mem_deref(od);
}


/*
 * baresip's log goes to standard error, never standard output. It can name numbers and
 * keys, so Consort keeps it in memory only.
 */

static void log_handler(uint32_t level, const char *msg)
{
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	DWORD done;
	(void)level;

	if (msg)
		(void)WriteFile(err, msg, (DWORD)strlen(msg), &done, NULL);
}


/*
 * Posts from other threads
 */

static void post(enum post_kind kind, const char *id, enum cl_end why, char *line)
{
	struct post *p = calloc(1, sizeof(*p));

	if (!p) {
		free(line);
		return;
	}

	p->kind = kind;
	p->why = why;
	p->line = line;
	if (id)
		memcpy(p->id, id, sizeof(p->id));

	AcquireSRWLockExclusive(&g.lock);
	if (g.last)
		g.last->next = p;
	else
		g.posts = p;
	g.last = p;
	ReleaseSRWLockExclusive(&g.lock);

	cl_wake_post(g.wake);
}


static struct post *take_posts(void)
{
	struct post *p;

	AcquireSRWLockExclusive(&g.lock);
	p = g.posts;
	g.posts = g.last = NULL;
	ReleaseSRWLockExclusive(&g.lock);

	return p;
}


/*
 * Calls
 */

static void ccall_destructor(void *arg)
{
	struct ccall *cc = arg;

	list_unlink(&cc->le);
	cl_link_free(cc->link);
}


static struct ccall *find_call(const struct call *call)
{
	struct le *le;

	for (le = list_head(&g.calls_list); le; le = le->next) {
		struct ccall *cc = le->data;
		if (cc->call == call)
			return cc;
	}

	return NULL;
}


static struct ccall *find_id(const char *id)
{
	struct le *le;

	if (!id)
		return NULL;

	for (le = list_head(&g.calls_list); le; le = le->next) {
		struct ccall *cc = le->data;
		if (0 == strcmp(cc->id, id))
			return cc;
	}

	return NULL;
}


/** A call id's form: 16 lowercase hex digits */
static bool good_id(const char *id)
{
	if (str_len(id) != ID_MAX)
		return false;

	for (size_t i = 0; i < ID_MAX; i++) {
		const char c = id[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}

	return true;
}


/** A link ended its call's audio: on the link's thread, so it posts to the main thread */
static void link_ended(enum cl_end why, void *arg)
{
	const struct ccall *cc = arg;

	/* cc->id is written once, before the link starts, and the ccall outlives its link */
	post(POST_ENDED, cc->id, why, NULL);
}


static struct ccall *ccall_new(struct call *call, bool incoming)
{
	struct ccall *cc;
	uint8_t raw[ID_BYTES];

	cc = mem_zalloc(sizeof(*cc), ccall_destructor);
	if (!cc)
		return NULL;

	cc->call = call;
	cc->incoming = incoming;

	do {
		if (cl_random(raw, sizeof(raw))) {
			mem_deref(cc);
			return NULL;
		}
		cl_hex(cc->id, raw, sizeof(raw));
	} while (find_id(cc->id));

	if (cl_link_alloc(&cc->link, g.secret, link_ended, cc)) {
		mem_deref(cc);
		return NULL;
	}

	list_append(&g.calls_list, &cc->le, cc);

	return cc;
}


/** The call's source and player get its own name, before its audio starts */
static bool name_devices(struct ccall *cc)
{
	struct audio *a = call_audio(cc->call);

	if (cc->named)
		return true;
	if (!a || audio_set_devicename(a, cc->id, cc->id))
		return false;

	cc->named = true;

	return true;
}


static void hang_up(struct ccall *cc, const char *why)
{
	if (!cc->call)
		return;
	if (!cc->why)
		cc->why = why;

	/* ua_hangup raises CALL_CLOSED, which writes closed */
	ua_hangup(call_get_ua(cc->call), cc->call, 0, NULL);
}


static const char *why_words(enum cl_end why)
{
	switch (why) {

	case CL_END_HANGUP:     return "Consort hung up";
	case CL_END_CLOSED:     return "Consort's connection closed";
	case CL_END_NO_CONSORT: return "Consort didn't connect";
	case CL_END_RULE:       return "a frame of Consort's broke a rule";
	case CL_END_NOT_TAKEN:  return "Consort didn't take the line's frames";
	case CL_END_FAILED:     return "the add-on failed";
	default:                return "the call ended";
	}
}


static void closed(struct ccall *cc, const char *why)
{
	struct cl_link_stats st;
	struct odict *od = NULL, *fig = NULL;
	char text[TEXT_MAX + 1];

	cl_link_hangup(cc->link);
	cl_link_stats(cc->link, &st);

	/* The add-on's own words when it hung up; else baresip's, made safe */
	ascii(text, sizeof(text), cc->why ? cc->why : (why ? why : "the call ended"));

	if (0 == odict_alloc(&od, 8) && 0 == odict_alloc(&fig, 16)) {
		(void)odict_entry_add(fig, "ticks", ODICT_INT, (int64_t)st.ticks);
		(void)odict_entry_add(fig, "late", ODICT_INT, (int64_t)st.late);
		(void)odict_entry_add(fig, "latest_us", ODICT_INT, (int64_t)st.latest_us);
		(void)odict_entry_add(fig, "restarts", ODICT_INT, (int64_t)st.restarts);
		(void)odict_entry_add(fig, "played", ODICT_INT, (int64_t)st.played);
		(void)odict_entry_add(fig, "dry", ODICT_INT, (int64_t)st.dry);
		(void)odict_entry_add(fig, "most_queued", ODICT_INT, (int64_t)st.most_queued);
		(void)odict_entry_add(fig, "sent", ODICT_INT, (int64_t)st.sent);
		(void)odict_entry_add(fig, "refused", ODICT_INT, (int64_t)st.refused);

		(void)odict_entry_add(od, "event", ODICT_STRING, "closed");
		(void)odict_entry_add(od, "call", ODICT_STRING, cc->id);
		(void)odict_entry_add(od, "why", ODICT_STRING, text);
		(void)odict_entry_add(od, "figures", ODICT_OBJECT, fig);
		write_line(od);
	}

	mem_deref(fig);
	mem_deref(od);

	cc->call = NULL;
	list_unlink(&cc->le);
	mem_deref(cc);
}


static void answered(struct ccall *cc)
{
	uint8_t nonce[CL_NONCE_BYTES];
	char hex[2 * CL_NONCE_BYTES + 1];
	struct odict *od = NULL;
	uint16_t p;

	if (cc->answered)
		return;

	if (!cc->named) {
		hang_up(cc, "the call's audio isn't its own");
		return;
	}

	if (cl_link_answer(cc->link, &p, nonce)) {
		warning("consort: a call's listener couldn't be bound: hung up\n");
		hang_up(cc, "the call's listener couldn't be bound");
		return;
	}

	cc->answered = true;
	cl_hex(hex, nonce, sizeof(nonce));

	if (0 == odict_alloc(&od, 8)) {
		(void)odict_entry_add(od, "event", ODICT_STRING, "answered");
		(void)odict_entry_add(od, "call", ODICT_STRING, cc->id);
		(void)odict_entry_add(od, "port", ODICT_INT, (int64_t)p);
		(void)odict_entry_add(od, "nonce", ODICT_STRING, hex);
		write_line(od);
	}
	mem_deref(od);
}


/** At most ten keys a second go on; one past that is dropped, unless it is a 9 (an opt-out) */
static void key(struct ccall *cc, char k)
{
	const uint64_t now = tmr_jiffies();
	const unsigned oldest = cc->nkeys % KEYS_A_SECOND;

	if (cc->nkeys >= KEYS_A_SECOND && now - cc->keys[oldest] < 1000 && k != '9')
		return;

	cc->keys[oldest] = now;
	++cc->nkeys;
	cl_link_key(cc->link, k);
}


static void event_handler(enum bevent_ev ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);
	struct ccall *cc;
	(void)arg;

	if (!call)
		return;

	cc = find_call(call);

	switch (ev) {

	case BEVENT_CALL_INCOMING:
		/* Its streams exist already (call_accept makes them): name its devices now */
		if (cc)
			break;
		cc = ccall_new(call, true);
		if (!cc) {
			ua_hangup(call_get_ua(call), call, 500, "Server Internal Error");
			break;
		}
		if (!name_devices(cc)) {
			hang_up(cc, "the call's audio couldn't be made its own");
			break;
		}
		event_str("incoming", "call", cc->id, "from", call_peeruri(call),
			  "to", account_aor(ua_account(call_get_ua(call))));
		break;

	case BEVENT_CALL_OUTGOING:
		/* Raised inside ua_connect, before its streams exist: named at its offer */
		if (!cc)
			cc = ccall_new(call, false);
		g.dialed = cc;
		break;

	case BEVENT_CALL_LOCAL_SDP:
		/* The offer comes right after the streams are made, before any audio */
		if (cc && !cc->incoming && !name_devices(cc))
			hang_up(cc, "the call's audio couldn't be made its own");
		break;

	case BEVENT_CALL_RINGING:
		if (cc && !cc->incoming && !cc->answered)
			event_str("ringing", "call", cc->id, NULL, NULL, NULL, NULL);
		break;

	case BEVENT_CALL_PROGRESS:
		if (cc && !cc->incoming && !cc->answered)
			event_str("progress", "call", cc->id, NULL, NULL, NULL, NULL);
		break;

	case BEVENT_CALL_ESTABLISHED:
		if (!cc)
			break;
		if (cc->incoming && !cc->accepted) {
			/* Only Consort answers a call that rings */
			hang_up(cc, "answered without Consort");
			break;
		}
		answered(cc);
		break;

	case BEVENT_CALL_DTMF_START: {
		const char *k = bevent_get_text(event);

		if (cc && cc->answered && k && str_len(k) == 1 && cl_is_key(k[0]))
			key(cc, k[0]);
		break;
	}

	case BEVENT_CALL_CLOSED:
		if (cc)
			closed(cc, bevent_get_text(event));
		break;

	default:
		break;
	}
}


/*
 * The audio source and player named "consort": each a known call's, by its device name
 */

static void src_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	if (st->cc) {
		cl_link_source(st->cc->link, NULL, NULL);
		mem_deref(st->cc);
	}
}


/** On the link's thread: a frame of Consort's (or silence) to baresip */
static void src_feed(const int16_t samples[CL_SAMPLES], void *arg)
{
	struct ausrc_st *st = arg;
	int16_t frame[CL_SAMPLES];
	struct auframe af;

	memcpy(frame, samples, sizeof(frame));
	auframe_init(&af, AUFMT_S16LE, frame, CL_SAMPLES, CL_RATE, 1);
	af.timestamp = st->ts;
	st->ts += (uint64_t)CL_SAMPLES * AUDIO_TIMEBASE / CL_RATE;

	st->rh(&af, st->arg);
}


static int src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *device,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ccall *cc = find_id(device);
	struct ausrc_st *st;
	(void)as;
	(void)errh;

	if (!stp || !prm || !rh)
		return EINVAL;
	if (!cc) {
		warning("consort: a source for no known call: refused\n");
		return EINVAL;
	}
	if (prm->srate != CL_RATE || prm->ch != 1 || prm->ptime != 20 ||
	    prm->fmt != AUFMT_S16LE) {
		warning("consort: a source not at 8000 Hz, one channel, 20 ms: refused\n");
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), src_destructor);
	if (!st)
		return ENOMEM;

	st->cc = mem_ref(cc);
	st->rh = rh;
	st->arg = arg;
	cl_link_source(cc->link, src_feed, st);

	*stp = st;

	return 0;
}


static void play_destructor(void *arg)
{
	struct auplay_st *st = arg;

	if (st->cc) {
		cl_link_player(st->cc->link, NULL, NULL);
		mem_deref(st->cc);
	}
}


/** On the link's thread: the caller's next 20 ms from baresip (silence if it has none) */
static void play_pull(int16_t samples[CL_SAMPLES], void *arg)
{
	struct auplay_st *st = arg;
	struct auframe af;

	memset(samples, 0, CL_SAMPLES * sizeof(int16_t));
	auframe_init(&af, AUFMT_S16LE, samples, CL_SAMPLES, CL_RATE, 1);

	st->wh(&af, st->arg);
}


static int play_alloc(struct auplay_st **stp, const struct auplay *ap,
		      struct auplay_prm *prm, const char *device,
		      auplay_write_h *wh, void *arg)
{
	struct ccall *cc = find_id(device);
	struct auplay_st *st;
	(void)ap;

	if (!stp || !prm || !wh)
		return EINVAL;
	if (!cc) {
		warning("consort: a player for no known call: refused\n");
		return EINVAL;
	}
	if (prm->srate != CL_RATE || prm->ch != 1 || prm->ptime != 20 ||
	    prm->fmt != AUFMT_S16LE) {
		warning("consort: a player not at 8000 Hz, one channel, 20 ms: refused\n");
		return ENOTSUP;
	}

	st = mem_zalloc(sizeof(*st), play_destructor);
	if (!st)
		return ENOMEM;

	st->cc = mem_ref(cc);
	st->wh = wh;
	st->arg = arg;
	cl_link_player(cc->link, play_pull, st);

	*stp = st;

	return 0;
}


/*
 * Commands, from standard input on the reader thread, run on the main thread
 */

static bool only_keys(const struct odict *od, const char *const *keys, size_t n)
{
	struct le *le;

	for (le = list_head(&od->lst); le; le = le->next) {
		const struct odict_entry *e = le->data;
		bool known = false;

		for (size_t i = 0; i < n; i++)
			known = known || 0 == strcmp(odict_entry_key(e), keys[i]);
		if (!known)
			return false;
	}

	return true;
}


static void quit_all(const char *why)
{
	struct le *le;

	if (g.quitting)
		return;
	g.quitting = true;

	warning("consort: quitting: %s\n", why);

	le = list_head(&g.calls_list);
	while (le) {
		struct ccall *cc = le->data;
		le = le->next;
		hang_up(cc, why);
	}

	re_cancel();
}


static void broken(const char *why)
{
	consort_line_failed(why);
	quit_all(why);
}


/** A dial's address: sip: and printable ASCII, no spaces, no <>"; and nothing past URI_MAX */
static bool good_uri(const char *to)
{
	const size_t n = str_len(to);

	if (n < 5 || n > URI_MAX || 0 != strncmp(to, "sip:", 4))
		return false;

	for (size_t i = 0; i < n; i++) {
		const unsigned char c = (unsigned char)to[i];
		if (c <= 0x20 || c > 0x7e || c == '<' || c == '>' || c == '"' || c == ';')
			return false;
	}

	return true;
}


static void command(const char *line, size_t len)
{
	static const char *const dial_keys[] = {"id", "do", "to"};
	static const char *const call_keys[] = {"id", "do", "call"};
	static const char *const quit_keys[] = {"id", "do"};
	struct odict *od = NULL;
	const struct odict_entry *e;
	const char *what;
	uint64_t id = 0;
	int err;

	err = json_decode_odict(&od, 8, line, len, 2);
	if (err) {
		broken("a command that isn't JSON");
		goto out;
	}

	e = odict_lookup(od, "id");
	what = odict_string(od, "do");
	if (!e || odict_entry_type(e) != ODICT_INT || odict_entry_int(e) < 1 || !what) {
		broken("a command without its id or do");
		goto out;
	}
	id = (uint64_t)odict_entry_int(e);

	if (0 == strcmp(what, "dial")) {
		const char *to = odict_string(od, "to");
		struct le *le = list_head(uag_list());
		struct call *call = NULL;
		struct ccall *cc;

		if (!only_keys(od, dial_keys, RE_ARRAY_SIZE(dial_keys)) || !good_uri(to)) {
			broken("a dial that isn't one");
			goto out;
		}
		if (!le) {
			reply(id, false, NULL, "the line has no account");
			goto out;
		}

		g.dialed = NULL;
		err = ua_connect(le->data, &call, NULL, to, VIDMODE_OFF);
		cc = g.dialed;
		g.dialed = NULL;

		if (err || !call) {
			/* baresip freed the call without closing it: drop what was made for it */
			if (cc) {
				cc->call = NULL;
				list_unlink(&cc->le);
				mem_deref(cc);
			}
			reply(id, false, NULL, "the call couldn't be placed");
			goto out;
		}
		if (!cc || cc->call != call) {
			ua_hangup(call_get_ua(call), call, 0, NULL);
			reply(id, false, NULL, "the call couldn't be placed");
			goto out;
		}
		reply(id, true, cc->id, NULL);
	}
	else if (0 == strcmp(what, "accept") || 0 == strcmp(what, "hangup")) {
		const bool accept = what[0] == 'a';
		struct ccall *cc;

		if (!only_keys(od, call_keys, RE_ARRAY_SIZE(call_keys)) ||
		    !good_id(odict_string(od, "call"))) {
			broken("a command that isn't one");
			goto out;
		}

		cc = find_id(odict_string(od, "call"));
		if (!cc || !cc->call) {
			reply(id, false, NULL, "no such call");
			goto out;
		}

		if (accept) {
			if (!cc->incoming || cc->accepted) {
				reply(id, false, cc->id, "not a call that rang");
				goto out;
			}
			cc->accepted = true;
			err = ua_answer(call_get_ua(cc->call), cc->call, VIDMODE_OFF);
			reply(id, !err, cc->id, err ? "the call couldn't be answered" : NULL);
		}
		else {
			reply(id, true, cc->id, NULL);
			hang_up(cc, "Consort hung up");
		}
	}
	else if (0 == strcmp(what, "quit")) {
		if (!only_keys(od, quit_keys, RE_ARRAY_SIZE(quit_keys))) {
			broken("a quit that isn't one");
			goto out;
		}
		reply(id, true, NULL, NULL);
		quit_all("Consort said quit");
	}
	else {
		broken("a command the add-on doesn't know");
	}

 out:
	mem_deref(od);
}


static void posted(void *arg)
{
	struct post *p = take_posts();
	(void)arg;

	while (p) {
		struct post *next = p->next;

		switch (p->kind) {

		case POST_LINE:
			if (!g.quitting)
				command(p->line, str_len(p->line));
			break;

		case POST_TOO_LONG:
			broken("a control line past 4,096 bytes");
			break;

		case POST_EOF:
			quit_all("Consort's end of the control channel closed");
			break;

		case POST_ENDED: {
			struct ccall *cc = find_id(p->id);

			if (cc)
				hang_up(cc, why_words(p->why));
			break;
		}
		}

		free(p->line);
		free(p);
		p = next;
	}
}


/** The reader of standard input: the first two lines for main.c, the rest as posts */
static DWORD WINAPI reader(LPVOID arg)
{
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	char *line = malloc(LINE_MAX + 1);
	size_t have = 0;
	bool skip = false;
	(void)arg;

	if (!line)
		goto out;

	for (;;) {
		char c;
		DWORD n = 0;

		if (!ReadFile(in, &c, 1, &n, NULL) || n == 0)
			break;

		if (c == '\n') {
			if (skip) {
				skip = false;
				have = 0;
				continue;
			}
			if (have && line[have - 1] == '\r')
				--have;
			line[have] = '\0';

			if (g.nlines < 2) {
				g.lines[g.nlines] = line;
				InterlockedIncrement(&g.nlines);
				SetEvent(g.got);
				line = malloc(LINE_MAX + 1);
				if (!line)
					break;
				/* After the second, nothing until posts can be taken */
				if (g.nlines == 2 &&
				    WaitForSingleObject(g.ready, INFINITE) != WAIT_OBJECT_0)
					break;
			}
			else {
				char *copy = malloc(have + 1);
				if (copy) {
					memcpy(copy, line, have + 1);
					post(POST_LINE, NULL, CL_END_NONE, copy);
				}
			}
			have = 0;
			continue;
		}

		if (skip)
			continue;

		if (have >= LINE_MAX) {
			if (g.nlines < 2)
				break;
			skip = true;
			post(POST_TOO_LONG, NULL, CL_END_NONE, NULL);
			continue;
		}
		line[have++] = c;
	}

 out:
	if (line) {
		cl_wipe(line, LINE_MAX + 1);
		free(line);
	}

	InterlockedExchange(&g.input_ended, 1);
	SetEvent(g.got);
	if (g.nlines >= 2)
		post(POST_EOF, NULL, CL_END_NONE, NULL);

	return 0;
}


/** Waits for the n-th first line (1 or 2): it, or NULL when it didn't come in time */
static const char *first_line(LONG n)
{
	const DWORD start = GetTickCount();

	for (;;) {
		DWORD spent;

		ResetEvent(g.got);
		if (g.nlines >= n)
			return g.lines[n - 1];
		if (g.input_ended)
			return NULL;
		spent = GetTickCount() - start;
		if (spent >= FIRST_WAIT)
			return NULL;
		(void)WaitForSingleObject(g.got, FIRST_WAIT - spent);
	}
}


static void wipe_line(unsigned n)
{
	if (g.lines[n]) {
		cl_wipe(g.lines[n], LINE_MAX + 1);
		free(g.lines[n]);
		g.lines[n] = NULL;
	}
}


/** {"line": 1, "secret": "<64 hex>"}, nothing else */
static int take_secret(const char *line)
{
	static const char *const keys[] = {"line", "secret"};
	struct odict *od = NULL;
	const struct odict_entry *v, *e;
	const char *hex;
	int err = EINVAL;

	if (!line || json_decode_odict(&od, 4, line, str_len(line), 2))
		goto out;

	v = odict_lookup(od, "line");
	hex = odict_string(od, "secret");
	if (!only_keys(od, keys, RE_ARRAY_SIZE(keys)) || !v ||
	    odict_entry_type(v) != ODICT_INT || odict_entry_int(v) != 1 || !hex)
		goto out;

	if (cl_unhex(g.secret, CL_SECRET_BYTES, hex, str_len(hex)))
		err = 0;

 out:
	/* The decoded copy of the secret's text is in od: wipe it before it is freed */
	e = od ? odict_lookup(od, "secret") : NULL;
	if (e && odict_entry_type(e) == ODICT_STRING) {
		char *s = odict_entry_str(e);
		cl_wipe(s, str_len(s));
	}
	mem_deref(od);

	return err;
}


/** 127.0.0.1:<port>, the port from 0 to 65535 */
static bool good_listen(const char *s)
{
	static const char prefix[] = "127.0.0.1:";
	const size_t n = str_len(s), p = sizeof(prefix) - 1;
	unsigned long port = 0;

	if (n <= p || n > p + 5 || 0 != strncmp(s, prefix, p))
		return false;
	for (size_t i = p; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		port = port * 10 + (unsigned long)(s[i] - '0');
	}

	return port <= 65535;
}


/** An account's address: sip:<user>@127.0.0.1, the user plain letters, digits, . _ - */
static bool good_account(const char *s)
{
	static const char host[] = "@127.0.0.1";
	const size_t n = str_len(s), h = sizeof(host) - 1;

	if (n < 4 + 1 + h || n > 64 || 0 != strncmp(s, "sip:", 4) ||
	    0 != strcmp(s + n - h, host))
		return false;

	for (size_t i = 4; i < n - h; i++) {
		const char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return false;
	}

	return true;
}


/** {"listen": "127.0.0.1:<port>", "account": "sip:<user>@127.0.0.1", "calls": n} */
static int take_settings(const char *line)
{
	static const char *const keys[] = {"listen", "account", "calls"};
	struct odict *od = NULL;
	const struct odict_entry *calls;
	const char *listen, *account;
	int err = EINVAL;

	if (!line || json_decode_odict(&od, 4, line, str_len(line), 2))
		goto out;

	listen = odict_string(od, "listen");
	account = odict_string(od, "account");
	calls = odict_lookup(od, "calls");
	if (!only_keys(od, keys, RE_ARRAY_SIZE(keys)) || !good_listen(listen) ||
	    !good_account(account) || !calls || odict_entry_type(calls) != ODICT_INT ||
	    odict_entry_int(calls) < 1 || odict_entry_int(calls) > 16)
		goto out;

	str_ncpy(g.listen, listen, sizeof(g.listen));
	str_ncpy(g.account, account, sizeof(g.account));
	g.calls = (uint32_t)odict_entry_int(calls);
	err = 0;

 out:
	mem_deref(od);

	return err;
}


int consort_line_open(void)
{
	int err;

	/* Standard output is the add-on's from the first byte: baresip's log goes elsewhere */
	log_enable_stdout(false);
	log_level_set(LEVEL_WARN);
	g.log.h = log_handler;
	log_register_handler(&g.log);

	InitializeSRWLock(&g.lock);
	g.got = CreateEventW(NULL, TRUE, FALSE, NULL);
	g.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g.got || !g.ready)
		return ENOMEM;

	g.reader = CreateThread(NULL, 0, reader, NULL, 0, NULL);
	if (!g.reader)
		return ENOMEM;

	err = take_secret(first_line(1));
	wipe_line(0);
	if (err) {
		consort_line_failed("secret");
		return err;
	}

	err = take_settings(first_line(2));
	wipe_line(1);
	if (err) {
		consort_line_failed("settings");
		return err;
	}

	return 0;
}


const char *consort_line_listen(void)
{
	return g.listen;
}


const char *consort_line_account(void)
{
	return g.account;
}


uint32_t consort_line_calls(void)
{
	return g.calls;
}


static void hello(void *arg)
{
	struct sa laddr;
	char sip[64] = "";
	struct odict *od = NULL;
	(void)arg;

	if (0 == sip_transp_laddr(uag_sip(), &laddr, SIP_TRANSP_UDP, NULL))
		(void)re_snprintf(sip, sizeof(sip), "%J", &laddr);

	if (odict_alloc(&od, 8))
		return;

	(void)odict_entry_add(od, "event", ODICT_STRING, "hello");
	(void)odict_entry_add(od, "version", ODICT_STRING, CONSORT_LINE_VERSION);
	(void)odict_entry_add(od, "baresip", ODICT_STRING, baresip_version());
	(void)odict_entry_add(od, "sip", ODICT_STRING, sip);
	write_line(od);
	mem_deref(od);
}


static int module_init(void)
{
	int err;

	err = cl_wake_alloc(&g.wake, posted, NULL);
	if (err)
		return err;

	err  = ausrc_register(&g.ausrc, baresip_ausrcl(), "consort", src_alloc);
	err |= auplay_register(&g.auplay, baresip_auplayl(), "consort", play_alloc);
	err |= bevent_register(event_handler, NULL);
	if (err)
		return err;

	/* Lines past the first two may be taken now */
	SetEvent(g.ready);
	tmr_start(&g.hello, 0, hello, NULL);

	return 0;
}


static int module_close(void)
{
	bool reader_gone = true;

	tmr_cancel(&g.hello);
	bevent_unregister(event_handler);

	/* Every link's thread stops here, so none can post again */
	list_flush(&g.calls_list);

	g.ausrc = mem_deref(g.ausrc);
	g.auplay = mem_deref(g.auplay);

	/* The reader may be waiting in ReadFile: stop it before what it posts to goes */
	if (g.reader) {
		(void)CancelSynchronousIo(g.reader);
		SetEvent(g.ready);
		reader_gone = WaitForSingleObject(g.reader, 1000) == WAIT_OBJECT_0;
		if (reader_gone) {
			CloseHandle(g.reader);
			g.reader = NULL;
		}
	}
	if (reader_gone) {
		struct post *p = take_posts();

		while (p) {
			struct post *next = p->next;
			free(p->line);
			free(p);
			p = next;
		}
		cl_wake_free(g.wake);
		g.wake = NULL;
	}

	log_unregister_handler(&g.log);
	cl_wipe(g.secret, sizeof(g.secret));

	return 0;
}


const struct mod_export DECL_EXPORTS(consort) = {
	"consort",
	"application",
	module_init,
	module_close
};
