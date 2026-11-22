"""Check that the share prefixes accepted by app and rule handling are exact."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <wchar.h>
#include "share_path.h"

int main(void)
{
    const wchar_t *tail = L"wsl.localhost\\Debian\\tmp\\App.exe";
    const wchar_t *aliases[] = {
        L"\\\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\\\?\\UNC\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\??\\UNC\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\Device\\Mup\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\Device\\LanmanRedirector\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\Device\\Mup\\;LanmanRedirector\\;X:00000000000000ab\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\Device\\LanmanRedirector\\;X:00000000000000ab\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\\\;LanmanRedirector\\;X:00000000000000ab\\wsl.localhost\\Debian\\tmp\\App.exe",
        L"\\\\;X:00000000000000ab\\wsl.localhost\\Debian\\tmp\\App.exe",
    };
    for (unsigned i = 0; i < sizeof(aliases)/sizeof(aliases[0]); i++) {
        const wchar_t *actual = sw_share_path_tail(aliases[i]);
        assert(actual && wcscmp(actual, tail) == 0);
    }

    assert(!sw_share_path_tail(L"\\\\server"));
    assert(!sw_share_path_tail(L"\\\\server\\share"));
    assert(!sw_share_path_tail(L"\\\\server\\share\\"));
    assert(!sw_share_path_tail(L"\\\\?\\C:\\Apps\\App.exe"));
    assert(!sw_share_path_tail(L"\\\\.\\pipe\\App.exe"));
    assert(!sw_share_path_tail(L"\\Device\\Muppet\\server\\share\\App.exe"));
    assert(!sw_share_path_tail(L"\\Device\\Mup\\;unknown\\server\\share\\App.exe"));
    assert(!sw_share_path_tail(L"\\Device\\Mup\\;X:garbage\\server\\share\\App.exe"));
    assert(!sw_share_path_tail(L"\\\\server\\share\\*"));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    source = pathlib.Path(folder) / 'test.c'
    exe = source.with_suffix('')
    source.write_text(SOURCE)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Isrc', str(source), '-o', str(exe)],
                   cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
print('Share path identity tests passed')
