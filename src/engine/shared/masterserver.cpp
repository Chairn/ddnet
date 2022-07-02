#include "masterserver.h"

const unsigned char g_aServerBrowseGetInfo[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'g', 'i', 'e', '3'};
const unsigned char g_aServerBrowseInfo[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'i', 'n', 'f', '3'};

const unsigned char g_aServerBrowseGetInfo64Legacy[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'f', 's', 't', 'd'};
const unsigned char g_aServerBrowseInfo64Legacy[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'd', 't', 's', 'f'};

const unsigned char g_aServerBrowseInfoExtended[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'i', 'e', 'x', 't'};
const unsigned char g_aServerBrowseInfoExtendedMore[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'i', 'e', 'x', '+'};

const unsigned char g_aServerBrowseChallenge[SERVERBROWSE_SIZE] = {255, 255, 255, 255, 'c', 'h', 'a', 'l'};
