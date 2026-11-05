// Experimental application-side adapter for the public routine 2.7.11 SDK.
// Never force-include this header in routine's own translation units.
#pragma once
#include "routine.h"
#include <mscat.h>

typedef BOOL (WINAPI *CCAHFFH2)(HCATADMIN,HANDLE,DWORD *,BYTE *,DWORD);
typedef BOOL (WINAPI *CCAAC2)(HCATADMIN *,const GUID *,LPCWSTR,PCCERT_STRONG_SIGN_PARA,DWORD);

typedef const R_STRINGREF *PCR_STRINGREF;
typedef const SID *PCSID;

#define _r_config_getboolean _r_config_getboolean_ex
#define _r_config_getlong _r_config_getlong_ex
#define _r_config_getlong64 _r_config_getlong64_ex
#define _r_config_getulong _r_config_getulong_ex
#define _r_config_getulong64 _r_config_getulong64_ex
#define _r_config_getfont _r_config_getfont_ex
#define _r_config_getstringexpand _r_config_getstringexpand_ex
#define _r_config_getstring _r_config_getstring_ex
#define _r_config_setboolean _r_config_setboolean_ex
#define _r_config_setlong _r_config_setlong_ex
#define _r_config_setlong64 _r_config_setlong64_ex
#define _r_config_setulong _r_config_setulong_ex
#define _r_config_setulong64 _r_config_setulong64_ex
#define _r_config_setfont _r_config_setfont_ex
#define _r_config_setstringexpand _r_config_setstringexpand_ex
#define _r_config_setstring _r_config_setstring_ex
#define _r_obj_addlistitem _r_obj_addlistitem_ex

// API reordering: the current application consistently places output first.
#define _r_fs_openfile(out,path,access,share,options,dir) _r_fs_openfile((PR_STRINGREF)(path),access,share,options,dir,out)
#define _r_fs_createfile(out,path,disposition,access,share,attributes,options,dir,size) _r_fs_createfile((PR_STRINGREF)(path),disposition,access,share,attributes,options,dir,size,out)
#define _r_reg_openkey(out,root,path,flags,access) _r_reg_openkey(root,(LPWSTR)(path),flags,access,out)
#define _r_sys_loadicon(out,module,name,size) _r_sys_loadicon(module,name,size,out)
#define _r_res_loadimage(out,module,type,name,format,width,height) _r_res_loadimage(module,type,name,format,width,height,out)
#define _r_res_loadresource(out,module,type,name,language) _r_res_loadresource(module,type,name,language,out)
#define _r_res_queryversion(out,block) _r_res_queryversion(block,out)
#define _r_imagelist_create(out,width,height,flags,count,grow) _r_imagelist_create(width,height,flags,count,grow,out)
#define _r_imagelist_getsystem(out,size) _r_imagelist_getsystem(size,out)
#define _r_imagelist_setsize(list,width,height) IImageList2_SetIconSize((IImageList2 *)(list),width,height)
#define _r_sys_openprocess(out,pid,access) _r_sys_openprocess(ULongToHandle(pid),access,out)
#define _r_sys_queryprocessstring(out,process,kind) _r_sys_queryprocessstring(process,kind,out)
#define _r_sys_querytokeninformation(out,token,kind) _r_sys_querytokeninformation(token,kind,out)
#define _r_sys_getservicesid(out,name) _r_sys_getservicesid((LPWSTR)(name),out)
#define _r_sys_getusername(out,sid,domain) _r_sys_getusername((PSID)(sid),domain,out)
#define _r_sys_getprocessimagepathbyid(out,pid,dos) _r_sys_getprocessimagepathbyid(ULongToHandle(pid),dos,out)
#define _r_sys_querytaginformation(pid,tag) _r_sys_querytaginformation(ULongToHandle(pid),tag)
#define _r_sys_loadlibrary2(out,name,flags) _r_sys_loadlibrary2((LPWSTR)(name),flags,(PVOID_PTR)(out))
#define _r_sys_loadlibraryasresource(out,name) _r_sys_loadlibraryasresource((PR_STRINGREF)(name),(PVOID_PTR)(out))
#define _r_sys_compressbuffer(out,format,buffer) _r_sys_compressbuffer(format,buffer,out)
// The old SDK stops after six growth attempts, rejecting small highly
// compressed profiles. Grow to its existing hard limit instead, checking
// input length before narrowing and never returning a partial output buffer.
static inline NTSTATUS sw_compat_decompress(PR_BYTE_PTR output,USHORT format,PR_BYTEREF input)
{
    *output=NULL;
    if (!input || !input->buffer || !input->length) return STATUS_NO_DATA_DETECTED;
    if (input->length>PR_SIZE_BUFFER_OVERFLOW) return STATUS_BUFFER_OVERFLOW;
    ULONG capacity=(ULONG)input->length;
    if (capacity<4096) capacity=4096;
    for (;;)
    {
        PR_BYTE result=_r_obj_createbyte_ex(NULL,capacity);
        ULONG returned=0;
        NTSTATUS status=RtlDecompressBuffer(format,(PUCHAR)result->buffer,capacity,(PUCHAR)input->buffer,(ULONG)input->length,&returned);
        // LZNT1 can return success at a complete 4 KiB chunk boundary even
        // when more chunks remain. A completely filled buffer is inconclusive.
        if (NT_SUCCESS(status) && returned<capacity)
        {
            result->length=returned; *output=result;
            return status;
        }
        _r_obj_dereference(result);
        if (!NT_SUCCESS(status) && status!=STATUS_BAD_COMPRESSION_BUFFER && status!=STATUS_BUFFER_TOO_SMALL) return status;
        if (capacity==PR_SIZE_BUFFER_OVERFLOW) return STATUS_BUFFER_OVERFLOW;
        capacity=capacity>PR_SIZE_BUFFER_OVERFLOW/2?PR_SIZE_BUFFER_OVERFLOW:capacity*2;
    }
}
#define _r_sys_decompressbuffer sw_compat_decompress
static inline NTSTATUS sw_compat_filehash(PR_STRING_PTR output,LPCWSTR algorithm,PCR_STRINGREF path,HANDLE file)
{
    *output=NULL;
    return _r_crypt_getfilehash(algorithm,(PR_STRINGREF)path,file,output);
}
#define _r_crypt_getfilehash sw_compat_filehash
#define _r_str_environmentexpandstring(out,environment,name) _r_str_environmentexpandstring(environment,(PR_STRINGREF)(name),out)
#define _r_str_fromguid(out,guid,upper) _r_str_fromguid((LPGUID)(guid),upper,out)
#define _r_str_fromsid(out,sid) _r_str_fromsid((PSID)(sid),out)
#define _r_unixtime_to_filetime(out,time) _r_unixtime_to_filetime(time,out)
#define _r_calc_filetime2largeinteger(out,time) _r_calc_filetime2largeinteger(time,out)
#define _r_path_geticon(path,icon,index) _r_path_geticon((PR_STRINGREF)(path),index,icon)

// The two string hash entry points exchanged names, not algorithms.
static inline ULONG sw_compat_hash_ref(PCR_STRINGREF value, BOOLEAN ignorecase) { return _r_str_gethash2((PR_STRINGREF)value,ignorecase); }
static inline ULONG sw_compat_hash_z(LPCWSTR value, BOOLEAN ignorecase) { return _r_str_gethash((LPWSTR)value,ignorecase); }
#define _r_str_gethash sw_compat_hash_ref
#define _r_str_gethash2 sw_compat_hash_z

static inline BOOLEAN sw_compat_enum(PR_HASHTABLE table, PVOID_PTR value, PULONG hash, PULONG_PTR cursor, BOOLEAN pointer)
{
    ULONG_PTR full_hash = 0;
    BOOLEAN found = pointer ? _r_obj_enumhashtablepointer(table,value,&full_hash,cursor) : _r_obj_enumhashtable(table,value,&full_hash,cursor);
    if (found && hash) *hash = (ULONG)full_hash;
    return found;
}
#define _r_obj_enumhashtable(t,v,h,c) sw_compat_enum(t,v,h,c,FALSE)
#define _r_obj_enumhashtablepointer(t,v,h,c) sw_compat_enum(t,v,h,c,TRUE)

static inline LPWSTR sw_compat_basename(LPCWSTR path)
{
    R_STRINGREF ref; _r_obj_initializestringref(&ref,(LPWSTR)path);
    return _r_path_getbasename(&ref);
}
#define _r_path_getbasename2 sw_compat_basename
static inline BOOLEAN sw_compat_exists(PCR_STRINGREF path)
{
    return _r_fs_exists((PR_STRINGREF)path);
}
#define _r_fs_isexists sw_compat_exists
static inline LONG sw_compat_drive(PCR_STRINGREF path)
{
    if (path->length < 2 * sizeof(WCHAR) || path->buffer[1] != L':') return INT_ERROR;
    WCHAR letter = path->buffer[0];
    if (letter >= L'a' && letter <= L'z') return letter-L'a';
    return letter >= L'A' && letter <= L'Z' ? letter-L'A' : INT_ERROR;
}
#define _r_path_getdrivenumber sw_compat_drive
#define _r_path_isnetwork(path) ((path)->length >= 2 * sizeof(WCHAR) && (path)->buffer[0] == L'\\' && (path)->buffer[1] == L'\\')
#define _r_obj_isbyteempty(value) (!(value) || !(value)->length)
#define _r_theme_initialize(hwnd) _r_theme_initialize(hwnd,_r_theme_isenabled())
#define _r_format_interval(seconds) _r_format_interval(seconds,FALSE)
// Raise/lower within the ordinary Z order without changing topmost state.
static inline VOID sw_compat_topzorder(HWND hwnd, BOOLEAN raise)
{
    SetWindowPos(hwnd,raise?HWND_TOP:HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
#define _r_wnd_topzoder sw_compat_topzorder
#define _r_wnd_sendcommand _r_ctrl_sendcommand
#define _r_ctrl_seticon2(hwnd,id,icon) _r_wnd_sendmessage(hwnd,id,STM_SETICON,(WPARAM)(icon),0)
#define _r_ctrl_setselection(hwnd,id,start,end) _r_wnd_sendmessage(hwnd,id,EM_SETSEL,start,end)
// Control helper renames in the August 2026 application source.
#define _r_button_setcheck _r_ctrl_checkbutton
#define _r_button_checkradio _r_ctrl_checkradio
#define _r_button_ischecked _r_ctrl_isbuttonchecked
#define _r_button_isradiochecked _r_ctrl_isradiochecked
#define _r_button_seticon _r_ctrl_seticon
#define _r_button_setmargins _r_ctrl_setbuttonmargins
#define _r_tooltip_create _r_ctrl_createtip
#define _r_tooltip_settext _r_ctrl_settiptext
#define _r_edit_setmargin _r_ctrl_settextmargin
#define _r_edit_setreadonly _r_ctrl_setreadonly
#define _r_edit_showballoontip _r_ctrl_showballoontip
#define _r_edit_setselection(hwnd,id,start,end) _r_wnd_sendmessage(hwnd,id,EM_SETSEL,start,end)
#define _r_updown_setacceleration _r_ctrl_setacceleration
#define _r_menu_addseparator(menu) AppendMenuW(menu,MF_SEPARATOR,0,NULL)
#define _r_rebar_getinfo(hwnd,id,index,info) _r_wnd_sendmessage(hwnd,id,RB_GETBANDINFOW,index,(LPARAM)(info))
#define _r_rebar_setinfo(hwnd,id,index,info) _r_wnd_sendmessage(hwnd,id,RB_SETBANDINFOW,index,(LPARAM)(info))
#define _r_toolbar_getidealsize(hwnd,id,height,size) _r_wnd_sendmessage(hwnd,id,TB_GETIDEALSIZE,height,(LPARAM)(size))
#define _r_sys_settimer SetTimer
#define _r_sys_terminatethread NtTerminateThread
#define GENERAL_ID 0x53575554U
// Retain the public SDK's maximum accepted buffer bound for this build.
#define PR_SIZE_BUFFER_MINIMUM PR_SIZE_BUFFER_OVERFLOW
#ifndef IN6_IS_ADDR_ULA
#define IN6_IS_ADDR_ULA(address) ((((const BYTE *)(address))[0] & 0xfe) == 0xfc)
#endif

static inline BOOLEAN sw_compat_setcontext(HWND hwnd, ULONG property, PVOID value)
{
    _r_wnd_setcontext(hwnd,property,value);
    return _r_wnd_getcontext(hwnd,property) == value;
}
static inline BOOLEAN sw_compat_removecontext(HWND hwnd, ULONG property)
{
    _r_wnd_removecontext(hwnd,property);
    return _r_wnd_getcontext(hwnd,property) == NULL;
}
#define _r_wnd_setcontext sw_compat_setcontext
#define _r_wnd_removecontext sw_compat_removecontext
static inline INT sw_compat_selecttab(HWND hwnd, INT id, INT index)
{
    _r_tab_selectitem(hwnd,id,index);
    return (INT)_r_wnd_sendmessage(hwnd,id,TCM_GETCURSEL,0,0);
}
#define _r_tab_selectitem sw_compat_selecttab
static inline NTSTATUS sw_compat_setpos(HANDLE file, LONG64 value)
{
    LARGE_INTEGER pos; pos.QuadPart=value; return _r_fs_setpos(file,&pos);
}
#define _r_fs_setpos sw_compat_setpos
static inline NTSTATUS sw_compat_filesize(PR_STRINGREF path, HANDLE file, LONG64 *value)
{
    LARGE_INTEGER size; NTSTATUS status = _r_fs_getsize(path,file,&size);
    if (NT_SUCCESS(status)) *value=size.QuadPart;
    return status;
}
#define _r_fs_getsize sw_compat_filesize

typedef VOID (NTAPI *SW_COMPAT_THREAD_CALLBACK)(PVOID);
typedef struct { SW_COMPAT_THREAD_CALLBACK callback; PVOID argument; } SW_COMPAT_THREAD;
static NTSTATUS NTAPI sw_compat_thread_entry(PVOID parameter)
{
    SW_COMPAT_THREAD context = *(SW_COMPAT_THREAD *)parameter;
    _r_mem_free(parameter);
    context.callback(context.argument);
    return STATUS_SUCCESS;
}
static inline NTSTATUS sw_compat_createthread(PHANDLE output,HANDLE process,SW_COMPAT_THREAD_CALLBACK callback,PVOID argument,PR_ENVIRONMENT environment,LPCWSTR name)
{
    if (process != NtCurrentProcess()) return STATUS_NOT_SUPPORTED;
    SW_COMPAT_THREAD *context=(SW_COMPAT_THREAD *)_r_mem_allocate(sizeof(SW_COMPAT_THREAD));
    context->callback=callback; context->argument=argument;
    NTSTATUS status=_r_sys_createthread(output,process,sw_compat_thread_entry,context,environment,name);
    if (!NT_SUCCESS(status)) _r_mem_free(context);
    return status;
}
#define _r_sys_createthread sw_compat_createthread
#define _r_sys_setprocessprivilege(hwnd,process,privileges,count,enable) _r_sys_setprocessprivilege(process,privileges,count,enable)

// Read-only string parameters acquired const annotations in the newer SDK.
#define _r_obj_initializestringref(ref,string) _r_obj_initializestringref(ref,(LPWSTR)(string))
#define _r_str_copystring(out,size,input) _r_str_copystring(out,size,(PR_STRINGREF)(input))
#define _r_str_getlength2(input) _r_str_getlength2((PR_STRINGREF)(input))
#define _r_str_splitatchar(input,ch,left,right) _r_str_splitatchar((PR_STRINGREF)(input),ch,left,right)
#define _r_str_toulong(input) _r_str_toulong((PR_STRINGREF)(input))
#define _r_str_versioncompare(left,right) _r_str_versioncompare((PR_STRINGREF)(left),(PR_STRINGREF)(right))
#define _r_menu_setitemtext(menu,id,position,text) _r_menu_setitemtext(menu,id,position,(LPWSTR)(text))
#define _r_toolbar_setbutton(hwnd,id,command,text,style,state,image) _r_toolbar_setbutton(hwnd,id,command,(LPWSTR)(text),style,state,(image)==I_DEFAULT?I_IMAGENONE:(image))

static inline VOID sw_compat_dialogpath(PR_FILE_DIALOG dialog,PCR_STRINGREF path)
{
    PR_STRING copy=_r_obj_createstring2((PR_STRINGREF)path);
    _r_filedialog_setpath(dialog,copy->buffer);
    _r_obj_dereference(copy);
}
#define _r_filedialog_setpath sw_compat_dialogpath
#define _r_listview_setstyle(hwnd,id,style,groups) _r_listview_setstyle(hwnd,id,(ULONG)(style),groups)
static inline PR_STRINGREF sw_compat_imagepath(void)
{
    static R_INITONCE once=PR_INITONCE_INIT;
    static R_STRINGREF path;
    if (_r_initonce_begin(&once))
    {
        _r_obj_initializestringref(&path,_r_sys_getimagepath());
        _r_initonce_end(&once);
    }
    return &path;
}
#define _r_sys_getimagepath sw_compat_imagepath
static inline VOID sw_compat_updatecomponent(LPCWSTR name,LPCWSTR short_name,LPCWSTR version,PCR_STRINGREF path,BOOLEAN installer)
{
    PR_STRING copy=_r_obj_createstring2((PR_STRINGREF)path);
    _r_update_addcomponent(name,short_name,version,copy,installer);
    _r_obj_dereference(copy);
}
#define _r_update_addcomponent sw_compat_updatecomponent
static inline NTSTATUS sw_compat_backup(PCR_STRINGREF path,BOOLEAN remove_source)
{
    PR_STRING copy=_r_obj_createstring2((PR_STRINGREF)path);
    NTSTATUS status=_r_path_makebackup(copy,remove_source);
    _r_obj_dereference(copy);
    return status;
}
#define _r_path_makebackup sw_compat_backup
static inline BOOLEAN sw_compat_invertboolean(LPCWSTR key,BOOLEAN fallback,LPCWSTR section)
{
    BOOLEAN value=!_r_config_getboolean_ex(key,fallback,section);
    _r_config_setboolean_ex(key,value,section);
    return value;
}
#define _r_config_invertboolean sw_compat_invertboolean
static inline NTSTATUS sw_compat_createprocess(PCR_STRINGREF file,PCR_STRINGREF command,PCR_STRINGREF directory,BOOLEAN wait)
{
    PR_STRING f=file?_r_obj_createstring2((PR_STRINGREF)file):NULL;
    PR_STRING c=command?_r_obj_createstring2((PR_STRINGREF)command):NULL;
    PR_STRING d=directory?_r_obj_createstring2((PR_STRINGREF)directory):NULL;
    NTSTATUS status=_r_sys_createprocess(f?f->buffer:NULL,c?c->buffer:NULL,d?d->buffer:NULL,wait);
    if (f) _r_obj_dereference(f);
    if (c) _r_obj_dereference(c);
    if (d) _r_obj_dereference(d);
    return status;
}
#define _r_sys_createprocess sw_compat_createprocess
static inline INT sw_compat_menupopup(HMENU menu,HWND hwnd,PPOINT point,LPARAM parameter)
{
    INT command=_r_menu_popup(menu,hwnd,point,FALSE);
    if (command) _r_ctrl_sendcommand(hwnd,command,parameter);
    return command;
}
#define _r_menu_popup sw_compat_menupopup
// The current SDK accepts INT_ERROR as append; the public SDK requires an index.
#define _r_listview_additem(hwnd,id,index,text,image,group,context) _r_listview_additem(hwnd,id,((index)==INT_ERROR?_r_listview_getitemcount(hwnd,id):(index)),(LPWSTR)(text),image,group,context)
#define _r_listview_scroll(hwnd,id,position) _r_wnd_sendmessage(hwnd,id,LVM_SCROLL,0,position)
static inline NTSTATUS sw_compat_hardlink(PCR_STRINGREF source,PCR_STRINGREF destination)
{
    PR_STRING from=_r_obj_createstring2((PR_STRINGREF)source);
    PR_STRING to=_r_obj_createstring2((PR_STRINGREF)destination);
    NTSTATUS status=CreateHardLinkW(to->buffer,from->buffer,NULL)?STATUS_SUCCESS:_r_sys_doserrortontstatus(GetLastError());
    _r_obj_dereference(from); _r_obj_dereference(to);
    return status;
}
#define _r_fs_createhardlink sw_compat_hardlink

// Current application logging/message APIs place description before status.
#define _r_log(level,guid,title,description,status) _r_log(level,guid,title,status,description)
#define _r_show_errormessage(hwnd,title,description,status,type) _r_show_errormessage(hwnd,title,status,description,type)
static inline PLARGE_INTEGER sw_compat_timestamp(PLARGE_INTEGER value)
{
    NtQuerySystemTime(value);
    return value;
}
#define _r_sys_gettimestamp sw_compat_timestamp
#define _r_toolbar_setpadding(hwnd,id,x,y) _r_wnd_sendmessage(hwnd,id,TB_SETPADDING,0,MAKELPARAM(x,y))
#define _r_toolbar_setbuttonsize(hwnd,id,x,y) _r_wnd_sendmessage(hwnd,id,TB_SETBUTTONSIZE,0,MAKELPARAM(x,y))
#ifndef __cplusplus
static inline HRESULT sw_compat_imagelist_replace(HIMAGELIST image,HBITMAP bitmap,HBITMAP mask,INT index)
{
    INT count=0,added=0;
    HRESULT status=IImageList2_GetImageCount((IImageList2*)image,&count);
    if (FAILED(status)) return status;
    if (index<count) return IImageList2_Replace((IImageList2*)image,index,bitmap,mask);
    return IImageList2_Add((IImageList2*)image,bitmap,mask,&added);
}
#define _r_imagelist_replace sw_compat_imagelist_replace
#endif
