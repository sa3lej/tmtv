/*
 * tmate-recording.c — asciinema cast v2 session recording.
 *
 * Records terminal output to ~/.tmtv/recordings/<session>-<timestamp>.cast.
 * Compatible with `asciinema play` and `asciinema cat`.
 *
 * Cast v2 format:
 *   Line 1: JSON header {"version":2,"width":W,"height":H,...}
 *   Lines 2+: [elapsed, "o", "data"]  — output events
 *             [elapsed, "r", "CxR"]   — resize events
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "tmate.h"

static struct {
	FILE		*fp;
	char		*path;
	struct timespec	 start;
} recording;

static double
elapsed_seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - recording.start.tv_sec) +
	       (double)(now.tv_nsec - recording.start.tv_nsec) / 1e9;
}

/*
 * JSON-escape raw PTY data into buf.  Handles:
 *   - control characters (U+0000..U+001F) as \uXXXX
 *   - backslash and double-quote
 *   - valid UTF-8 sequences passed through
 *   - invalid bytes replaced with U+FFFD
 *
 * Returns number of bytes written (not counting NUL terminator).
 * buf must be at least 6*len + 1 bytes (worst case: every byte -> \uXXXX).
 */
static size_t
json_escape_pty(char *buf, size_t bufsz, const char *src, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	const unsigned char *p = (const unsigned char *)src;
	const unsigned char *end = p + len;
	char *out = buf;
	char *out_end = buf + bufsz - 1;	/* leave room for NUL */

	while (p < end && out < out_end) {
		unsigned char c = *p;

		if (c == '"' || c == '\\') {
			if (out + 2 > out_end)
				break;
			*out++ = '\\';
			*out++ = (char)c;
			p++;
		} else if (c < 0x20) {
			/* Control character -> \uXXXX */
			if (out + 6 > out_end)
				break;
			*out++ = '\\';
			*out++ = 'u';
			*out++ = '0';
			*out++ = '0';
			*out++ = hex[c >> 4];
			*out++ = hex[c & 0x0f];
			p++;
		} else if (c < 0x80) {
			/* Plain ASCII */
			*out++ = (char)c;
			p++;
		} else {
			/*
			 * UTF-8 multi-byte.  Validate the sequence and
			 * pass it through, or emit U+FFFD for bad bytes.
			 */
			int expect;
			unsigned int codepoint;

			if ((c & 0xe0) == 0xc0) {
				expect = 2;
				codepoint = c & 0x1f;
			} else if ((c & 0xf0) == 0xe0) {
				expect = 3;
				codepoint = c & 0x0f;
			} else if ((c & 0xf8) == 0xf0) {
				expect = 4;
				codepoint = c & 0x07;
			} else {
				/* Invalid lead byte -> U+FFFD */
				goto emit_replacement;
			}

			if (p + expect > end)
				goto emit_replacement;

			int ok = 1;
			for (int i = 1; i < expect; i++) {
				if ((p[i] & 0xc0) != 0x80) {
					ok = 0;
					break;
				}
				codepoint = (codepoint << 6) | (p[i] & 0x3f);
			}

			/* Reject overlong encodings and surrogates */
			if (!ok ||
			    (expect == 2 && codepoint < 0x80) ||
			    (expect == 3 && codepoint < 0x800) ||
			    (expect == 4 && codepoint < 0x10000) ||
			    (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
			    codepoint > 0x10ffff)
				goto emit_replacement;

			/* Valid — copy the bytes through */
			if (out + expect > out_end)
				break;
			for (int i = 0; i < expect; i++)
				*out++ = (char)p[i];
			p += expect;
			continue;

		emit_replacement:
			/* \ufffd = 8 bytes */
			if (out + 8 > out_end)
				break;
			memcpy(out, "\\ufffd", 6);
			out += 6;
			p++;
		}
	}

	*out = '\0';
	return (size_t)(out - buf);
}

/*
 * Ensure ~/.tmtv/recordings/ exists.  Returns the directory path
 * (caller must free) or NULL on failure.
 */
static char *
recordings_dir(void)
{
	const char *home;
	char *tmtv_dir, *rec_dir;

	home = find_home();
	if (home == NULL)
		return (NULL);

	xasprintf(&tmtv_dir, "%s/.tmtv", home);
	if (mkdir(tmtv_dir, 0700) != 0 && errno != EEXIST) {
		tmate_debug("mkdir %s: %s", tmtv_dir, strerror(errno));
		free(tmtv_dir);
		return (NULL);
	}

	xasprintf(&rec_dir, "%s/recordings", tmtv_dir);
	if (mkdir(rec_dir, 0700) != 0 && errno != EEXIST) {
		tmate_debug("mkdir %s: %s", rec_dir, strerror(errno));
		free(rec_dir);
		free(tmtv_dir);
		return (NULL);
	}

	free(tmtv_dir);
	return (rec_dir);
}

void
tmtv_recording_start(const char *token, u_int width, u_int height)
{
	char *dir, *safe_token;
	time_t now;

	if (recording.fp != NULL) {
		tmate_debug("recording already active: %s", recording.path);
		return;
	}

	dir = recordings_dir();
	if (dir == NULL) {
		tmate_status_message("Recording failed: cannot create directory");
		return;
	}

	/* Use token for filename, but sanitize: replace / with _ */
	safe_token = xstrdup(token && *token ? token : "session");
	for (char *cp = safe_token; *cp; cp++) {
		if (*cp == '/')
			*cp = '_';
	}

	now = time(NULL);
	xasprintf(&recording.path, "%s/%s-%lld.cast",
		  dir, safe_token, (long long)now);
	free(safe_token);
	free(dir);

	recording.fp = fopen(recording.path, "w");
	if (recording.fp == NULL) {
		tmate_status_message("Recording failed: %s", strerror(errno));
		free(recording.path);
		recording.path = NULL;
		return;
	}

	clock_gettime(CLOCK_MONOTONIC, &recording.start);

	/* Cast v2 header — single JSON line */
	fprintf(recording.fp,
		"{\"version\":2,\"width\":%u,\"height\":%u,"
		"\"timestamp\":%lld,"
		"\"title\":\"%s\","
		"\"env\":{\"TERM\":\"xterm-256color\"}}\n",
		width, height, (long long)now, token ? token : "session");
	fflush(recording.fp);

	tmate_status_message("Recording to %s", recording.path);
	tmate_info("Recording started: %s", recording.path);
}

void
tmtv_recording_write(const char *buf, size_t len)
{
	char *escaped;
	size_t esc_len;
	size_t bufsz;

	if (recording.fp == NULL || len == 0)
		return;

	bufsz = len * 6 + 1;
	escaped = xmalloc(bufsz);
	esc_len = json_escape_pty(escaped, bufsz, buf, len);

	fprintf(recording.fp, "[%.6f, \"o\", \"%.*s\"]\n",
		elapsed_seconds(), (int)esc_len, escaped);

	free(escaped);

	/* Periodic flush — every write for now; could batch later */
	fflush(recording.fp);
}

void
tmtv_recording_resize(u_int width, u_int height)
{
	if (recording.fp == NULL)
		return;

	fprintf(recording.fp, "[%.6f, \"r\", \"%ux%u\"]\n",
		elapsed_seconds(), width, height);
	fflush(recording.fp);
}

void
tmtv_recording_stop(void)
{
	if (recording.fp == NULL)
		return;

	fflush(recording.fp);
	fclose(recording.fp);
	recording.fp = NULL;

	tmate_status_message("Recording saved: %s", recording.path);
	tmate_info("Recording stopped: %s", recording.path);

	free(recording.path);
	recording.path = NULL;
}

int
tmtv_recording_active(void)
{
	return (recording.fp != NULL);
}
