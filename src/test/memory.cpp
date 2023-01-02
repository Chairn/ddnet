#include <gtest/gtest.h>

#include <base/system.h>
#include <base/vmath.h>

#include <vector>

bool mem_is_null(const void* block, size_t size)
{
	const unsigned char *bytes = (const unsigned char *)block;
	size_t i;
	for(i = 0; i < size; i++)
	{
		if(bytes[i] != 0)
		{
			return false;
		}
	}
	return true;
}

TEST(Memory, BaseTypes)
{
    void* aVoid[123] = {(void*)1,(void*)2,(void*)3,(void*)4};
	EXPECT_FALSE(mem_is_null(aVoid, sizeof(aVoid)));
	mem_zero(&aVoid, sizeof(aVoid));
	EXPECT_TRUE(mem_is_null(aVoid, sizeof(aVoid)));
	aVoid[0] = aVoid;
	EXPECT_FALSE(mem_is_null(aVoid, sizeof(aVoid)));
	mem_zero(aVoid[0], 123*sizeof(void*));
	EXPECT_TRUE(mem_is_null(aVoid, sizeof(aVoid)));
	aVoid[0] = aVoid;
	EXPECT_FALSE(mem_is_null(aVoid, sizeof(aVoid)));
	mem_zero(&aVoid[0], 123*sizeof(void*));
	EXPECT_TRUE(mem_is_null(aVoid, sizeof(aVoid)));

	void* aVoid2[123] = {(void*)1,(void*)2,(void*)3,(void*)4};
	EXPECT_FALSE(mem_is_null(aVoid2, sizeof(aVoid2)));
	mem_zero(aVoid2, sizeof(aVoid2));
	EXPECT_TRUE(mem_is_null(aVoid2, sizeof(aVoid2)));

	int m_aTest[512] = {1,2,3,4,56};
	EXPECT_FALSE(mem_is_null(m_aTest, sizeof(m_aTest)));
	mem_zero(&m_aTest, sizeof(m_aTest));
	EXPECT_TRUE(mem_is_null(m_aTest, sizeof(m_aTest)));
	m_aTest[2] = 42;
	EXPECT_FALSE(mem_is_null(m_aTest, sizeof(m_aTest)));
	mem_zero(&m_aTest[0], sizeof(m_aTest));
	EXPECT_TRUE(mem_is_null(m_aTest, sizeof(m_aTest)));

	int m_aTest2[512] = {1,2,3,4,56};
	EXPECT_FALSE(mem_is_null(m_aTest2, sizeof(m_aTest2)));
	mem_zero(m_aTest2, sizeof(m_aTest2));
	EXPECT_TRUE(mem_is_null(m_aTest2, sizeof(m_aTest2)));

	int* m_apTest[512] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)6,(int*)6};
	EXPECT_FALSE(mem_is_null(m_apTest, sizeof(m_apTest)));
	mem_zero(&m_apTest, sizeof(m_apTest));
	EXPECT_TRUE(mem_is_null(m_apTest, sizeof(m_apTest)));

	int* m_apTest2[512] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)6,(int*)6};
	EXPECT_FALSE(mem_is_null(m_apTest2, sizeof(m_apTest2)));
	mem_zero(m_apTest2, sizeof(m_apTest2));
	EXPECT_TRUE(mem_is_null(m_apTest2, sizeof(m_apTest2)));

	int aaTest[10][20] = {1,2,3,4,5,6,78,9};
	EXPECT_FALSE(mem_is_null(aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	EXPECT_TRUE(mem_is_null(aaTest, sizeof(aaTest)));
	aaTest[0][0] = 42;
	EXPECT_FALSE(mem_is_null(aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null(aaTest, sizeof(aaTest)));
	aaTest[0][0] = 42; aaTest[9][19] = 42;
	EXPECT_FALSE(mem_is_null(aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null(aaTest, sizeof(aaTest)));

	int aaTest2[10][20] = {1,2,3,4,5,6,78,9};
	EXPECT_FALSE(mem_is_null(aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	EXPECT_TRUE(mem_is_null(aaTest2, sizeof(aaTest2)));
	aaTest2[0][0] = 42; aaTest2[9][19] = 42;
	EXPECT_FALSE(mem_is_null(aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null(aaTest2, sizeof(aaTest2)));

	int* aapTest[10][20] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)5,(int*)6,(int*)78,(int*)9};
	EXPECT_FALSE(mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest, sizeof(aapTest));
	EXPECT_TRUE(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (int*)42; aapTest[9][19] = (int*)42;
	EXPECT_FALSE(mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0], sizeof(aapTest));
	EXPECT_TRUE(mem_is_null(aapTest, sizeof(aapTest)));

	int* aapTest2[10][20] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)5,(int*)6,(int*)78,(int*)9};
	EXPECT_FALSE(mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2, sizeof(aapTest2));
	EXPECT_TRUE(mem_is_null(aapTest2, sizeof(aapTest2)));
	aapTest2[0][0] = (int*)42; aapTest2[9][19] = (int*)42;
	EXPECT_FALSE(mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2[0], sizeof(aapTest2));
	EXPECT_TRUE(mem_is_null(aapTest2, sizeof(aapTest2)));
}

TEST(Memory, PODTypes)
{
	NETADDR aTest[2];	aTest[0].port = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].port = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));

	NETADDR aTest2[2];	aTest2[0].port = 42;
	EXPECT_FALSE(mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	EXPECT_TRUE(mem_is_null(aTest2, sizeof(aTest2)));

	NETADDR* apTest[123] = {(NETADDR*)1,(NETADDR*)2,(NETADDR*)3,(NETADDR*)4,(NETADDR*)6,(NETADDR*)6};
	EXPECT_FALSE(mem_is_null(apTest, sizeof(apTest)));
	mem_zero((NETADDR**)apTest, sizeof(apTest));
	EXPECT_TRUE(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (NETADDR*)apTest; apTest[122] = (NETADDR*)apTest;
	EXPECT_FALSE(mem_is_null(apTest, sizeof(apTest)));
	mem_zero(&apTest, sizeof(apTest));
	EXPECT_TRUE(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (NETADDR*)apTest; apTest[122] = (NETADDR*)apTest;
	EXPECT_FALSE(mem_is_null(apTest, sizeof(apTest)));
	mem_zero(&apTest[0], sizeof(apTest));
	EXPECT_TRUE(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (NETADDR*)apTest; apTest[122] = (NETADDR*)apTest;;
	EXPECT_FALSE(mem_is_null(apTest, sizeof(apTest)));
	mem_zero(apTest, sizeof(apTest));
	EXPECT_TRUE(mem_is_null(apTest, sizeof(apTest)));
	
	// 2D arrays
	NETADDR aaTest[2][2] = {1,2,3,4};
	EXPECT_FALSE(mem_is_null((NETADDR*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((NETADDR*)aaTest, sizeof(aaTest)));
	aaTest[0][0].port = 42; aaTest[1][1].port = 42;
	EXPECT_FALSE(mem_is_null((NETADDR*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((NETADDR*)aaTest, sizeof(aaTest)));
	aaTest[0][0].port = 42; aaTest[1][1].port = 42;
	EXPECT_FALSE(mem_is_null((NETADDR*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((NETADDR*)aaTest, sizeof(aaTest)));

	NETADDR aaTest2[2][2] = {1,2,3,4};
	EXPECT_FALSE(mem_is_null((NETADDR*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	EXPECT_TRUE(mem_is_null((NETADDR*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].port = 42; aaTest2[1][1].port = 42;
	EXPECT_FALSE(mem_is_null((NETADDR*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((NETADDR*)aaTest2, sizeof(aaTest2)));

	// 2D pointer arrays
	NETADDR* aapTest[10][20] = {(NETADDR*)1,(NETADDR*)2,(NETADDR*)3,(NETADDR*)4,(NETADDR*)5,(NETADDR*)6,(NETADDR*)78,(NETADDR*)9};
	EXPECT_FALSE(mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest, sizeof(aapTest));
	EXPECT_TRUE(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (NETADDR*)42; aapTest[9][19] = (NETADDR*)42;
	EXPECT_FALSE(mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0], sizeof(aapTest));
	EXPECT_TRUE(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (NETADDR*)42; aapTest[9][19] = (NETADDR*)42;
	EXPECT_FALSE(mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0][0], sizeof(aapTest));
	EXPECT_TRUE(mem_is_null(aapTest, sizeof(aapTest)));

	NETADDR* aapTest2[10][20] = {(NETADDR*)1,(NETADDR*)2,(NETADDR*)3,(NETADDR*)4,(NETADDR*)5,(NETADDR*)6,(NETADDR*)78,(NETADDR*)9};
	EXPECT_FALSE(mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2, sizeof(aapTest2));
	EXPECT_TRUE(mem_is_null(aapTest2, sizeof(aapTest2)));
	aapTest2[0][0] = (NETADDR*)42; aapTest2[9][19] = (NETADDR*)42;
	EXPECT_FALSE(mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2[0], sizeof(aapTest2));
	EXPECT_TRUE(mem_is_null(aapTest2, sizeof(aapTest2)));
}

struct CDummyClass
{
    int x = 1234;
	int y;
};

bool mem_is_null(CDummyClass* block, size_t size)
{
    const CDummyClass *data = block;
	size_t i;
	for(i = 0; i < size/sizeof(CDummyClass); i++)
	{
		if(data[i].x != 1234 || data[i].y != 0)
		{
			return false;
		}
	}
	return true;
}

TEST(Memory, ConstructibleTypes)
{
    CDummyClass aTest[2];	aTest[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));

	CDummyClass aTest2[2];	aTest2[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	EXPECT_TRUE(mem_is_null(aTest2, sizeof(aTest2)));
	
	CDummyClass aaTest[2][2] = {1,2,3,4};
	EXPECT_FALSE(mem_is_null((CDummyClass*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass*)aaTest, sizeof(aaTest)));
	aaTest[0][0].x = 42; aaTest[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass*)aaTest, sizeof(aaTest)));
	aaTest[0][0].x = 42; aaTest[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass*)aaTest, sizeof(aaTest)));

	CDummyClass aaTest2[2][2] = {1,2,3,4};
	EXPECT_FALSE(mem_is_null((CDummyClass*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	EXPECT_TRUE(mem_is_null((CDummyClass*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].x = 42; aaTest2[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass*)aaTest2, sizeof(aaTest2)));
}

struct CDummyClass2
{
    int x;
    int y;
    ~CDummyClass2()
    {
        if(x == 684684 && y == -12345678)
            dbg_break();
    }
};

TEST(Memory, DestructibleTypes)
{
    CDummyClass2 aTest[2];	aTest[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));

	CDummyClass2 aTest2[2];	aTest2[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	EXPECT_TRUE(mem_is_null(aTest2, sizeof(aTest2)));
	
	CDummyClass2 aaTest[2][2] = {1,2,3,4};
	EXPECT_FALSE(mem_is_null((CDummyClass2*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass2*)aaTest, sizeof(aaTest)));
	aaTest[0][0].x = 42; aaTest[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass2*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass2*)aaTest, sizeof(aaTest)));
	aaTest[0][0].x = 42; aaTest[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass2*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass2*)aaTest, sizeof(aaTest)));

	CDummyClass2 aaTest2[2][2] = {1,2,3,4};
	EXPECT_FALSE(mem_is_null((CDummyClass2*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	EXPECT_TRUE(mem_is_null((CDummyClass2*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].x = 42; aaTest2[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass2*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass2*)aaTest2, sizeof(aaTest2)));
}

struct CDummyClass3
{
    int x;
    int y;
    std::vector<int> v;
};

bool mem_is_null(CDummyClass3* block, size_t size)
{
    const CDummyClass3 *data = block;
	size_t i;
	for(i = 0; i < size/sizeof(CDummyClass3); i++)
	{
		if(data[i].x != 0 || data[i].y != 0 || data[i].v.size() != 0)
		{
			return false;
		}
	}
	return true;
}

TEST(Memory, ComplexTypes)
{
    CDummyClass3 aTest[2];	aTest[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	EXPECT_TRUE(mem_is_null(aTest, sizeof(aTest)));

	CDummyClass3 aTest2[2];	aTest2[0].x = 42;
	EXPECT_FALSE(mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	EXPECT_TRUE(mem_is_null(aTest2, sizeof(aTest2)));
	
	CDummyClass3 aaTest[2][2] = {1,2,{3,4}};
	EXPECT_FALSE(mem_is_null((CDummyClass3*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass3*)aaTest, sizeof(aaTest)));
	aaTest[0][0].x = 42; aaTest[1][1].y = 42;
	EXPECT_FALSE(mem_is_null((CDummyClass3*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass3*)aaTest, sizeof(aaTest)));
	aaTest[0][0].x = 42; aaTest[1][1].v = {42};
	EXPECT_FALSE(mem_is_null((CDummyClass3*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass3*)aaTest, sizeof(aaTest)));

	CDummyClass3 aaTest2[2][2] = {1,2,{3,4}};
	EXPECT_FALSE(mem_is_null((CDummyClass3*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	EXPECT_TRUE(mem_is_null((CDummyClass3*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].x = 42; aaTest2[1][1].v = {42};
	EXPECT_FALSE(mem_is_null((CDummyClass3*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	EXPECT_TRUE(mem_is_null((CDummyClass3*)aaTest2, sizeof(aaTest2)));
}
