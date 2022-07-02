#include "test.h"

#include <gtest/gtest.h>

#include <base/system.h>

static const int gs_aIntData[] = {0, 1, -1, 32, 64, 256, -512, 12345, -123456, 1234567, 12345678, 123456789, 2147483647, (-2147483647 - 1)};
static const int gs_IntNum = std::size(gs_aIntData);

static const unsigned gs_aUIntData[] = {0u, 1u, 2u, 32u, 64u, 256u, 512u, 12345u, 123456u, 1234567u, 12345678u, 123456789u, 2147483647u, 2147483648u, 4294967295u};
static const int gs_UIntNum = std::size(gs_aIntData);

TEST(BytePacking, RoundtripInt)
{
	for(int i = 0; i < gs_IntNum; i++)
	{
		unsigned char aPacked[4];
		int_to_bytes_be(aPacked, gs_aIntData[i]);
		EXPECT_EQ(bytes_be_to_int(aPacked), gs_aIntData[i]);
	}
}

TEST(BytePacking, RoundtripUnsigned)
{
	for(int i = 0; i < gs_UIntNum; i++)
	{
		unsigned char aPacked[4];
		uint_to_bytes_be(aPacked, gs_aUIntData[i]);
		EXPECT_EQ(bytes_be_to_uint(aPacked), gs_aUIntData[i]);
	}
}
