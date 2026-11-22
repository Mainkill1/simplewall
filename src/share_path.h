// Recognize file paths reported by the Windows UNC redirectors.
#pragma once

#include <wchar.h>

static inline const wchar_t *sw_skip_share_prefix(const wchar_t *path, const wchar_t *prefix)
{
    const wchar_t *next = path;
    while (*prefix)
    {
        wchar_t ch = *next++;
        if (ch >= L'A' && ch <= L'Z')
            ch += L'a' - L'A';
        if (ch != *prefix++)
            return NULL;
    }
    return next;
}

static inline const wchar_t *sw_share_prefix_tail(const wchar_t *path)
{
    const wchar_t *tail;

    if (!path)
        return NULL;

    tail = sw_skip_share_prefix(path, L"\\\\?\\unc\\");
    if (!tail)
        tail = sw_skip_share_prefix(path, L"\\??\\unc\\");
    if (!tail)
    {
        tail = sw_skip_share_prefix(path, L"\\device\\mup\\");
        if (!tail)
            tail = sw_skip_share_prefix(path, L"\\device\\lanmanredirector\\");
    }
    if (!tail)
        tail = sw_skip_share_prefix(path, L"\\\\");

    // Mapped SMB names can carry a provider and an authentication ID. Older
    // profiles can have these tags after a UNC prefix as well.
    if (tail && *tail == L';')
    {
        const wchar_t *provider = sw_skip_share_prefix(tail, L";lanmanredirector\\");
        if (provider)
            tail = provider;
        if (*tail == L';')
        {
            wchar_t drive = tail[1];
            if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) ||
                tail[2] != L':')
                return NULL;

            const wchar_t *id = tail + 3;
            const wchar_t *end = id + wcsspn(id, L"0123456789abcdefABCDEF");
            if (end == id || *end != L'\\')
                return NULL;
            tail = end + 1;
        }
    }
    return tail;
}

static inline const wchar_t *sw_share_path_tail(const wchar_t *path)
{
    const wchar_t *tail = sw_share_prefix_tail(path);
    if (!tail || !*tail || wcspbrk(tail, L"<>:\"/|?*"))
        return NULL;

    const wchar_t *share = wcschr(tail, L'\\');
    if (!share || share == tail || (share == tail + 1 && tail[0] == L'.'))
        return NULL;
    share++;

    const wchar_t *file = wcschr(share, L'\\');
    if (!file || file == share || !file[1] || file[1] == L'\\')
        return NULL;
    if (tail[wcslen(tail) - 1] == L'\\')
        return NULL;

    return tail;
}
