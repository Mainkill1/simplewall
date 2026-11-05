// Exercise the adapter ABI against real Windows version resources and SIDs.
#include <windows.h>
#include <sddl.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

// Public SDK signatures and output initialization, before application mappings.
static BOOL _r_res_queryversion (LPCVOID block, PVOID *output)
{
    UINT length = 0;
    *output = NULL;
    return VerQueryValueW (block, L"\\", output, &length);
}

static LONG _r_str_fromsid (PSID sid, LPWSTR *output)
{
    return ConvertSidToStringSidW (sid, output) ? 0 : -1;
}

#define INT_ERROR -1
#define I_DEFAULT -8

// Public SDK insertion forwards the row index verbatim to the native control.
static INT _r_listview_getitemcount(HWND window, INT id)
{
    return (INT)SendMessageW(id ? GetDlgItem(window,id) : window,LVM_GETITEMCOUNT,0,0);
}

static INT _r_listview_additem(HWND window, INT id, INT index, LPWSTR text, INT image, INT group, LPARAM context)
{
    LVITEMW item = {0};
    item.iItem=index;
    if(text){item.mask|=LVIF_TEXT;item.pszText=text;}
    if(image!=I_DEFAULT){item.mask|=LVIF_IMAGE;item.iImage=image;}
    if(group!=I_DEFAULT){item.mask|=LVIF_GROUPID;item.iGroupId=group;}
    if(context!=I_DEFAULT){item.mask|=LVIF_PARAM;item.lParam=context;}
    return (INT)SendMessageW(id ? GetDlgItem(window,id) : window,LVM_INSERTITEMW,0,(LPARAM)&item);
}

#include "sdk-adapter-mappings.h"

int main (void)
{
    WCHAR path[MAX_PATH];
    DWORD ignored, length, protection;
    VS_FIXEDFILEINFO *version = NULL;
    PVOID block;
    PSID sid = NULL;
    LPWSTR text = NULL;
    int result = 1;
    INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_LISTVIEW_CLASSES};
    HWND list = NULL;

    GetSystemDirectoryW (path, MAX_PATH);
    wcscat_s (path, MAX_PATH, L"\\kernel32.dll");
    length = GetFileVersionInfoSizeW (path, &ignored);
    if (!length)
        return 2;
    block = VirtualAlloc (NULL, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!block)
        return 2;
    if (!GetFileVersionInfoW (path, 0, length, block) ||
        !VirtualProtect (block, length, PAGE_READONLY, &protection))
        goto Cleanup;

    __try
    {
        if (!_r_res_queryversion ((PVOID*)&version, block) ||
            !version || version->dwSignature != VS_FFI_SIGNATURE)
        {
            puts ("FAIL: invalid version query output");
            goto Cleanup;
        }
        if (!ConvertStringSidToSidW (L"S-1-5-18", &sid) ||
            _r_str_fromsid (&text, sid) != 0 || !text || wcscmp (text, L"S-1-5-18"))
        {
            puts ("FAIL: invalid SID conversion output");
            goto Cleanup;
        }
        if(!InitCommonControlsEx(&controls)) goto Cleanup;
        list=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_POPUP|LVS_REPORT,0,0,200,100,NULL,NULL,GetModuleHandleW(NULL),NULL);
        if(!list) goto Cleanup;
        if(_r_listview_additem(list,0,INT_ERROR,L"Packet 1",I_DEFAULT,I_DEFAULT,42)!=0 ||
           _r_listview_additem(list,0,INT_ERROR,L"Packet 2",I_DEFAULT,I_DEFAULT,43)!=1 ||
           _r_listview_getitemcount(list,0)!=2) {
            puts("FAIL: append index rejected by native list view");goto Cleanup;
        }
        if(_r_listview_additem(list,0,0,L"First",I_DEFAULT,I_DEFAULT,44)!=0 ||
           _r_listview_getitemcount(list,0)!=3) {
            puts("FAIL: explicit insertion index changed");goto Cleanup;
        }
        LVITEMW first = {0};first.mask=LVIF_PARAM;first.iItem=0;
        if(!SendMessageW(list,LVM_GETITEMW,0,(LPARAM)&first) || first.lParam!=44) {
            puts("FAIL: insertion lost item context");goto Cleanup;
        }
        puts ("PASS: protected version query, SID conversion and native list append");
        result = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf ("FAIL: adapter raised exception 0x%08lX\n", GetExceptionCode ());
    }

Cleanup:
    if (list) DestroyWindow(list);
    if (text) LocalFree (text);
    if (sid) LocalFree (sid);
    VirtualFree (block, 0, MEM_RELEASE);
    return result;
}
