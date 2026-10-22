/* $OpenBSD: hyperlinks.c,v 1.6 2026/09/28 10:42:01 nicm Exp $ */

/*
 * Copyright (c) 2021 Will <author@will.party>
 * Copyright (c) 2022 Jeff Chiang <pobomp@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "tmate-snapshot.h"
#include <limits.h>

/*
 * OSC 8 hyperlinks, described at:
 *
 *     https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
 *
 * Each hyperlink and ID combination is assigned a number ("inner" in this
 * file) which is stored in an extended grid cell and maps into a tree here.
 *
 * Each URI has one inner number and one external ID (which tmux uses to send
 * the hyperlink to the terminal) and one internal ID (which is received from
 * the sending application inside tmux).
 *
 * Anonymous hyperlinks are each unique and are not reused even if they have
 * the same URI (terminals will not want to tie them together).
 */

#define MAX_HYPERLINKS 5000
#define MAX_HYPERLINK_URI 1024

static long long hyperlinks_next_external_id = 1;
static u_int global_hyperlinks_count;

struct hyperlinks_uri {
	struct hyperlinks	*tree;

	u_int			 inner;
	const char		*internal_id;
	const char		*external_id;
	const char		*uri;

	TAILQ_ENTRY(hyperlinks_uri) list_entry;
	RB_ENTRY(hyperlinks_uri)    by_inner_entry;
	RB_ENTRY(hyperlinks_uri)    by_uri_entry; /* by internal ID and URI */
};
RB_HEAD(hyperlinks_by_uri_tree, hyperlinks_uri);
RB_HEAD(hyperlinks_by_inner_tree, hyperlinks_uri);

TAILQ_HEAD(hyperlinks_list, hyperlinks_uri);
static struct hyperlinks_list global_hyperlinks =
    TAILQ_HEAD_INITIALIZER(global_hyperlinks);

struct hyperlinks {
	u_int				next_inner;
	struct hyperlinks_by_inner_tree	by_inner;
	struct hyperlinks_by_uri_tree	by_uri;
	u_int				references;
	int				snapshot_detached;
};

static int
hyperlinks_by_uri_cmp(struct hyperlinks_uri *left, struct hyperlinks_uri *right)
{
	int	r;

	if (*left->internal_id == '\0' || *right->internal_id == '\0') {
		/*
		 * If both URIs are anonymous, use the inner for comparison so
		 * that they do not match even if the URI is the same - each
		 * anonymous URI should be unique.
		 */
		if (*left->internal_id != '\0')
			return (-1);
		if (*right->internal_id != '\0')
			return (1);
		if (left->inner < right->inner)
			return (-1);
		if (left->inner > right->inner)
			return (1);
		return (0);
	}

	r = strcmp(left->internal_id, right->internal_id);
	if (r != 0)
		return (r);
	return (strcmp(left->uri, right->uri));
}
RB_PROTOTYPE_STATIC(hyperlinks_by_uri_tree, hyperlinks_uri, by_uri_entry,
    hyperlinks_by_uri_cmp);
RB_GENERATE_STATIC(hyperlinks_by_uri_tree, hyperlinks_uri, by_uri_entry,
    hyperlinks_by_uri_cmp);

static int
hyperlinks_by_inner_cmp(struct hyperlinks_uri *left,
    struct hyperlinks_uri *right)
{
	if (left->inner < right->inner)
		return (-1);
	if (left->inner > right->inner)
		return (1);
	return (0);
}
RB_PROTOTYPE_STATIC(hyperlinks_by_inner_tree, hyperlinks_uri, by_inner_entry,
    hyperlinks_by_inner_cmp);
RB_GENERATE_STATIC(hyperlinks_by_inner_tree, hyperlinks_uri, by_inner_entry,
    hyperlinks_by_inner_cmp);

/* Remove a hyperlink. */
static void
hyperlinks_remove(struct hyperlinks_uri *hlu)
{
	struct hyperlinks	*hl = hlu->tree;

	if (!hl->snapshot_detached) {
		TAILQ_REMOVE(&global_hyperlinks, hlu, list_entry);
		global_hyperlinks_count--;
	}

	RB_REMOVE(hyperlinks_by_inner_tree, &hl->by_inner, hlu);
	RB_REMOVE(hyperlinks_by_uri_tree, &hl->by_uri, hlu);

	free((void *)hlu->internal_id);
	free((void *)hlu->external_id);
	free((void *)hlu->uri);
	free(hlu);
}

/* Store a new hyperlink or return if it already exists. */
u_int
hyperlinks_put(struct hyperlinks *hl, const char *uri_in,
    const char *internal_id_in)
{
	struct hyperlinks_uri	 find, *hlu;
	char			*uri, *internal_id, *external_id;

	/*
	 * Anonymous URI are stored with an empty internal ID and the tree
	 * comparator will make sure they never match each other (so each
	 * anonymous URI is unique).
	 */
	if (internal_id_in == NULL)
		internal_id_in = "";

	utf8_stravis(&uri, uri_in, VIS_OCTAL|VIS_CSTYLE);
	if (strlen(uri) > MAX_HYPERLINK_URI) {
		free(uri);
		return (0);
	}

	utf8_stravis(&internal_id, internal_id_in, VIS_OCTAL|VIS_CSTYLE);
	if (*internal_id != '\0') {
		find.uri = uri;
		find.internal_id = internal_id;

		hlu = RB_FIND(hyperlinks_by_uri_tree, &hl->by_uri, &find);
		if (hlu != NULL) {
			free (uri);
			free (internal_id);
			return (hlu->inner);
		}
	}
	xasprintf(&external_id, "tmux%llX", hyperlinks_next_external_id++);

	hlu = xcalloc(1, sizeof *hlu);
	hlu->inner = hl->next_inner++;
	hlu->internal_id = internal_id;
	hlu->external_id = external_id;
	hlu->uri = uri;
	hlu->tree = hl;
	RB_INSERT(hyperlinks_by_uri_tree, &hl->by_uri, hlu);
	RB_INSERT(hyperlinks_by_inner_tree, &hl->by_inner, hlu);

	TAILQ_INSERT_TAIL(&global_hyperlinks, hlu, list_entry);
	if (++global_hyperlinks_count == MAX_HYPERLINKS)
		hyperlinks_remove(TAILQ_FIRST(&global_hyperlinks));

	return (hlu->inner);
}

/* Get hyperlink by inner number. */
int
hyperlinks_get(struct hyperlinks *hl, u_int inner, const char **uri_out,
    const char **internal_id_out, const char **external_id_out)
{
	struct hyperlinks_uri	find, *hlu;

	find.inner = inner;

	hlu = RB_FIND(hyperlinks_by_inner_tree, &hl->by_inner, &find);
	if (hlu == NULL)
		return (0);
	if (internal_id_out != NULL)
		*internal_id_out = hlu->internal_id;
	if (external_id_out != NULL)
		*external_id_out = hlu->external_id;
	*uri_out = hlu->uri;
	return (1);
}

/* Initialize hyperlink set. */
struct hyperlinks *
hyperlinks_init(void)
{
	struct hyperlinks	*hl;

	hl = xcalloc(1, sizeof *hl);
	hl->next_inner = 1;
	RB_INIT(&hl->by_uri);
	RB_INIT(&hl->by_inner);
	hl->references = 1;
	return (hl);
}

/* Copy hyperlink set. */
struct hyperlinks *
hyperlinks_copy(struct hyperlinks *hl)
{
	hl->references++;
	return (hl);
}

/* Free all hyperlinks but not the set itself. */
void
hyperlinks_reset(struct hyperlinks *hl)
{
	struct hyperlinks_uri	*hlu, *hlu1;

	RB_FOREACH_SAFE(hlu, hyperlinks_by_inner_tree, &hl->by_inner, hlu1)
		hyperlinks_remove(hlu);
}

/* Free hyperlink set. */
void
hyperlinks_free(struct hyperlinks *hl)
{
	if (--hl->references == 0) {
		hyperlinks_reset(hl);
		free(hl);
	}
}

/* These indices are scoped by the serialized screen table, not receiver handles.
 * Plans stay off the global eviction list until a complete frame is committed. */
static int
hyperlink_bin(msgpack_object o, u_int max)
{
	return (o.type == MSGPACK_OBJECT_BIN && o.via.bin.size <= max &&
	    (o.via.bin.size == 0 || tmtv_snapshot_text((const u_char *)o.via.bin.ptr, o.via.bin.size)));
}

static int
hyperlink_snapshot_cmp(const void *left, const void *right)
{
	const msgpack_object *a = *(const msgpack_object *const *)left;
	const msgpack_object *b = *(const msgpack_object *const *)right;
	u_int i, n;
	int result;
	for (i = 1; i <= 2; i++) {
		n = a[i].via.bin.size < b[i].via.bin.size ? a[i].via.bin.size : b[i].via.bin.size;
		result = memcmp(a[i].via.bin.ptr, b[i].via.bin.ptr, n);
		if (result != 0) return (result);
		if (a[i].via.bin.size < b[i].via.bin.size) return (-1);
		if (a[i].via.bin.size > b[i].via.bin.size) return (1);
	}
	return (0);
}

int
hyperlinks_snapshot_validate(msgpack_object o)
{
	msgpack_object *v, *named[TMTV_SNAPSHOT_MAX_LINKS];
	u_int i, n = 0, last = 0;
	if (o.type != MSGPACK_OBJECT_ARRAY || o.via.array.size > TMTV_SNAPSHOT_MAX_LINKS ||
	    !tmtv_snapshot_object_bounded(o)) return (0);
	for (i = 0; i < o.via.array.size; i++) {
		if (o.via.array.ptr[i].type != MSGPACK_OBJECT_ARRAY || o.via.array.ptr[i].via.array.size != 3) return (0);
		v = o.via.array.ptr[i].via.array.ptr;
		if (v[0].type != MSGPACK_OBJECT_POSITIVE_INTEGER || v[0].via.u64 <= last ||
		    v[0].via.u64 > INT_MAX || !hyperlink_bin(v[1], MAX_HYPERLINK_URI) ||
		    !hyperlink_bin(v[2], TMTV_SNAPSHOT_MAX_PARSER_BYTES)) return (0);
		last = v[0].via.u64;
		if (v[2].via.bin.size) named[n++] = v;
	}
	/* Named URI/ID pairs are unique. Sorting bounds work to N log N even
	 * for a full hostile table with long, almost-identical IDs. */
	qsort(named, n, sizeof *named, hyperlink_snapshot_cmp);
	for (i = 1; i < n; i++)
		if (hyperlink_snapshot_cmp(&named[i-1], &named[i]) == 0) return (0);
	return (1);
}

/* Caller validates the table once; reference lookup is logarithmic per cell. */
int
hyperlinks_snapshot_has(msgpack_object table, u_int id)
{
	u_int lo = 0, hi = table.via.array.size, mid;
	uint64_t found;
	if (!id) return (1);
	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		found = table.via.array.ptr[mid].via.array.ptr[0].via.u64;
		if (found == id) return (1);
		if (found < id) lo = mid + 1; else hi = mid;
	}
	return (0);
}

int
hyperlinks_snapshot_encode(struct hyperlinks *hl, msgpack_packer *pk)
{
	struct hyperlinks_uri *hlu;
	u_int n = 0;
	if (hl != NULL) RB_FOREACH(hlu, hyperlinks_by_inner_tree, &hl->by_inner) n++;
	if (n > TMTV_SNAPSHOT_MAX_LINKS || msgpack_pack_array(pk, n)) return (-1);
	if (hl == NULL) return (0);
	RB_FOREACH(hlu, hyperlinks_by_inner_tree, &hl->by_inner) {
		if (hlu->inner > INT_MAX || strlen(hlu->uri) > MAX_HYPERLINK_URI ||
		    strlen(hlu->internal_id) > TMTV_SNAPSHOT_MAX_PARSER_BYTES ||
		    msgpack_pack_array(pk, 3) || msgpack_pack_unsigned_int(pk, hlu->inner) ||
		    msgpack_pack_bin(pk, strlen(hlu->uri)) || msgpack_pack_bin_body(pk, hlu->uri, strlen(hlu->uri)) ||
		    msgpack_pack_bin(pk, strlen(hlu->internal_id)) || msgpack_pack_bin_body(pk, hlu->internal_id, strlen(hlu->internal_id))) return (-1);
	}
	return (0);
}

struct hyperlinks *
hyperlinks_snapshot_decode(msgpack_object o)
{
	struct hyperlinks *hl;
	struct hyperlinks_uri *hlu;
	msgpack_object *v;
	u_int i;
	if (!hyperlinks_snapshot_validate(o)) return (NULL);
	hl = hyperlinks_init(); hl->snapshot_detached = 1;
	for (i = 0; i < o.via.array.size; i++) {
		v = o.via.array.ptr[i].via.array.ptr;
		hlu = xcalloc(1, sizeof *hlu); hlu->tree = hl;
		hlu->inner = v[0].via.u64;
		hlu->uri = xstrndup(v[1].via.bin.ptr, v[1].via.bin.size);
		hlu->internal_id = xstrndup(v[2].via.bin.ptr, v[2].via.bin.size);
		/* URI/ID are already normalized, not passed through stravis twice. */
		hlu->external_id = NULL;
		RB_INSERT(hyperlinks_by_inner_tree, &hl->by_inner, hlu);
		RB_INSERT(hyperlinks_by_uri_tree, &hl->by_uri, hlu);
		hl->next_inner = hlu->inner + 1;
	}
	return (hl);
}

void
hyperlinks_snapshot_activate(struct hyperlinks *hl)
{
	struct hyperlinks_uri *hlu;
	char *id;
	if (hl == NULL || !hl->snapshot_detached) return;
	hl->snapshot_detached = 0;
	RB_FOREACH(hlu, hyperlinks_by_inner_tree, &hl->by_inner) {
		xasprintf(&id, "tmux%llX", hyperlinks_next_external_id++);
		hlu->external_id = id;
		TAILQ_INSERT_TAIL(&global_hyperlinks, hlu, list_entry);
		if (++global_hyperlinks_count >= MAX_HYPERLINKS)
			hyperlinks_remove(TAILQ_FIRST(&global_hyperlinks));
	}
}
