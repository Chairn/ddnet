/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_LOCALIZATION_H
#define GAME_LOCALIZATION_H

#include <base/system.h> // GNUC_ATTRIBUTE
#include <engine/shared/memheap.h>
#include <vector>

class CLocalizationDatabase
{
	class CString
	{
	public:
		unsigned m_Hash = 0;
		unsigned m_ContextHash = 0;
		const char *m_pReplacement = nullptr;

		CString() {}
		CString(unsigned Hash, unsigned ContextHash, const char *pReplacement) :
			m_Hash(Hash), m_ContextHash(ContextHash), m_pReplacement(pReplacement)
		{
		}

		bool operator<(const CString &Other) const { return m_Hash < Other.m_Hash || (m_Hash == Other.m_Hash && m_ContextHash < Other.m_ContextHash); }
		bool operator<=(const CString &Other) const { return m_Hash < Other.m_Hash || (m_Hash == Other.m_Hash && m_ContextHash <= Other.m_ContextHash); }
		bool operator==(const CString &Other) const { return m_Hash == Other.m_Hash && m_ContextHash == Other.m_ContextHash; }
	};

	std::vector<CString> m_vStrings;
	CHeap m_StringsHeap;
	int m_VersionCounter = 0;
	int m_CurrentVersion = 0;

public:
	CLocalizationDatabase();

	bool Load(const char *pFilename, class IStorage *pStorage, class IConsole *pConsole);

	int Version() const { return m_CurrentVersion; }

	void AddString(const char *pOrgStr, const char *pNewStr, const char *pContext);
	const char *FindString(unsigned Hash, unsigned ContextHash) const;
};

extern CLocalizationDatabase g_Localization;

class CLocConstString
{
	const char *m_pDefaultStr = nullptr;
	const char *m_pCurrentStr = nullptr;
	unsigned m_Hash = 0;
	unsigned m_ContextHash = 0;
	int m_Version = 0;

public:
	CLocConstString(const char *pStr, const char *pContext = "");
	void Reload();

	inline operator const char *()
	{
		if(m_Version != g_Localization.Version())
			Reload();
		return m_pCurrentStr;
	}
};

extern const char *Localize(const char *pStr, const char *pContext = "")
	GNUC_ATTRIBUTE((format_arg(1)));
#endif
