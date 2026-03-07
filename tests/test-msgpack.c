/*
 * Unit tests for tmate msgpack encoding/decoding.
 */

#include <msgpack.h>
#include <string.h>
#include "test-harness.h"

/* Test basic msgpack packing */
TEST(msgpack_pack_string)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	msgpack_unpacked result;
	const char *test_str = "hello tmtv";

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	size_t len = strlen(test_str);
	msgpack_pack_str(&pk, len);
	msgpack_pack_str_body(&pk, test_str, len);

	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, sbuf.data, sbuf.size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS);
	ASSERT_EQ(result.data.type, MSGPACK_OBJECT_STR);
	ASSERT_EQ(result.data.via.str.size, len);
	ASSERT(memcmp(result.data.via.str.ptr, test_str, len) == 0);

	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&sbuf);
}

TEST(msgpack_pack_int)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	msgpack_unpacked result;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_int(&pk, 42);

	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, sbuf.data, sbuf.size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS);
	ASSERT_EQ(result.data.via.i64, 42);

	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&sbuf);
}

TEST(msgpack_pack_array)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	msgpack_unpacked result;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, 1);
	msgpack_pack_int(&pk, 2);
	msgpack_pack_int(&pk, 3);

	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, sbuf.data, sbuf.size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS);
	ASSERT_EQ(result.data.type, MSGPACK_OBJECT_ARRAY);
	ASSERT_EQ(result.data.via.array.size, 3);
	ASSERT_EQ(result.data.via.array.ptr[0].via.i64, 1);
	ASSERT_EQ(result.data.via.array.ptr[1].via.i64, 2);
	ASSERT_EQ(result.data.via.array.ptr[2].via.i64, 3);

	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&sbuf);
}

TEST(msgpack_pack_boolean)
{
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	msgpack_unpacked result;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_true(&pk);

	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, sbuf.data, sbuf.size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS);
	ASSERT_EQ(result.data.type, MSGPACK_OBJECT_BOOLEAN);
	ASSERT(result.data.via.boolean);

	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&sbuf);
}

TEST(msgpack_roundtrip_protocol_message)
{
	/* Simulate a TMATE_OUT_HEADER message: [type, version, string] */
	msgpack_sbuffer sbuf;
	msgpack_packer pk;
	msgpack_unpacked result;

	msgpack_sbuffer_init(&sbuf);
	msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, 0); /* TMATE_OUT_HEADER */
	msgpack_pack_int(&pk, 6); /* protocol version */
	size_t vlen = strlen("3.6a");
	msgpack_pack_str(&pk, vlen);
	msgpack_pack_str_body(&pk, "3.6a", vlen);

	msgpack_unpacked_init(&result);
	ASSERT(msgpack_unpack_next(&result, sbuf.data, sbuf.size, NULL) ==
	    MSGPACK_UNPACK_SUCCESS);

	msgpack_object_array arr = result.data.via.array;
	ASSERT_EQ(arr.size, 3);
	ASSERT_EQ(arr.ptr[0].via.i64, 0);
	ASSERT_EQ(arr.ptr[1].via.i64, 6);
	ASSERT_EQ(arr.ptr[2].via.str.size, 4);
	ASSERT(memcmp(arr.ptr[2].via.str.ptr, "3.6a", 4) == 0);

	msgpack_unpacked_destroy(&result);
	msgpack_sbuffer_destroy(&sbuf);
}

int
main(void)
{
	TEST_SUITE_BEGIN("msgpack encoding/decoding");

	RUN_TEST(msgpack_pack_string);
	RUN_TEST(msgpack_pack_int);
	RUN_TEST(msgpack_pack_array);
	RUN_TEST(msgpack_pack_boolean);
	RUN_TEST(msgpack_roundtrip_protocol_message);

	TEST_SUITE_END();
}
