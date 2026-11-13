/* Read-only inventory for an imported Wine Z: tree inaccessible to HDC shell. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
static FILE *out;
static void scan(const wchar_t *root, unsigned depth)
{
    wchar_t pattern[2048], path[2048];
    WIN32_FIND_DATAW data;
    _snwprintf(pattern, 2048, L"%ls\\*", root);
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L"..")) continue;
        _snwprintf(path,2048,L"%ls\\%ls",root,data.cFileName);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && depth) scan(path,depth-1);
        else {
            const wchar_t *ext=wcsrchr(data.cFileName,L'.');
            if (!ext || _wcsicmp(ext,L".exe")) continue;
            HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
            if (file==INVALID_HANDLE_VALUE) continue;
            IMAGE_DOS_HEADER dos; DWORD read; WORD machine=0;
            if (ReadFile(file,&dos,sizeof(dos),&read,NULL) && read==sizeof(dos) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>0) {
                SetFilePointer(file,dos.e_lfanew+4,NULL,FILE_BEGIN);
                ReadFile(file,&machine,sizeof(machine),&read,NULL);
            }
            CloseHandle(file);
            char utf8[6144];
            WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof(utf8),NULL,NULL);
            fprintf(out,"0x%04x\t%s\n",machine,utf8);
        }
    } while (FindNextFileW(find,&data));
    FindClose(find);
}
int main(void)
{
    out=fopen("C:\\vp-gumu10-evidence\\game-executables.tsv","w");
    if (!out) return 1;
    setvbuf(out,NULL,_IONBF,0);
    scan(L"Z:\\games",3);
    fclose(out);
    return 0;
}
