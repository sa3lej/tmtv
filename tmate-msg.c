#include <time.h>
#include "tmate.h"

static void tmate_status_message_client(struct client *c, const char *message)
{
	int delay;

	delay = options_get_number(c->session ? c->session->options :
	    global_s_options, "tmate-display-time");

	status_message_set(c, delay, 0, 0, 0, "[tmate] %s", message);
}

static void tmate_status_message_session(const char *message)
{
	struct session *s;
	struct window_pane *wp;
	struct window_mode_entry *wme;

	if (tmate_foreground)
		return;

	s = RB_MIN(sessions, &sessions);
	if (!s) {
		cfg_add_cause("%s", message);
		return;
	}

	wp = s->curw->window->active;
	wme = TAILQ_FIRST(&wp->modes);
	if (wme != NULL && wme->mode == &window_copy_mode)
		window_copy_add(wp, "%s", message);
}

void __tmate_status_message(const char *fmt, va_list ap)
{
	struct client *c;
	char *message;

	xvasprintf(&message, fmt, ap);
	tmate_info("%s", message);

	TAILQ_FOREACH(c, &clients, entry) {
		if (c && !(c->flags & CLIENT_READONLY))
			tmate_status_message_client(c, message);
	}

	tmate_status_message_session(message);

	free(message);
}

void tmate_status_message(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__tmate_status_message(fmt, ap);
	va_end(ap);
}
