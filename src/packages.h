// simplewall
// Copyright (c) 2016-2026 Henry++

#pragma once

BOOLEAN _app_package_isnotexists (
	_In_ PR_STRING package_sid
);

VOID _app_package_parsepath (
	_Inout_ PR_STRING_PTR package_root_folder
);

VOID _app_package_getpackagebyname (
	_In_ HANDLE hroot,
	_In_ LPCWSTR path,
	_In_ PR_STRING key_name
);

VOID _app_package_getpackagebysid (
	_In_ HANDLE hroot,
	_In_ LPCWSTR path,
	_In_ PR_STRING key_name
);

VOID NTAPI _app_package_threadproc (
	_In_ PVOID arglist
);

VOID _app_package_getpackageslist (
	_In_opt_ HWND hwnd
);

VOID _app_package_getserviceslist (
	_In_opt_ HWND hwnd
);

LPENUM_SERVICE_STATUS_PROCESS _app_package_enumservices (
	_In_opt_ HWND hwnd,
	_In_ BOOLEAN include_instances,
	_Out_ PULONG count
);
