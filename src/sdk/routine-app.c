#include "routine.h"

// Override the feed for the SDK's automatic and manual update checks.
#define _r_app_getupdate_url() L"https://raw.githubusercontent.com/Mainkill1/simplewall/master/VERSION.fork"

// The public SDK still looks for the language-pack timestamp in [Russian].
static DWORD read_profile_string(LPCWSTR section, LPCWSTR key, LPCWSTR fallback,
	LPWSTR buffer, DWORD count, LPCWSTR path)
{
	DWORD length = GetPrivateProfileStringW(section, key, fallback, buffer, count, path);

	if (!length && section && key && lstrcmpW(section, L"Russian") == 0 && lstrcmpW(key, L"000") == 0)
		length = GetPrivateProfileStringW(L"\u0420\u0443\u0441\u0441\u043A\u0438\u0439", key, fallback, buffer, count, path);

	return length;
}

#define GetPrivateProfileStringW read_profile_string
#include <rapp.c>
