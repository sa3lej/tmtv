/* Exercise the actual portable implementations, not libc substitutes. */
#include <sys/socket.h>
#include <resolv.h>
#include <errno.h>
#include <unistd.h>
#include "tmux.h"
#include "test-harness.h"

/* Include system declarations before renaming the fallback symbols. */
#undef b64_ntop
#undef b64_pton
#define b64_ntop tmtv_compat_b64_ntop
#define b64_pton tmtv_compat_b64_pton
#define getline tmtv_compat_getline
#define getdelim tmtv_compat_getdelim
#define getpeereid tmtv_compat_getpeereid
#include "../compat/base64.c"
#include "../compat/getline.c"
#include "../compat/getpeereid.c"

TEST(base64_roundtrip_and_padding)
{
	const unsigned char data[] = {0, 0xff, 0x80, 'x'};
	unsigned char decoded[8];
	char encoded[16];
	size_t n;
	int len;

	for (n = 0; n <= sizeof data; n++) {
		len = b64_ntop(data, n, encoded, sizeof encoded);
		ASSERT_EQ(len, (int)(4 * ((n + 2) / 3)));
		ASSERT_EQ(b64_pton(encoded, decoded, sizeof decoded), (int)n);
		ASSERT_EQ(memcmp(data, decoded, n), 0);
	}
	ASSERT_EQ(b64_pton(" Zg==\n", decoded, sizeof decoded), 1);
	ASSERT_EQ(decoded[0], 'f');
}

TEST(base64_rejects_invalid_and_small_buffers)
{
	struct { unsigned char data[2]; unsigned char canary; } out;
	char encoded[4];

	ASSERT_EQ(b64_pton("!!!!", out.data, sizeof out.data), -1);
	ASSERT_EQ(b64_pton("Zg=", out.data, sizeof out.data), -1);
	ASSERT_EQ(b64_pton("Zh==", out.data, sizeof out.data), -1);
	out.canary = 0xa5;
	ASSERT_EQ(b64_pton("Zm9v", out.data, sizeof out.data), -1);
	ASSERT_EQ(out.canary, 0xa5);
	/* Four encoded bytes also need space for the terminating NUL. */
	ASSERT_EQ(b64_ntop((const unsigned char *)"foo", 3,
	    encoded, sizeof encoded), -1);
}

TEST(getline_grows_and_handles_final_line)
{
	FILE *fp = tmpfile();
	char *line = NULL;
	size_t size = 0, i, length = BUFSIZ * 3;

	ASSERT_NOT_NULL(fp);
	for (i = 0; i < length; i++)
		ASSERT_EQ(fputc('x', fp), 'x');
	ASSERT(fputs("\nlast", fp) >= 0);
	rewind(fp);
	ASSERT_EQ(getline(&line, &size, fp), (ssize_t)(length + 1));
	ASSERT(size > length + 1);
	for (i = 0; i < length; i++)
		ASSERT_EQ(line[i], 'x');
	ASSERT_EQ(line[length], '\n');
	ASSERT_EQ(line[length + 1], '\0');
	ASSERT_EQ(getline(&line, &size, fp), 4);
	ASSERT_STR_EQ(line, "last");
	ASSERT_EQ(getline(&line, &size, fp), -1);
	free(line);
	fclose(fp);
}

TEST(peer_credentials_and_invalid_descriptor)
{
	int sockets[2];
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;

	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
	ASSERT_EQ(getpeereid(sockets[0], &uid, &gid), 0);
	ASSERT_EQ(uid, geteuid());
	ASSERT_EQ(gid, getegid());
	close(sockets[0]);
	close(sockets[1]);
#if defined(HAVE_SO_PEERCRED) || defined(HAVE_GETPEERUCRED)
	uid = (uid_t)-1;
	gid = (gid_t)-1;
	ASSERT_EQ(getpeereid(-1, &uid, &gid), -1);
	ASSERT_EQ(uid, (uid_t)-1);
	ASSERT_EQ(gid, (gid_t)-1);
#endif
}

int
main(void)
{
	TEST_SUITE_BEGIN("Production compatibility fallbacks");
	RUN_TEST(base64_roundtrip_and_padding);
	RUN_TEST(base64_rejects_invalid_and_small_buffers);
	RUN_TEST(getline_grows_and_handles_final_line);
	RUN_TEST(peer_credentials_and_invalid_descriptor);
	TEST_SUITE_END();
}
