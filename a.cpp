#include <iostream>
#include <iomanip>
#include <algorithm>
#include <type_traits>
#include <cassert>
#include <cstring>

#include <string>
#include <typeinfo>
#include <cstdlib>
#include <memory>
#include <cxxabi.h>
#include <vector>

#include "../src/engine/client/client.h"
CClient::CClient() :
	m_DemoPlayer(&m_SnapshotDelta, [&]() { UpdateDemoIntraTimers(); })
{
	for(auto &DemoRecorder : m_aDemoRecorder)
		DemoRecorder = CDemoRecorder(&m_SnapshotDelta);

	m_pEditor = 0;
	m_pInput = 0;
	m_pGraphics = 0;
	m_pSound = 0;
	m_pGameClient = 0;
	m_pMap = 0;
	m_pConfigManager = 0;
	m_pConfig = 0;
	m_pConsole = 0;

	m_RenderFrameTime = 0.0001f;
	m_RenderFrameTimeLow = 1.0f;
	m_RenderFrameTimeHigh = 0.0f;
	m_RenderFrames = 0;
	m_LastRenderTime = time_get();

	m_GameTickSpeed = SERVER_TICK_SPEED;

	m_SnapCrcErrors = 0;
	m_AutoScreenshotRecycle = false;
	m_AutoStatScreenshotRecycle = false;
	m_AutoCSVRecycle = false;
	m_EditorActive = false;

	m_aAckGameTick[0] = -1;
	m_aAckGameTick[1] = -1;
	m_aCurrentRecvTick[0] = 0;
	m_aCurrentRecvTick[1] = 0;
	m_aRconAuthed[0] = 0;
	m_aRconAuthed[1] = 0;
	m_aRconPassword[0] = '\0';
	m_aPassword[0] = '\0';

	// version-checking
	m_aVersionStr[0] = '0';
	m_aVersionStr[1] = '\0';

	// pinging
	m_PingStartTime = 0;

	m_aCurrentMap[0] = 0;

	m_aCmdConnect[0] = 0;

	// map download
	m_aMapdownloadFilename[0] = 0;
	m_aMapdownloadFilenameTemp[0] = 0;
	m_aMapdownloadName[0] = 0;
	m_pMapdownloadTask = NULL;
	m_MapdownloadFileTemp = 0;
	m_MapdownloadChunk = 0;
	m_MapdownloadSha256Present = false;
	m_MapdownloadSha256 = SHA256_ZEROED;
	m_MapdownloadCrc = 0;
	m_MapdownloadAmount = -1;
	m_MapdownloadTotalsize = -1;

	m_MapDetailsPresent = false;
	m_aMapDetailsName[0] = 0;
	m_MapDetailsSha256 = SHA256_ZEROED;
	m_MapDetailsCrc = 0;
	m_aMapDetailsUrl[0] = 0;

	IStorage::FormatTmpPath(m_aDDNetInfoTmp, sizeof(m_aDDNetInfoTmp), DDNET_INFO);
	m_pDDNetInfoTask = NULL;
	m_aNews[0] = '\0';
	m_aMapDownloadUrl[0] = '\0';
	m_Points = -1;

	m_CurrentServerInfoRequestTime = -1;
	m_CurrentServerPingInfoType = -1;
	m_CurrentServerPingBasicToken = -1;
	m_CurrentServerPingToken = -1;
	mem_zero(&m_CurrentServerPingUuid, sizeof(m_CurrentServerPingUuid));
	m_CurrentServerCurrentPingTime = -1;
	m_CurrentServerNextPingTime = -1;

	m_aCurrentInput[0] = 0;
	m_aCurrentInput[1] = 0;
	m_LastDummy = false;

	mem_zero(&m_aInputs, sizeof(m_aInputs));

	m_State = IClient::STATE_OFFLINE;
	m_StateStartTime = time_get();
	m_aConnectAddressStr[0] = 0;

	mem_zero(m_aapSnapshots, sizeof(m_aapSnapshots));
	m_aSnapshotStorage[0].Init();
	m_aSnapshotStorage[1].Init();
	m_aReceivedSnapshots[0] = 0;
	m_aReceivedSnapshots[1] = 0;
	m_aSnapshotParts[0] = 0;
	m_aSnapshotParts[1] = 0;

	m_VersionInfo.m_State = CVersionInfo::STATE_INIT;

	if(g_Config.m_ClDummy == 0)
		m_LastDummyConnectTime = 0;

	m_ReconnectTime = 0;

	m_GenerateTimeoutSeed = true;

	m_FrameTimeAvg = 0.0001f;
	m_BenchmarkFile = 0;
	m_BenchmarkStopTime = 0;

	mem_zero(&m_Checksum, sizeof(m_Checksum));
}

std::string demangle(const char* name)
{
    int status = -4; // some arbitrary value to eliminate the compiler warning

    // enable c++11 by passing the flag -std=c++11 to g++
    std::unique_ptr<char, void(*)(void*)> res {
        abi::__cxa_demangle(name, NULL, NULL, &status),
        std::free
    };

    return (status==0) ? res.get() : name ;
}

/*template <class T>
std::string type(const T& t) {

    return demangle(typeid(t).name());
}*/

int mem_is_null(const void* block, size_t size)
{
	const unsigned char *bytes = (const unsigned char *)block;
	size_t i;
	for(i = 0; i < size; i++)
	{
		if(bytes[i] != 0)
		{
			return 0;
		}
	}
	return 1;
}

class CEntry
{
public:
	CEntry(int num = 12)
	: m_NumAddrs(num), m_AllowPing(false)
	{
		std::cout << std::noboolalpha;
		std::cout << "Entry created @" << (void*)this << " with values " << m_NumAddrs << ", " << m_AllowPing << std::endl;
	}
	~CEntry()
	{
		std::cout << std::noboolalpha;
		std::cout << "Entry deleted @" << (void*)this << " with values " << m_NumAddrs << ", " << m_AllowPing << std::endl;
	}
	int m_NumAddrs;
	bool m_AllowPing;
};

class CEntry2
{
public:
	int m_NumAddrs;
	bool m_AllowPing;
};

class CEntry3
{
public:
	int m_NumAddrs = 1234;
	bool m_AllowPing;
};

class CEntry4
{
public:
	int m_NumAddrs;
	bool m_AllowPing;
	std::vector<int> m_vAddrs;
};

class CEntry5
{
public:
	CEntry5(int num, bool allow)
	: m_NumAddrs(num), m_AllowPing(allow)
	{}
	int m_NumAddrs;
	bool m_AllowPing;
};
class CEntry6
{
public:
	CEntry6()
	: m_NumAddrs(0), m_AllowPing(false) {}
	CEntry6(int num, bool allow)
	: m_NumAddrs(num), m_AllowPing(allow)
	{}
	~CEntry6() = default;
	int m_NumAddrs;
	bool m_AllowPing;
};

/*int mem_is_null(const CEntry* block, size_t size)
{
	const CEntry *data = block;
	size_t i;
	for(i = 0; i < size/sizeof(CEntry); i++)
	{
		if(data[i].m_NumAddrs != 12 || data[i].m_AllowPing != false)
		{
			return 0;
		}
	}
	return 1;
}
int mem_is_null(const CEntry3* block, size_t size)
{
	const CEntry3 *data = block;
	size_t i;
	for(i = 0; i < size/sizeof(CEntry3); i++)
	{
		if(data[i].m_NumAddrs != 1234 || data[i].m_AllowPing != false)
		{
			return 0;
		}
	}
	return 1;
}

template<typename T>
[[gnu::warning("pointer type")]]
inline void mem_zero(T *block, size_t size)
{
	typedef typename std::remove_all_extents<T>::type BaseT;
	std::cout << "----------------------------------------------------------------------------------" << std::endl << std::boolalpha
			  << std::left << std::setw(32) << typeid(block).name() << type(block) << std::endl
			  << std::left << std::setw(32) << typeid(*block).name() << type(*block) << std::endl 
			  << std::setw(32) << "is_pointer    " << std::is_pointer    <T>::value << std::endl
			  << std::setw(32) << "is_pointer (no extents)" << std::is_pointer<BaseT>::value << std::endl
			  << std::setw(32) << "is_array      " << std::is_array      <T>::value << std::endl
			  << std::setw(32) << "is_fundamental" << std::is_fundamental<T>::value << std::endl
			  << std::setw(32) << "is_fundamental (no extents)" << std::is_fundamental<BaseT>::value << std::endl
			  << std::setw(32) << "is_void       " << std::is_same <T, void>::value << std::endl
			  << std::setw(32) << "is_void (no extents)" << std::is_same<BaseT, void>::value << std::endl
			  << std::setw(32) << "is_void*      " << std::is_same <T, void>::value << std::endl
			  << std::setw(32) << "is_void* (no extents)" << std::is_same<BaseT, void*>::value << std::endl
			  << std::setw(32) << "is_default_cons" << std::is_default_constructible<T>::value << std::endl
			  << std::setw(32) << "is_default_cons (no extents)" << std::is_default_constructible<BaseT>::value << std::endl
			  << std::setw(32) << "is_trivial_cons" << std::is_trivially_constructible<T>::value << std::endl
			  << std::setw(32) << "is_trivial_cons (no extents)" << std::is_trivially_constructible<BaseT>::value << std::endl
			  << std::setw(32) << "is_destructible" << std::is_destructible<T>::value << std::endl
			  << std::setw(32) << "is_destructible (no extents)" << std::is_destructible<BaseT>::value << std::endl
			  << std::setw(32) << "is_trivial_dest" << std::is_trivially_destructible<T>::value << std::endl
			  << std::setw(32) << "is_trivial_dest (no extents)" << std::is_trivially_destructible<BaseT>::value << std::endl;

	if constexpr(std::is_pointer<T>::value || std::is_pointer<BaseT>::value)
	{
		// pointer of pointer, just memset it
		std::cout << "pointer of pointer, just memset it" << std::endl << std::endl;
		memset(block, 0, size);
	}
	else if constexpr(std::is_array<T>::value)
	{	// pointer to array type
		std::cout << "100% array of type " << typeid(typename std::remove_pointer<T>::type).name() << "      ";
		if constexpr(std::is_fundamental<BaseT>::value)
		{	// array of fundamental type, just memset it
			std::cout << "which is array of fundamental so memsetting it" << std::endl << std::endl;
			memset(block, 0, size);
		}
		else
		{	// array of user defined type, destroying all objects and recreating new ones
			std::cout << "which is array of user defined type, ";
			const size_t N = size/sizeof(BaseT);
			if constexpr(!std::is_trivially_destructible<BaseT>::value)
			{	// non trivial destructor means user provided or virtual destructor, see https://en.cppreference.com/w/cpp/language/destructor#Trivial_destructor
				// so we gotta call it manually
				std::cout << "destroying all objects ";
				for(size_t i(0); i < N; ++i)
				{
					((BaseT*)block)[i].~BaseT();
					// (*block)[i].~BaseT();
				}
			}
			if constexpr(std::is_trivially_constructible<BaseT>::value)
			{
				std::cout << "memsetting it" << std::endl << std::endl;
				memset(block, 0, size);
			}
			else
			{
				std::cout << "and recreating new ones" << std::endl << std::endl;
				new(block) BaseT[N]{};
			}
		}
	}
	else if constexpr(std::is_fundamental<typename std::remove_pointer<T>::type>::value ||
					  std::is_same<decltype(block), void*>::value)
	{ 	// pointer to fundamental type, just memset it
		std::cout << "pointer to fundamental type, just memset it" << std::endl << std::endl;
		memset(block, 0, size);
	} 
	else
	{	// pointer to type T, BUT CAN BE AN ARRAY...
		std::cout << "pointer to type T, BUT CAN BE AN ARRAY... ";
		const size_t N = size/sizeof(T);
		if constexpr(!std::is_trivially_destructible<T>::value)
		{	// non trivial destructor means user provided or virtual destructor, see https://en.cppreference.com/w/cpp/language/destructor#Trivial_destructor
			// so we gotta call it manually
			std::cout << "destroying all objects ";
			for(size_t i(0); i < N; ++i)
			{
				block[i].~T();
			}
		}
		if constexpr(std::is_trivially_constructible<T>::value)
		{
			std::cout << "memsetting it" << std::endl << std::endl;
			memset(block, 0, size);
		}
		else
		{
			std::cout << "recreating new ones" << std::endl << std::endl;
			new(block) T[N]{};
		}
	}
	// memset(block, 0, size);
	//assert(mem_is_null(block, size));
}

template<>
[[gnu::warning("pointer type")]]
inline void mem_zero(void *block, size_t size)
{
	typedef void T;
	typedef typename std::remove_all_extents<T>::type BaseT;
	static_assert(std::is_same<T, void>::value || std::is_trivial<T>::value || std::is_standard_layout<T>::value);
	std::cout << "----------------------------------------------------------------------------------" << std::endl
			  << std::left << std::setw(32) << typeid(block).name() << type(block) << std::endl
			  << std::setw(32) << "is_pointer    " << std::is_pointer    <T>::value << std::endl
			  << std::setw(32) << "is_pointer (no extents)" << std::is_pointer<BaseT>::value << std::endl
			  << std::setw(32) << "is_array      " << std::is_array      <T>::value << std::endl
			  << std::setw(32) << "is_fundamental" << std::is_fundamental<T>::value << std::endl
			  << std::setw(32) << "is_fundamental (no extents)" << std::is_fundamental<BaseT>::value << std::endl
			  << std::setw(32) << "is_void       " << std::is_same <T, void>::value << std::endl
			  << std::setw(32) << "is_void (no extents)" << std::is_same<BaseT, void>::value << std::endl
			  << std::setw(32) << "is_void*      " << std::is_same <T, void>::value << std::endl
			  << std::setw(32) << "is_void* (no extents)" << std::is_same<BaseT, void*>::value << std::endl
			  << std::setw(32) << "is_default_cons" << !std::is_default_constructible<T>::value << std::endl
			  << std::setw(32) << "is_default_cons (no extents)" << !std::is_default_constructible<BaseT>::value << std::endl
			  << std::setw(32) << "is_trivial_cons" << !std::is_trivially_constructible<T>::value << std::endl
			  << std::setw(32) << "is_trivial_cons (no extents)" << !std::is_trivially_constructible<BaseT>::value << std::endl
			  << std::setw(32) << "is_destructible" << !std::is_destructible<T>::value << std::endl
			  << std::setw(32) << "is_destructible (no extents)" << !std::is_destructible<BaseT>::value << std::endl
			  << std::setw(32) << "is_trivial_dest" << !std::is_trivially_destructible<T>::value << std::endl
			  << std::setw(32) << "is_trivial_dest (no extents)" << !std::is_trivially_destructible<BaseT>::value << std::endl;

	std::cout << "void pointer, just memset it" << std::endl << std::endl;
	memset(block, 0, size);
	assert(mem_is_null(block, size));
}*/

int main(int argc, char **argv)
{
	std::cout << std::boolalpha;
	{
	void* aVoid[123] = {(void*)1,(void*)2,(void*)3,(void*)4};
	assert(!mem_is_null(aVoid, sizeof(aVoid)));
	mem_zero(&aVoid, sizeof(aVoid));
	assert(mem_is_null(aVoid, sizeof(aVoid)));
	aVoid[0] = aVoid;
	assert(!mem_is_null(aVoid, sizeof(aVoid)));
	mem_zero(aVoid[0], 123*sizeof(void*));
	assert(mem_is_null(aVoid, sizeof(aVoid)));
	aVoid[0] = aVoid;
	assert(!mem_is_null(aVoid, sizeof(aVoid)));
	mem_zero(&aVoid[0], 123*sizeof(void*));
	assert(mem_is_null(aVoid, sizeof(aVoid)));

	void* aVoid2[123] = {(void*)1,(void*)2,(void*)3,(void*)4};
	assert(!mem_is_null(aVoid2, sizeof(aVoid2)));
	mem_zero(aVoid2, sizeof(aVoid2));
	assert(mem_is_null(aVoid2, sizeof(aVoid2)));

	int m_aTest[512] = {1,2,3,4,56};
	assert(!mem_is_null(m_aTest, sizeof(m_aTest)));
	mem_zero(&m_aTest, sizeof(m_aTest));
	assert(mem_is_null(m_aTest, sizeof(m_aTest)));
	m_aTest[2] = 42;
	assert(!mem_is_null(m_aTest, sizeof(m_aTest)));
	mem_zero(&m_aTest[0], sizeof(m_aTest));
	assert(mem_is_null(m_aTest, sizeof(m_aTest)));

	int m_aTest2[512] = {1,2,3,4,56};
	assert(!mem_is_null(m_aTest2, sizeof(m_aTest2)));
	mem_zero(m_aTest2, sizeof(m_aTest2));
	assert(mem_is_null(m_aTest2, sizeof(m_aTest2)));

	int* m_apTest[512] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)6,(int*)6};
	assert(!mem_is_null(m_apTest, sizeof(m_apTest)));
	mem_zero(&m_apTest, sizeof(m_apTest));
	assert(mem_is_null(m_apTest, sizeof(m_apTest)));

	int* m_apTest2[512] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)6,(int*)6};
	assert(!mem_is_null(m_apTest2, sizeof(m_apTest2)));
	mem_zero(m_apTest2, sizeof(m_apTest2));
	assert(mem_is_null(m_apTest2, sizeof(m_apTest2)));

	int aaTest[10][20] = {1,2,3,4,5,6,78,9};
	assert(!mem_is_null(aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null(aaTest, sizeof(aaTest)));
	aaTest[0][0] = 42;
	assert(!mem_is_null(aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null(aaTest, sizeof(aaTest)));
	aaTest[0][0] = 42; aaTest[9][19] = 42;
	assert(!mem_is_null(aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null(aaTest, sizeof(aaTest)));

	int aaTest2[10][20] = {1,2,3,4,5,6,78,9};
	assert(!mem_is_null(aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null(aaTest2, sizeof(aaTest2)));
	aaTest2[0][0] = 42; aaTest2[9][19] = 42;
	assert(!mem_is_null(aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null(aaTest2, sizeof(aaTest2)));

	int* aapTest[10][20] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)5,(int*)6,(int*)78,(int*)9};
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest, sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (int*)42; aapTest[9][19] = (int*)42;
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0], sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));

	int* aapTest2[10][20] = {(int*)1,(int*)2,(int*)3,(int*)4,(int*)5,(int*)6,(int*)78,(int*)9};
	assert(!mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2, sizeof(aapTest2));
	assert(mem_is_null(aapTest2, sizeof(aapTest2)));
	aapTest2[0][0] = (int*)42; aapTest2[9][19] = (int*)42;
	assert(!mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2[0], sizeof(aapTest2));
	assert(mem_is_null(aapTest2, sizeof(aapTest2)));
	}

	std::cout << "=======================================================================" << std::endl;
	std::cout << "                          User defined types                           " << std::endl;
	std::cout << "=======================================================================" << std::endl;

	{
	CEntry aTest[2];	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));

	CEntry aTest2[2];	aTest2[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	assert(mem_is_null(aTest2, sizeof(aTest2)));

	CEntry* apTest[123] = {(CEntry*)1,(CEntry*)2,(CEntry*)3,(CEntry*)4,(CEntry*)6,(CEntry*)6};
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero((CEntry**)apTest, sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (CEntry*)apTest; apTest[122] = (CEntry*)apTest;
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero(&apTest, sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (CEntry*)apTest; apTest[122] = (CEntry*)apTest;
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero(&apTest[0], sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (CEntry*)apTest; apTest[122] = (CEntry*)apTest;;
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero(apTest, sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	
	std::cout << "2D arrays_____________________________________________" << std::endl;
	CEntry aaTest[2][2] = {1,2,3,4};
	assert(!mem_is_null((CEntry*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null((CEntry*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null((CEntry*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null((CEntry*)aaTest, sizeof(aaTest)));

	CEntry aaTest2[2][2] = {1,2,3,4};
	assert(!mem_is_null((CEntry*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null((CEntry*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].m_NumAddrs = 42; aaTest2[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null((CEntry*)aaTest2, sizeof(aaTest2)));

	std::cout << "2D pointer arrays_____________________________________________" << std::endl;
	CEntry* aapTest[10][20] = {(CEntry*)1,(CEntry*)2,(CEntry*)3,(CEntry*)4,(CEntry*)5,(CEntry*)6,(CEntry*)78,(CEntry*)9};
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest, sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (CEntry*)42; aapTest[9][19] = (CEntry*)42;
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0], sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (CEntry*)42; aapTest[9][19] = (CEntry*)42;
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0][0], sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));

	CEntry* aapTest2[10][20] = {(CEntry*)1,(CEntry*)2,(CEntry*)3,(CEntry*)4,(CEntry*)5,(CEntry*)6,(CEntry*)78,(CEntry*)9};
	assert(!mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2, sizeof(aapTest2));
	assert(mem_is_null(aapTest2, sizeof(aapTest2)));
	aapTest2[0][0] = (CEntry*)42; aapTest2[9][19] = (CEntry*)42;
	assert(!mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2[0], sizeof(aapTest2));
	assert(mem_is_null(aapTest2, sizeof(aapTest2)));
	}

	std::cout << "=======================================================================" << std::endl;
	std::cout << "                          User defined types                           " << std::endl;
	std::cout << "=======================================================================" << std::endl;

	{
	CEntry2 aTest[2];	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));

	CEntry2 aTest2[2];	aTest2[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	assert(mem_is_null(aTest2, sizeof(aTest2)));

	CEntry2* apTest[123] = {(CEntry2*)1,(CEntry2*)2,(CEntry2*)3,(CEntry2*)4,(CEntry2*)6,(CEntry2*)6};
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero((CEntry2**)apTest, sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (CEntry2*)apTest; apTest[122] = (CEntry2*)apTest;
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero(&apTest, sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (CEntry2*)apTest; apTest[122] = (CEntry2*)apTest;
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero(&apTest[0], sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	apTest[0] = (CEntry2*)apTest; apTest[122] = (CEntry2*)apTest;;
	assert(!mem_is_null(apTest, sizeof(apTest)));
	mem_zero(apTest, sizeof(apTest));
	assert(mem_is_null(apTest, sizeof(apTest)));
	
	std::cout << "2D arrays_____________________________________________" << std::endl;
	CEntry2 aaTest[2][2] = {1,2,3,4};
	assert(!mem_is_null((CEntry2*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null((CEntry2*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry2*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null((CEntry2*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry2*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null((CEntry2*)aaTest, sizeof(aaTest)));

	CEntry2 aaTest2[2][2] = {1,2,3,4};
	assert(!mem_is_null((CEntry2*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null((CEntry2*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].m_NumAddrs = 42; aaTest2[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry2*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null((CEntry2*)aaTest2, sizeof(aaTest2)));

	std::cout << "2D pointer arrays_____________________________________________" << std::endl;
	CEntry2* aapTest[10][20] = {(CEntry2*)1,(CEntry2*)2,(CEntry2*)3,(CEntry2*)4,(CEntry2*)5,(CEntry2*)6,(CEntry2*)78,(CEntry2*)9};
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest, sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (CEntry2*)42; aapTest[9][19] = (CEntry2*)42;
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0], sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));
	aapTest[0][0] = (CEntry2*)42; aapTest[9][19] = (CEntry2*)42;
	assert(!mem_is_null(aapTest, sizeof(aapTest)));
	mem_zero(&aapTest[0][0], sizeof(aapTest));
	assert(mem_is_null(aapTest, sizeof(aapTest)));

	CEntry2* aapTest2[10][20] = {(CEntry2*)1,(CEntry2*)2,(CEntry2*)3,(CEntry2*)4,(CEntry2*)5,(CEntry2*)6,(CEntry2*)78,(CEntry2*)9};
	assert(!mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2, sizeof(aapTest2));
	assert(mem_is_null(aapTest2, sizeof(aapTest2)));
	aapTest2[0][0] = (CEntry2*)42; aapTest2[9][19] = (CEntry2*)42;
	assert(!mem_is_null(aapTest2, sizeof(aapTest2)));
	mem_zero(aapTest2[0], sizeof(aapTest2));
	assert(mem_is_null(aapTest2, sizeof(aapTest2)));
	}

	std::cout << "=======================================================================" << std::endl;
	std::cout << "                          User defined types                           " << std::endl;
	std::cout << "=======================================================================" << std::endl;

	{
	CEntry3 aTest[2];	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));

	CEntry3 aTest2[2];	aTest2[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	assert(mem_is_null(aTest2, sizeof(aTest2)));
	
	std::cout << "2D arrays_____________________________________________" << std::endl;
	CEntry3 aaTest[2][2] = {1,2,3,4};
	assert(!mem_is_null((CEntry3*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null((CEntry3*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry3*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null((CEntry3*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry3*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null((CEntry3*)aaTest, sizeof(aaTest)));

	CEntry3 aaTest2[2][2] = {1,2,3,4};
	assert(!mem_is_null((CEntry3*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null((CEntry3*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].m_NumAddrs = 42; aaTest2[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry3*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null((CEntry3*)aaTest2, sizeof(aaTest2)));
	}

	std::cout << "=======================================================================" << std::endl;
	std::cout << "                          User defined types                           " << std::endl;
	std::cout << "=======================================================================" << std::endl;

	{
	CEntry4 aTest[2];	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));

	CEntry4 aTest2[2];	aTest2[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	assert(mem_is_null(aTest2, sizeof(aTest2)));
	
	std::cout << "2D arrays_____________________________________________" << std::endl;
	CEntry4 aaTest[2][2] = {1,2};
	assert(!mem_is_null((CEntry4*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null((CEntry4*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry4*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null((CEntry4*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry4*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null((CEntry4*)aaTest, sizeof(aaTest)));

	CEntry4 aaTest2[2][2] = {1,2};
	assert(!mem_is_null((CEntry4*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null((CEntry4*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].m_NumAddrs = 42; aaTest2[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry4*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null((CEntry4*)aaTest2, sizeof(aaTest2)));
	}

	std::cout << "=======================================================================" << std::endl;
	std::cout << "                          User defined types                           " << std::endl;
	std::cout << "=======================================================================" << std::endl;

	/*
	{ // doesn't compile due to no default constructor in call to mem_zero
	CEntry5 Test(1, false);
	assert(!mem_is_null(&Test, sizeof(Test)));
	mem_zero(&Test, sizeof(Test));
	assert(mem_is_null(&Test, sizeof(Test)));
	}*/

	/*
	{ // this doesn't compile anyway :)
	CEntry5 aTest[2] = {{1,false}};	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));

	CEntry5 aTest2[2] = {{1,false}}; aTest2[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	assert(mem_is_null(aTest2, sizeof(aTest2)));
	
	std::cout << "2D arrays_____________________________________________" << std::endl;
	CEntry5 aaTest[2][2] = {{1,2}};
	assert(!mem_is_null((CEntry5*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null((CEntry5*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry5*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null((CEntry5*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry5*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null((CEntry5*)aaTest, sizeof(aaTest)));

	CEntry5 aaTest2[2][2] = {1,2};
	assert(!mem_is_null((CEntry5*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null((CEntry5*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].m_NumAddrs = 42; aaTest2[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry5*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null((CEntry5*)aaTest2, sizeof(aaTest2)));
	}*/

	{
	CEntry6 aTest[2] = {{1,false}};	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest, sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));
	aTest[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest, sizeof(aTest)));
	mem_zero(&aTest[0], sizeof(aTest));
	assert(mem_is_null(aTest, sizeof(aTest)));

	CEntry6 aTest2[2] = {{1,false}}; aTest2[0].m_NumAddrs = 42;
	assert(!mem_is_null(aTest2, sizeof(aTest2)));
	mem_zero(aTest2, sizeof(aTest2));
	assert(mem_is_null(aTest2, sizeof(aTest2)));
	
	std::cout << "2D arrays_____________________________________________" << std::endl;
	CEntry6 aaTest[2][2] = {{{1,false}}};
	assert(!mem_is_null((CEntry6*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest, sizeof(aaTest));
	assert(mem_is_null((CEntry6*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry6*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0], sizeof(aaTest));
	assert(mem_is_null((CEntry6*)aaTest, sizeof(aaTest)));
	aaTest[0][0].m_NumAddrs = 42; aaTest[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry6*)aaTest, sizeof(aaTest)));
	mem_zero(&aaTest[0][0], sizeof(aaTest));
	assert(mem_is_null((CEntry6*)aaTest, sizeof(aaTest)));

	CEntry6 aaTest2[2][2] = {{{1,2}}};
	assert(!mem_is_null((CEntry6*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2, sizeof(aaTest2));
	assert(mem_is_null((CEntry6*)aaTest2, sizeof(aaTest2)));
	aaTest2[0][0].m_NumAddrs = 42; aaTest2[1][1].m_NumAddrs = 42;
	assert(!mem_is_null((CEntry6*)aaTest2, sizeof(aaTest2)));
	mem_zero(aaTest2[0], sizeof(aaTest));
	assert(mem_is_null((CEntry6*)aaTest2, sizeof(aaTest2)));
	}

	{
	CClient *pClient = static_cast<CClient *>(malloc(sizeof(*pClient)));
	mem_zero(pClient, sizeof(CClient));
	}

	return 0;
}
