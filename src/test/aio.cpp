#include "test.h"
#include <gtest/gtest.h>

#include <base/system.h>

static const int gs_BufSize = 64 * 1024;

class Async : public ::testing::Test
{
protected:
	ASYNCIO *m_pAio;
	CTestInfo m_Info;
	bool Delete;

	void SetUp() override
	{
		IOHANDLE File = io_open(m_Info.m_aFilename, IOFLAG_WRITE);
		ASSERT_TRUE(File);
		m_pAio = aio_new(File);
		Delete = false;
	}

	~Async()
	{
		if(Delete)
		{
			fs_remove(m_Info.m_aFilename);
		}
	}

	void Write(const char *pText)
	{
		aio_write(m_pAio, pText, str_length(pText));
	}

	void Expect(const char *pOutput)
	{
		aio_close(m_pAio);
		aio_wait(m_pAio);
		aio_free(m_pAio);

		char aBuf[gs_BufSize];
		IOHANDLE File = io_open(m_Info.m_aFilename, IOFLAG_READ);
		ASSERT_TRUE(File);
		int Read = io_read(File, aBuf, sizeof(aBuf));
		io_close(File);

		ASSERT_EQ(str_length(pOutput), Read);
		ASSERT_TRUE(mem_comp(aBuf, pOutput, Read) == 0);
		Delete = true;
	}
};

TEST_F(Async, Empty)
{
	Expect("");
}

TEST_F(Async, Simple)
{
	static const char TEXT[] = "a\n";
	Write(TEXT);
	Expect(TEXT);
}

TEST_F(Async, Long)
{
	char aText[gs_BufSize + 1];
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		aText[i] = 'a';
	}
	aText[sizeof(aText) - 1] = 0;
	Write(aText);
	Expect(aText);
}

TEST_F(Async, Pieces)
{
	char aText[gs_BufSize + 1];
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		aText[i] = 'a';
	}
	aText[sizeof(aText) - 1] = 0;
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		Write("a");
	}
	Expect(aText);
}

TEST_F(Async, Mixed)
{
	char aText[gs_BufSize + 1];
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		aText[i] = 'a' + i % 26;
	}
	aText[sizeof(aText) - 1] = 0;
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		char w = 'a' + i % 26;
		aio_write(m_pAio, &w, 1);
	}
	Expect(aText);
}

TEST_F(Async, NonDivisor)
{
	static const int s_NumLetters = 13;
	static const int s_Size = gs_BufSize / s_NumLetters * s_NumLetters;
	char aText[s_Size + 1];
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		aText[i] = 'a' + i % s_NumLetters;
	}
	aText[sizeof(aText) - 1] = 0;
	for(unsigned i = 0; i < (sizeof(aText) - 1) / s_NumLetters; i++)
	{
		Write("abcdefghijklm");
	}
	Expect(aText);
}

TEST_F(Async, Transaction)
{
	static const int s_NumLetters = 13;
	static const int s_Size = gs_BufSize / s_NumLetters * s_NumLetters;
	char aText[s_Size + 1];
	for(unsigned i = 0; i < sizeof(aText) - 1; i++)
	{
		aText[i] = 'a' + i % s_NumLetters;
	}
	aText[sizeof(aText) - 1] = 0;
	for(unsigned i = 0; i < (sizeof(aText) - 1) / s_NumLetters; i++)
	{
		aio_lock(m_pAio);
		for(char c = 'a'; c < 'a' + s_NumLetters; c++)
		{
			aio_write_unlocked(m_pAio, &c, 1);
		}
		aio_unlock(m_pAio);
	}
	Expect(aText);
}
