/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_MASTERSERVER_H
#define ENGINE_SHARED_MASTERSERVER_H

#define SERVERBROWSE_SIZE 8
extern const unsigned char g_aServerBrowseGetInfo[SERVERBROWSE_SIZE];
extern const unsigned char g_aServerBrowseInfo[SERVERBROWSE_SIZE];

extern const unsigned char g_aServerBrowseGetInfo64Legacy[SERVERBROWSE_SIZE];
extern const unsigned char g_aServerBrowseInfo64Legacy[SERVERBROWSE_SIZE];

extern const unsigned char g_aServerBrowseInfoExtended[SERVERBROWSE_SIZE];
extern const unsigned char g_aServerBrowseInfoExtendedMore[SERVERBROWSE_SIZE];

extern const unsigned char g_aServerBrowseChallenge[SERVERBROWSE_SIZE];

enum
{
	SERVERINFO_VANILLA = 0,
	SERVERINFO_64_LEGACY,
	SERVERINFO_EXTENDED,
	SERVERINFO_EXTENDED_MORE,
	SERVERINFO_INGAME,
};
#endif // ENGINE_SHARED_MASTERSERVER_H
