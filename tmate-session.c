#ifdef HAVE_LIBBSD
#include <bsd/libutil.h>
#endif
#include <event2/dns.h>
#include <event2/util.h>
#include <event2/event.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "tmate.h"

#define TMATE_DNS_RETRY_TIMEOUT 2
#define TMATE_RECONNECT_RETRY_TIMEOUT 2

struct tmate_session tmate_session;
int tmate_foreground;

static void lookup_and_connect(void);

static void on_dns_retry(__unused evutil_socket_t fd, __unused short what,
			 void *arg)
{
	struct tmate_session *session = arg;

	assert(session->ev_dns_retry);
	event_free(session->ev_dns_retry);
	session->ev_dns_retry = NULL;

	lookup_and_connect();
}

static void dns_cb(int errcode, struct evutil_addrinfo *addr, void *ptr)
{
	struct evutil_addrinfo *ai;
	const char *host = ptr;

	evdns_base_free(tmate_session.ev_dnsbase, 0);
	tmate_session.ev_dnsbase = NULL;

	if (errcode) {
		struct tmate_session *session = &tmate_session;

		if (session->ev_dns_retry)
			return;

		struct timeval tv = { .tv_sec = TMATE_DNS_RETRY_TIMEOUT, .tv_usec = 0 };

		session->ev_dns_retry = evtimer_new(session->ev_base, on_dns_retry, session);
		if (!session->ev_dns_retry)
			tmate_fatal("out of memory");
		evtimer_add(session->ev_dns_retry, &tv);

		tmate_status_message("%s lookup failure. Retrying in %d seconds (%s)",
				     host, TMATE_DNS_RETRY_TIMEOUT,
				     evutil_gai_strerror(errcode));
		return;
	}

	cfg_add_cause("Connecting to %s...", host);
	tmate_status_message("Connecting to %s...", host);

	int i, num_clients = 0;
	for (ai = addr; ai; ai = ai->ai_next)
		num_clients++;

	struct tmate_ssh_client *ssh_clients[num_clients];

	for (ai = addr, i = 0; ai; ai = ai->ai_next, i++) {
		char buf[128];
		const char *ip = NULL;
		if (ai->ai_family == AF_INET) {
			struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
			ip = evutil_inet_ntop(AF_INET, &sin->sin_addr, buf, 128);
		} else if (ai->ai_family == AF_INET6) {
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ai->ai_addr;
			ip = evutil_inet_ntop(AF_INET6, &sin6->sin6_addr, buf, 128);
		}

		ssh_clients[i] = tmate_ssh_client_alloc(&tmate_session, ip);
	}

	for (i = 0; i < num_clients; i++)
		connect_ssh_client(ssh_clients[i]);

	evutil_freeaddrinfo(addr);
}

static void lookup_and_connect(void)
{
	struct evutil_addrinfo hints;
	const char *tmate_server_host;

	tmate_server_host = options_get_string(global_options,
					       "tmtv-server-host");
	if (!strlen(tmate_server_host)) {
		tmate_debug("No tmtv-server-host configured, running in local-only mode");
		return;
	}

	assert(!tmate_session.ev_dnsbase);
	tmate_session.ev_dnsbase = evdns_base_new(tmate_session.ev_base, 1);
	if (!tmate_session.ev_dnsbase)
		tmate_fatal("Cannot initialize the DNS lookup service");

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	tmate_debug("Looking up %s...", tmate_server_host);
	(void)evdns_getaddrinfo(tmate_session.ev_dnsbase, tmate_server_host, NULL,
				&hints, dns_cb, (void *)tmate_server_host);
}

static void __tmate_session_init(struct tmate_session *session,
				 struct event_base *base)
{
	memset(session, 0, sizeof(*session));

	session->ev_base = base;

	/*
	 * Early initialization of encoder because we need to parse
	 * config files to get the server configs, but while we are parsing
	 * config files, we need to buffer bind commands and all for the
	 * slave.
	 * Decoder is setup later.
	 */
	tmate_encoder_init(&session->encoder, NULL, &tmate_session);

	session->min_sx = -1;
	session->min_sy = -1;

	TAILQ_INIT(&session->clients);
	TAILQ_INIT(&session->env);

	/* Create input socket early so TMTV_INPUT_SOCKET is in the env */
	tmtv_input_socket_create();
	tmtv_input_socket_start(base);
}

void tmate_session_init(struct event_base *base)
{
	__tmate_session_init(&tmate_session, base);
	atexit(tmtv_input_socket_cleanup);
	tmate_write_header(&tmate_session);
}

static void send_authorized_keys(void)
{
	const char *opt_path;
	char *path;

	opt_path = options_get_string(global_options, "tmtv-authorized-keys");
	if (strlen(opt_path) == 0)
		return;

	path = xstrdup(opt_path);
	tmate_info("Using %s for access control", path);

	FILE *f;
	char *line;
	size_t len;

	if (path[0] == '~' && path[1] == '/') {
		const char *home = find_home();
		if (home) {
			char *new_path;
			xasprintf(&new_path, "%s%s", home, &path[1]);
			free(path);
			path = new_path;
		}
	}

	if ((f = fopen(path, "r")) == NULL) {
		cfg_add_cause("%s: %s", path, strerror(errno));
		free(path);
		return;
	}

	while ((line = fparseln(f, &len, NULL, NULL, 0)) != NULL) {
		if (len == 0)
			continue;
		tmate_set_val(&tmate_session, "authorized_keys", line);
		free(line);
	}

	if (ferror(f))
		cfg_add_cause("%s: %s", path, strerror(errno));

	fclose(f);
	free(path);
}

void tmate_session_start(void)
{
	/*
	 * We split init and start because:
	 * - We need to process the tmux config file during the connection as
	 *   we are setting up the tmate identity.
	 * - While we are parsing the config file, we need to be able to
	 *   serialize it, and so we need a worker encoder.
	 */
	if (tmate_foreground) {
		tmate_set_val(&tmate_session, "foreground", "true");
		tmate_info("To connect to the session locally, run: tmtv -S %s attach", socket_path);
	} else {
		cfg_add_cause("%s", "Tip: if you wish to use tmtv only for remote access, run: tmtv -F");
		cfg_add_cause("%s", "To see the following messages again, run: tmtv show-messages");
		cfg_add_cause("%s", "Press <q> or <ctrl-c> to continue");
		cfg_add_cause("%s", "---------------------------------------------------------------------");
	}

	/* Send named session request if configured */
	{
		const char *sname = options_get_string(global_options,
						      "tmtv-session-name");
		if (sname && *sname)
			tmate_set_val(&tmate_session, "session_name", sname);
	}

	/* Send session password if configured */
	{
		const char *pw = options_get_string(global_options,
						    "tmtv-session-password");
		if (pw != NULL && *pw != '\0')
			tmate_set_val(&tmate_session, "session_password", pw);
	}

	/* Send link TTL if configured */
	{
		const char *ttl = options_get_string(global_options,
						     "tmtv-link-ttl");
		if (ttl != NULL && *ttl != '\0')
			tmate_set_val(&tmate_session, "link_ttl", ttl);
	}

	/* Sync prefix key to server so web/SSH viewers use the right key */
	{
		key_code prefix = options_get_number(global_s_options,
						     "prefix");
		if (prefix != ('b' | KEYC_CTRL)) {
			char buf[32];
			snprintf(buf, sizeof(buf), "0x%llx",
				 (unsigned long long)prefix);
			tmate_set_val(&tmate_session, "prefix", buf);
		}
		key_code prefix2 = options_get_number(global_s_options,
						      "prefix2");
		if (prefix2 != KEYC_NONE) {
			char buf[32];
			snprintf(buf, sizeof(buf), "0x%llx",
				 (unsigned long long)prefix2);
			tmate_set_val(&tmate_session, "prefix2", buf);
		}
	}

	send_authorized_keys();
	tmate_write_uname(&tmate_session);
	tmate_write_ready(&tmate_session);
	lookup_and_connect();

	/* Start periodic status timer for detached sessions */
	tmate_start_status_timer(&tmate_session);
}

static void on_reconnect_retry(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	struct tmate_session *session = arg;

	assert(session->ev_connection_retry);
	event_free(session->ev_connection_retry);
	session->ev_connection_retry = NULL;

	if (session->last_server_ip) {
		/*
		 * We have a previous server ip. Let's try that again first,
		 * but then connect to any server if it fails again.
		 */
		struct tmate_ssh_client *c = tmate_ssh_client_alloc(session,
						session->last_server_ip);
		connect_ssh_client(c);
		free(session->last_server_ip);
		session->last_server_ip = NULL;
	} else {
		lookup_and_connect();
	}
}

void tmate_reconnect_session(struct tmate_session *session, const char *message)
{
	/*
	 * We no longer have an SSH connection. Time to reconnect.
	 * We'll reuse some of the session information if we can,
	 * and we'll try to reconnect to the same server if possible,
	 * to avoid an SSH connection string change.
	 */
	struct timeval tv = { .tv_sec = TMATE_RECONNECT_RETRY_TIMEOUT, .tv_usec = 0 };

	if (session->ev_connection_retry)
		return;

	session->ev_connection_retry = evtimer_new(session->ev_base, on_reconnect_retry, session);
	if (!session->ev_connection_retry)
		tmate_fatal("out of memory");
	evtimer_add(session->ev_connection_retry, &tv);

	if (message && !tmate_foreground)
		tmate_status_message("Reconnecting... (%s)", message);
	else
		tmate_status_message("Reconnecting...");

	/*
	 * This says that we'll need to send a snapshot of the current state.
	 */
	session->reconnected = true;
}
