#include "tmate.h"
#include "tmate-protocol.h"

static void on_encoder_buffer_ready(__unused evutil_socket_t fd,
				    __unused short what, void *arg)
{
	struct tmate_encoder *encoder = arg;

	encoder->ev_active = false;
	if (encoder->ready_callback)
		encoder->ready_callback(encoder->userdata, encoder->buffer);
}

static void on_encoder_retry_timer(__unused evutil_socket_t fd,
				   __unused short what, void *arg)
{
	struct tmate_encoder *encoder = arg;

	if (encoder->ready_callback && evbuffer_get_length(encoder->buffer))
		encoder->ready_callback(encoder->userdata, encoder->buffer);
}

/*
 * Encoder buffer caps.  Two limits:
 *
 * DISCONNECTED_MAX (16 MB): When no SSH connection is draining the buffer
 * (ready_callback is NULL), PTY data accumulates here and must be capped.
 *
 * CONNECTED_MAX (64 MB): When the SSH channel has backpressure (slow link,
 * multiple Claude Code agents), the buffer grows while retrying.  This is a
 * safety cap — we prefer backpressure over data loss, but 64 MB means
 * something is seriously wrong.  Drain oldest data to survive.
 *
 * WATERMARK (32 MB): Log a warning so the user knows throughput is degraded.
 */
#define ENCODER_BUFFER_MAX        (16 * 1024 * 1024)
#define ENCODER_BUFFER_CONNECTED  (64 * 1024 * 1024)
#define ENCODER_BUFFER_WATERMARK  (32 * 1024 * 1024)

static int on_encoder_write(void *userdata, const char *buf, size_t len)
{
	struct tmate_encoder *encoder = userdata;

	if (evbuffer_add(encoder->buffer, buf, len) < 0)
		tmate_fatal("Cannot buffer encoded data");

	size_t buflen = evbuffer_get_length(encoder->buffer);

	if (!encoder->ready_callback) {
		/* Nothing draining — hard cap */
		if (buflen > ENCODER_BUFFER_MAX)
			evbuffer_drain(encoder->buffer,
			    buflen - ENCODER_BUFFER_MAX);
	} else {
		/* Connected but backpressured — softer cap */
		if (buflen > ENCODER_BUFFER_CONNECTED) {
			tmate_info("encoder buffer at %zuMB, draining excess",
			    buflen / (1024 * 1024));
			evbuffer_drain(encoder->buffer,
			    buflen - ENCODER_BUFFER_CONNECTED);
		} else if (buflen > ENCODER_BUFFER_WATERMARK) {
			tmate_debug("encoder buffer at %zuMB (backpressure)",
			    buflen / (1024 * 1024));
		}
	}

	/*
	 * Defer the flush via event_active().  DO NOT call
	 * ready_callback directly — on_encoder_write fires per
	 * msgpack field, not per complete message.  Immediate flush
	 * causes reentrancy on the server side (ssh_channel_write
	 * from within a channel-data callback lets libssh deliver
	 * more channel data, re-entering the encoder).
	 *
	 * event_active() runs before the next poll(), so the latency
	 * penalty is microseconds.
	 */
	if (!encoder->ev_active) {
		event_active(encoder->ev_buffer, EV_READ, 0);
		encoder->ev_active = true;
	}

	return 0;
}

/* Really sad hack, but we can get away with it */
#define tmate_encoder_from_pk(pk) ((struct tmate_encoder *)pk)

void msgpack_pack_string(msgpack_packer *pk, const char *str)
{
	size_t len = strlen(str);

	msgpack_pack_str(pk, len);
	msgpack_pack_str_body(pk, str, len);
}

void msgpack_pack_boolean(msgpack_packer *pk, bool value)
{
	if (value)
		msgpack_pack_true(pk);
	else
		msgpack_pack_false(pk);
}

void tmate_encoder_init(struct tmate_encoder *encoder,
			tmate_encoder_write_cb *callback,
			void *userdata)
{
	msgpack_packer_init(&encoder->pk, encoder, &on_encoder_write);
	encoder->buffer = evbuffer_new();
	encoder->ready_callback = callback;
	encoder->userdata = userdata;

	if (!encoder->buffer)
		tmate_fatal("Can't allocate buffer");

	encoder->ev_buffer = event_new(tmate_session.ev_base, -1,
		EV_READ | EV_PERSIST, on_encoder_buffer_ready, encoder);
	if (!encoder->ev_buffer)
		tmate_fatal("Can't allocate event");

	event_add(encoder->ev_buffer, NULL);

	encoder->ev_retry = evtimer_new(tmate_session.ev_base,
		on_encoder_retry_timer, encoder);
	if (!encoder->ev_retry)
		tmate_fatal("Can't allocate retry timer");

	encoder->ev_active = false;
}

void tmate_encoder_destroy(struct tmate_encoder *encoder)
{
	/* encoder->pk doesn't need any cleanup */
	evbuffer_free(encoder->buffer);
	event_del(encoder->ev_buffer);
	event_free(encoder->ev_buffer);
	if (encoder->ev_retry) {
		event_del(encoder->ev_retry);
		event_free(encoder->ev_retry);
	}
	memset(encoder, 0, sizeof(*encoder));
}

/*
 * Schedule a retry after SSH backpressure.  Uses a 25ms timer instead
 * of event_active() to avoid busy-looping when ssh_channel_write()
 * returns -1 because the channel window is full.
 */
void tmate_encoder_schedule_retry(struct tmate_encoder *encoder)
{
	struct timeval tv = { .tv_sec = 0, .tv_usec = 25000 };

	if (!evtimer_pending(encoder->ev_retry, NULL))
		evtimer_add(encoder->ev_retry, &tv);
}

void tmate_encoder_set_ready_callback(struct tmate_encoder *encoder,
				      tmate_encoder_write_cb *callback,
				      void *userdata)
{
	encoder->ready_callback = callback;
	encoder->userdata = userdata;
	if (encoder->ready_callback)
		encoder->ready_callback(encoder->userdata, encoder->buffer);
}

void tmate_decoder_error(void)
{
	/* TODO Don't kill the session, disconnect */
	tmate_print_stack_trace();
	tmate_fatal("Received a bad message");
}

void init_unpacker(struct tmate_unpacker *uk, msgpack_object obj)
{
	if (obj.type != MSGPACK_OBJECT_ARRAY)
		tmate_decoder_error();

	uk->argv = obj.via.array.ptr;
	uk->argc = obj.via.array.size;
}

int64_t unpack_int(struct tmate_unpacker *uk)
{
	int64_t val;

	if (uk->argc == 0)
		tmate_decoder_error();

	if (uk->argv[0].type != MSGPACK_OBJECT_POSITIVE_INTEGER &&
	    uk->argv[0].type != MSGPACK_OBJECT_NEGATIVE_INTEGER)
		tmate_decoder_error();

	val = uk->argv[0].via.i64;

	uk->argv++;
	uk->argc--;

	return val;
}

bool unpack_bool(struct tmate_unpacker *uk)
{
	bool val;

	if (uk->argc == 0)
		tmate_decoder_error();

	if (uk->argv[0].type != MSGPACK_OBJECT_BOOLEAN)
		tmate_decoder_error();

	val = uk->argv[0].via.boolean;

	uk->argv++;
	uk->argc--;

	return val;
}

void unpack_buffer(struct tmate_unpacker *uk, const char **buf, size_t *len)
{
	if (uk->argc == 0)
		tmate_decoder_error();

	if (uk->argv[0].type != MSGPACK_OBJECT_STR &&
	    uk->argv[0].type != MSGPACK_OBJECT_BIN)
		tmate_decoder_error();

	*len = uk->argv[0].via.str.size;
	*buf = uk->argv[0].via.str.ptr;

	uk->argv++;
	uk->argc--;
}

char *unpack_string(struct tmate_unpacker *uk)
{
	const char *buf;
	char *alloc_buf;
	size_t len;

	unpack_buffer(uk, &buf, &len);

	alloc_buf = xmalloc(len + 1);
	memcpy(alloc_buf, buf, len);
	alloc_buf[len] = '\0';

	return alloc_buf;
}

void unpack_array(struct tmate_unpacker *uk, struct tmate_unpacker *nested)
{
	if (uk->argc == 0)
		tmate_decoder_error();

	init_unpacker(nested, uk->argv[0]);

	uk->argv++;
	uk->argc--;
}

#define UNPACKER_RESERVE_SIZE 1024

void tmate_decoder_init(struct tmate_decoder *decoder, tmate_decoder_reader *reader,
			void *userdata)
{
	if (!msgpack_unpacker_init(&decoder->unpacker, UNPACKER_RESERVE_SIZE))
		tmate_fatal("Cannot initialize the unpacker");
	decoder->reader = reader;
	decoder->userdata = userdata;
}

void tmate_decoder_destroy(struct tmate_decoder *decoder)
{
	msgpack_unpacker_destroy(&decoder->unpacker);
	memset(decoder, 0, sizeof(*decoder));
}

void tmate_decoder_get_buffer(struct tmate_decoder *decoder,
			      char **buf, size_t *len)
{
	if (!msgpack_unpacker_reserve_buffer(&decoder->unpacker, UNPACKER_RESERVE_SIZE))
		tmate_fatal("cannot expand decoder buffer");

	*buf = msgpack_unpacker_buffer(&decoder->unpacker);
	*len = msgpack_unpacker_buffer_capacity(&decoder->unpacker);
}

void tmate_decoder_commit(struct tmate_decoder *decoder, size_t len)
{
	struct tmate_unpacker _uk, *uk = &_uk;
	msgpack_unpacked result;

	msgpack_unpacker_buffer_consumed(&decoder->unpacker, len);

	msgpack_unpacked_init(&result);
	while (msgpack_unpacker_next(&decoder->unpacker, &result)) {
		init_unpacker(uk, result.data);
		decoder->reader(decoder->userdata, uk);
	}
	msgpack_unpacked_destroy(&result);
}
