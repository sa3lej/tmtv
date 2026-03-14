#ifndef TMATE_H
#define TMATE_H

#include <sys/types.h>
#include <msgpack.h>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <event.h>

#include "tmux.h"

#define tmate_debug(...) log_debug(__VA_ARGS__)
#define tmate_info(...)  log_debug(__VA_ARGS__)
#define tmate_fatal(...) fatalx(__VA_ARGS__)

/* Global foreground mode flag (defined in tmux.c) */
extern int tmate_foreground;

/* tmate-msgpack.c */

typedef void tmate_encoder_write_cb(void *userdata, struct evbuffer *buffer);

struct tmate_encoder {
	msgpack_packer pk;
	tmate_encoder_write_cb *ready_callback;
	void *userdata;
	struct evbuffer *buffer;
	struct event *ev_buffer;
	bool ev_active;
	struct event *ev_retry;	/* backpressure retry timer */
};

extern void tmate_encoder_init(struct tmate_encoder *encoder,
			       tmate_encoder_write_cb *callback,
			       void *userdata);
extern void tmate_encoder_destroy(struct tmate_encoder *encoder);
extern void tmate_encoder_set_ready_callback(struct tmate_encoder *encoder,
					     tmate_encoder_write_cb *callback,
					     void *userdata);
extern void tmate_encoder_schedule_retry(struct tmate_encoder *encoder);

extern void msgpack_pack_string(msgpack_packer *pk, const char *str);
extern void msgpack_pack_boolean(msgpack_packer *pk, bool value);

#define _pack(enc, what, ...) msgpack_pack_##what(&(enc)->pk, ##__VA_ARGS__)

struct tmate_unpacker;
struct tmate_decoder;
typedef void tmate_decoder_reader(void *userdata, struct tmate_unpacker *uk);

struct tmate_decoder {
	struct msgpack_unpacker unpacker;
	tmate_decoder_reader *reader;
	void *userdata;
};

extern void tmate_decoder_init(struct tmate_decoder *decoder, tmate_decoder_reader *reader, void *userdata);
extern void tmate_decoder_destroy(struct tmate_decoder *decoder);
extern void tmate_decoder_get_buffer(struct tmate_decoder *decoder, char **buf, size_t *len);
extern void tmate_decoder_commit(struct tmate_decoder *decoder, size_t len);

struct tmate_unpacker {
	int argc;
	msgpack_object *argv;
};

extern void init_unpacker(struct tmate_unpacker *uk, msgpack_object obj);
extern void tmate_decoder_error(void);
extern int64_t unpack_int(struct tmate_unpacker *uk);
extern bool unpack_bool(struct tmate_unpacker *uk);
extern void unpack_buffer(struct tmate_unpacker *uk, const char **buf, size_t *len);
extern char *unpack_string(struct tmate_unpacker *uk);
extern void unpack_array(struct tmate_unpacker *uk, struct tmate_unpacker *nested);

#define unpack_each(nested_uk, tmp_uk, uk)						\
	for (unpack_array(uk, tmp_uk);							\
	     (tmp_uk)->argc > 0 && (init_unpacker(nested_uk, (tmp_uk)->argv[0]), 1);	\
	     (tmp_uk)->argv++, (tmp_uk)->argc--)

/* tmate-encoder.c */

#define TMATE_PROTOCOL_VERSION 6

struct tmate_session;

extern void tmate_write_header(struct tmate_session *session);
extern void tmate_write_uname(struct tmate_session *session);
extern void tmate_write_ready(struct tmate_session *session);
extern void tmate_sync_layout(struct tmate_session *session,
			      struct session *s);
extern void tmate_pty_data(struct tmate_session *session,
			   struct window_pane *wp, const char *buf, size_t len);
extern int tmate_should_replicate_cmd(const struct cmd_entry *cmd);
extern void tmate_set_val(struct tmate_session *session,
			  const char *name, const char *value);
extern void tmate_exec_cmd_args(struct tmate_session *session,
				int argc, const char **argv);
extern void tmate_exec_cmd(struct tmate_session *session, struct cmd *cmd);
extern void tmate_failed_cmd(struct tmate_session *session,
			     int client_id, const char *cause);
extern void tmate_status(struct tmate_session *session,
			 const char *left, const char *right);
extern void tmate_sync_copy_mode(struct tmate_session *session,
				 struct window_pane *wp);
extern void tmate_write_copy_mode(struct tmate_session *session,
				  struct window_pane *wp, const char *str);
extern void tmate_write_fin(struct tmate_session *session);
extern void tmate_send_reconnection_state(struct tmate_session *session);
extern void tmate_expand_status(void);
extern void tmate_start_status_timer(struct tmate_session *session);

/* tmate-decoder.c */

struct tmate_session;
extern void tmate_dispatch_slave_message(struct tmate_session *session,
					 struct tmate_unpacker *uk);
extern struct session *tmate_find_session(struct tmate_session *ts);

/* tmate-ssh-client.c */

enum tmate_ssh_client_state_types {
	SSH_NONE,
	SSH_INIT,
	SSH_CONNECT,
	SSH_AUTH_SERVER,
	SSH_AUTH_CLIENT_NONE,
	SSH_AUTH_CLIENT_PUBKEY,
	SSH_NEW_CHANNEL,
	SSH_OPEN_CHANNEL,
	SSH_BOOTSTRAP,
	SSH_READY,
};

struct tmate_ssh_client {
	/* XXX The "session" word is used for three things:
	 * - the ssh session
	 * - the tmate sesssion
	 * - the tmux session
	 * A tmux session is associated 1:1 with a tmate session.
	 * An ssh session belongs to a tmate session, and a tmate session
	 * has one ssh session, except during bootstrapping where
	 * there is one ssh session per tmate server, and the first one wins.
	 */
	struct tmate_session *tmate_session;
	TAILQ_ENTRY(tmate_ssh_client) node;

	char *server_ip;

	int state;

	/*
	 * ssh_callbacks is allocated because the libssh API sucks (userdata
	 * has to be in the struct itself).
	 */
	struct ssh_callbacks_struct ssh_callbacks;
	char *tried_passphrase;
	ssh_session session;
	ssh_channel channel;

	struct event *ev_ssh;
};
TAILQ_HEAD(tmate_ssh_clients, tmate_ssh_client);

extern void connect_ssh_client(struct tmate_ssh_client *client);
extern struct tmate_ssh_client *tmate_ssh_client_alloc(struct tmate_session *session,
						       const char *server_ip);

/* tmate-env.c */

struct tmate_env {
	TAILQ_ENTRY(tmate_env) entry;
	char *name;
	char *value;
};
TAILQ_HEAD(tmate_env_list, tmate_env);

/* tmate-session.c */

struct tmate_session {
	struct event_base *ev_base;
	struct evdns_base *ev_dnsbase;
	struct event *ev_dns_retry;

	struct tmate_encoder encoder;
	struct tmate_decoder decoder;

	/* True when the slave has sent all the environment variables */
	int tmate_env_ready;

	/* Per-session tmate environment variables (format variables) */
	struct tmate_env_list env;

	int min_sx;
	int min_sy;

	/*
	 * This list contains one connection per IP. The first connected
	 * client wins, and saved in *client. When we have a winner, the
	 * losers are disconnected and killed.
	 */
	struct tmate_ssh_clients clients;
	int need_passphrase;
	char *passphrase;

	bool reconnected;
	struct event *ev_connection_retry;
	struct event *ev_status_timer;
	char *last_server_ip;
	char *reconnection_data;
	/*
	 * When we reconnect, instead of serializing the key bindings and
	 * options, we replay all the tmux commands we replicated.
	 * It may be a little innacurate to replicate the state, but
	 * it's much easier.
	 */
	struct {
		unsigned int capacity;
		unsigned int tail;
		struct {
			int argc;
			char **argv;
		} *cmds;
	} saved_tmux_cmds;
};

extern struct tmate_session tmate_session;
extern void tmate_session_init(struct event_base *base);
extern void tmate_session_start(void);
extern void tmate_reconnect_session(struct tmate_session *session, const char *message);

/* tmate-recording.c */
extern void tmtv_recording_start(const char *token, u_int width, u_int height);
extern void tmtv_recording_write(const char *buf, size_t len);
extern void tmtv_recording_resize(u_int width, u_int height);
extern void tmtv_recording_stop(void);
extern int  tmtv_recording_active(void);

/* tmate-input-socket.c */
extern void tmtv_input_socket_create(void);
extern void tmtv_input_socket_start(struct event_base *base);
extern void tmtv_input_socket_cleanup(void);
extern void tmtv_input_on_user_join(int user_id, const char *name,
				     bool readonly, const char *type);
extern void tmtv_input_on_user_leave(int user_id);
extern void tmtv_input_on_user_input(int user_id, int pane_id, key_code key);
extern void tmtv_input_send_mode(bool enabled, bool mirror);
extern bool tmtv_input_has_subscribers(void);

/* tmate-debug.c */
extern void tmate_print_stack_trace(void);
extern void tmate_catch_sigsegv(void);
extern void tmate_preload_trace_lib(void);

/* tmate-msg.c */

extern void __tmate_status_message(const char *fmt, va_list ap);
extern void printflike(1, 2) tmate_status_message(const char *fmt, ...);

/* tmate-env.c */

extern void tmate_set_env(struct tmate_session *session,
			   const char *name, const char *value);
extern void tmate_format(struct tmate_session *session,
			  struct format_tree *ft);

#ifdef TMATE_SERVER_BUILD
/* tmate-auth-keys.c (server build only) */
extern bool tmate_allow_auth(const char *pubkey);
extern bool tmate_check_session_password(const char *password);
extern bool tmate_has_session_password(void);
/* tmate-daemon-encoder.c: forward viewer input to tmate client */
extern void tmate_client_pane_key(int pane_id, key_code key);
extern void tmate_client_cmd(int client_id, struct cmd *cmd);
extern int tmate_should_exec_cmd_locally(const struct cmd_entry *cmd);
#endif

#endif
