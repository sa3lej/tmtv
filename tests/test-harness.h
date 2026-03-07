/*
 * Minimal C test harness for tmtv.
 * No external dependencies - just include this header.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _tests_run = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do { \
	_tests_run++; \
	printf("  %-50s ", #name); \
	test_##name(); \
	_tests_passed++; \
	printf("PASS\n"); \
} while (0)

#define ASSERT(cond) do { \
	if (!(cond)) { \
		printf("FAIL\n"); \
		fprintf(stderr, "    %s:%d: assertion failed: %s\n", \
		    __FILE__, __LINE__, #cond); \
		_tests_failed++; \
		_tests_passed--; \
		return; \
	} \
} while (0)

#define ASSERT_EQ(a, b) do { \
	if ((a) != (b)) { \
		printf("FAIL\n"); \
		fprintf(stderr, "    %s:%d: expected %lld, got %lld\n", \
		    __FILE__, __LINE__, (long long)(b), (long long)(a)); \
		_tests_failed++; \
		_tests_passed--; \
		return; \
	} \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
	if (strcmp((a), (b)) != 0) { \
		printf("FAIL\n"); \
		fprintf(stderr, "    %s:%d: expected \"%s\", got \"%s\"\n", \
		    __FILE__, __LINE__, (b), (a)); \
		_tests_failed++; \
		_tests_passed--; \
		return; \
	} \
} while (0)

#define ASSERT_NOT_NULL(p) do { \
	if ((p) == NULL) { \
		printf("FAIL\n"); \
		fprintf(stderr, "    %s:%d: expected non-NULL\n", \
		    __FILE__, __LINE__); \
		_tests_failed++; \
		_tests_passed--; \
		return; \
	} \
} while (0)

#define TEST_SUITE_BEGIN(name) \
	printf("\n== %s ==\n", name)

#define TEST_SUITE_END() do { \
	printf("\n%d tests: %d passed, %d failed\n", \
	    _tests_run, _tests_passed, _tests_failed); \
	return _tests_failed > 0 ? 1 : 0; \
} while (0)

#endif /* TEST_HARNESS_H */
