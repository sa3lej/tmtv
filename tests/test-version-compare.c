/*
 * Unit tests for tmtv_version_compare().
 *
 * Verifies semver comparison: equal, older, newer, partial versions,
 * malformed strings, and NULL inputs.
 */

#include "test-harness.h"

/* Declare the function under test (defined in server/tmate-daemon-decoder.c) */
extern int tmtv_version_compare(const char *a, const char *b);

/*
 * Provide a standalone implementation here so the test compiles without
 * linking the entire server.  This must be kept in sync with the real
 * implementation in server/tmate-daemon-decoder.c.
 */
#include <stdio.h>

int
tmtv_version_compare(const char *a, const char *b)
{
	int a_major = 0, a_minor = 0, a_patch = 0;
	int b_major = 0, b_minor = 0, b_patch = 0;

	if (a != NULL)
		sscanf(a, "%d.%d.%d", &a_major, &a_minor, &a_patch);
	if (b != NULL)
		sscanf(b, "%d.%d.%d", &b_major, &b_minor, &b_patch);

	if (a_major != b_major)
		return a_major - b_major;
	if (a_minor != b_minor)
		return a_minor - b_minor;
	return a_patch - b_patch;
}

/* Equal versions */
TEST(equal_versions)
{
	ASSERT_EQ(tmtv_version_compare("1.3.5", "1.3.5"), 0);
	ASSERT_EQ(tmtv_version_compare("0.0.0", "0.0.0"), 0);
	ASSERT_EQ(tmtv_version_compare("10.20.30", "10.20.30"), 0);
}

/* Client older than server (a < b) */
TEST(client_older)
{
	ASSERT(tmtv_version_compare("1.2.0", "1.3.0") < 0);
	ASSERT(tmtv_version_compare("1.3.4", "1.3.5") < 0);
	ASSERT(tmtv_version_compare("0.9.9", "1.0.0") < 0);
	ASSERT(tmtv_version_compare("1.2.9", "1.3.0") < 0);
}

/* Client newer than server (a > b) */
TEST(client_newer)
{
	ASSERT(tmtv_version_compare("2.0.0", "1.9.9") > 0);
	ASSERT(tmtv_version_compare("1.3.6", "1.3.5") > 0);
	ASSERT(tmtv_version_compare("1.4.0", "1.3.99") > 0);
}

/* Major version difference dominates */
TEST(major_dominates)
{
	ASSERT(tmtv_version_compare("2.0.0", "1.99.99") > 0);
	ASSERT(tmtv_version_compare("1.0.0", "2.0.0") < 0);
}

/* Minor version difference when major is equal */
TEST(minor_difference)
{
	ASSERT(tmtv_version_compare("1.4.0", "1.3.0") > 0);
	ASSERT(tmtv_version_compare("1.2.0", "1.3.0") < 0);
}

/* Patch version difference when major.minor are equal */
TEST(patch_difference)
{
	ASSERT(tmtv_version_compare("1.3.5", "1.3.4") > 0);
	ASSERT(tmtv_version_compare("1.3.4", "1.3.5") < 0);
}

/* Malformed strings treated as 0.0.0 */
TEST(malformed_strings)
{
	ASSERT_EQ(tmtv_version_compare("garbage", "garbage"), 0);
	ASSERT(tmtv_version_compare("garbage", "1.0.0") < 0);
	ASSERT(tmtv_version_compare("1.0.0", "garbage") > 0);
	/* "unknown" is what the server stores for old clients */
	ASSERT(tmtv_version_compare("unknown", "1.0.0") < 0);
}

/* NULL inputs treated as 0.0.0 */
TEST(null_inputs)
{
	ASSERT_EQ(tmtv_version_compare(NULL, NULL), 0);
	ASSERT(tmtv_version_compare(NULL, "1.0.0") < 0);
	ASSERT(tmtv_version_compare("1.0.0", NULL) > 0);
}

/* Partial version strings (missing components default to 0) */
TEST(partial_versions)
{
	ASSERT_EQ(tmtv_version_compare("1", "1.0.0"), 0);
	ASSERT_EQ(tmtv_version_compare("1.2", "1.2.0"), 0);
	ASSERT(tmtv_version_compare("1", "1.0.1") < 0);
	ASSERT(tmtv_version_compare("1.2", "1.2.1") < 0);
}

int main(void)
{
	TEST_SUITE_BEGIN("tmtv_version_compare");

	RUN_TEST(equal_versions);
	RUN_TEST(client_older);
	RUN_TEST(client_newer);
	RUN_TEST(major_dominates);
	RUN_TEST(minor_difference);
	RUN_TEST(patch_difference);
	RUN_TEST(malformed_strings);
	RUN_TEST(null_inputs);
	RUN_TEST(partial_versions);

	TEST_SUITE_END();
}
