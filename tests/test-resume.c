#include "test-harness.h"
#include "../server/tmate-resume.h"
#include <fcntl.h>
#include <unistd.h>

static const char *secret =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

TEST(stable_independent_capabilities)
{
	char rw[33], ro[36], rw2[33], ro2[36];
	ASSERT_EQ(tmtv_resume_tokens(secret, rw, ro), 0);
	ASSERT_EQ(tmtv_resume_tokens(secret, rw2, ro2), 0);
	ASSERT_STR_EQ(rw, "43fafc568044e3b9a463e274e7ba6e4b");
	ASSERT_STR_EQ(ro, "ro-0c272337aaaa89b7d05aa25390053719");
	ASSERT_STR_EQ(rw, rw2);
	ASSERT_STR_EQ(ro, ro2);
	ASSERT_EQ(strlen(rw), 32);
	ASSERT_EQ(strlen(ro), 35);
	ASSERT(strncmp(ro, "ro-", 3) == 0);
	ASSERT(strcmp(rw, ro + 3) != 0);
	ASSERT_EQ(tmtv_resume_tokens(
	    "100102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", rw2, ro2), 0);
	ASSERT(strcmp(rw, rw2) != 0);
	ASSERT(strcmp(ro, ro2) != 0);
}

TEST(viewer_tokens_are_not_identity)
{
	char rw[33], ro[36], out[33], out_ro[36];
	ASSERT_EQ(tmtv_resume_tokens(secret, rw, ro), 0);
	ASSERT_EQ(tmtv_resume_tokens(rw, out, out_ro), -1);
	ASSERT_EQ(tmtv_resume_tokens(ro, out, out_ro), -1);
	ASSERT_EQ(tmtv_resume_tokens("", out, out_ro), -1);
	ASSERT_EQ(tmtv_resume_tokens(
	    "z00102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", out, out_ro), -1);
}

TEST(old_cleanup_cannot_remove_replacement)
{
	char path[] = "/tmp/tmtv-resume-test-XXXXXX";
	ASSERT_NOT_NULL(mkdtemp(path));
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	ASSERT(fd >= 0);
	ASSERT_EQ(tmtv_alias_replace(fd, "alias", "old"), 0);
	ASSERT(tmtv_alias_points_to(fd, "alias", "old"));
	ASSERT_EQ(tmtv_alias_replace(fd, "alias", "replacement"), 0);
	tmtv_alias_remove_owned(fd, "alias", "old");
	ASSERT(tmtv_alias_points_to(fd, "alias", "replacement"));
	tmtv_alias_remove_owned(fd, "alias", "replacement");
	ASSERT(!tmtv_alias_points_to(fd, "alias", "replacement"));
	close(fd);
	ASSERT_EQ(rmdir(path), 0);
}

int main(void)
{
	TEST_SUITE_BEGIN("Reconnect identity and alias ownership");
	RUN_TEST(stable_independent_capabilities);
	RUN_TEST(viewer_tokens_are_not_identity);
	RUN_TEST(old_cleanup_cannot_remove_replacement);
	TEST_SUITE_END();
}
