#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
static HANDLE output;
static void log_line(const char *format, ...) {
    char buffer[1024]; va_list args; va_start(args,format);
    int n=vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);
    DWORD written;WriteFile(output,buffer,n,&written,NULL);FlushFileBuffers(output);
}
int main(int argc,char **argv) {
    output=CreateFileW(L"C:\\vp-icu-probe.txt",GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(output==INVALID_HANDLE_VALUE)return 250;
    if(argc>1) {
        HMODULE crt=LoadLibraryA(argv[1]);
        log_line("explicit CRT=%p error=%lu\n",crt,GetLastError());
        if(!crt)return 7;
    }
    HMODULE module=LoadLibraryW(L"icuuc.dll");
    log_line("module=%p error=%lu\n",module,GetLastError());
    if(!module)return 1;
    void (__cdecl *version)(uint8_t*)=(void*)GetProcAddress(module,"u_getVersion");
    void (__cdecl *chars)(const char*,uint16_t*,int32_t)=(void*)GetProcAddress(module,"u_charsToUChars");
    int32_t (__cdecl *upper)(uint16_t*,int32_t,const uint16_t*,int32_t,const char*,int32_t*)=(void*)GetProcAddress(module,"u_strToUpper");
    MEMORY_BASIC_INFORMATION v={0};VirtualQuery((void*)version,&v,sizeof(v));
    log_line("version=%p chars=%p upper=%p page=%p protect=%lx\n",version,chars,upper,v.BaseAddress,v.Protect);
    if(!version||!chars||!upper)return 2;
    uint8_t numbers[4]={0};version(numbers);
    log_line("version=%u.%u.%u.%u\n",numbers[0],numbers[1],numbers[2],numbers[3]);
    void (__cdecl *init)(int32_t*)=(void*)GetProcAddress(module,"u_init");
    int32_t init_status=0;
    log_line("calling u_init=%p\n",init);if(!init)return 3;init(&init_status);
    log_line("u_init status=%ld\n",(long)init_status);
    const char* (__cdecl *locale)(void)=(void*)GetProcAddress(module,"uloc_getDefault");
    log_line("calling uloc_getDefault=%p\n",locale);if(!locale)return 4;
    log_line("default=%s\n",locale());
    HMODULE international=LoadLibraryW(L"icuin.dll");
    log_line("international=%p\n",international);if(!international)return 5;
    void* (__cdecl *collation)(const char*,int32_t*)=(void*)GetProcAddress(international,"ucol_open");
    log_line("calling ucol_open=%p\n",collation);if(!collation)return 6;
    int32_t col_status=0;void *collator=collation("root",&col_status);
    log_line("collator=%p status=%ld\n",collator,(long)col_status);
    int32_t (__cdecl *canonicalize)(const char*,char*,int32_t,int32_t*)=(void*)GetProcAddress(module,"uloc_canonicalize");
    int32_t (__cdecl *language_fn)(const char*,char*,int32_t,int32_t*)=(void*)GetProcAddress(module,"uloc_getLanguage");
    int32_t (__cdecl *count_fn)(void)=(void*)GetProcAddress(module,"uloc_countAvailable");
    const char* (__cdecl *available)(int32_t)=(void*)GetProcAddress(module,"uloc_getAvailable");
    if(!canonicalize||!language_fn||!count_fn||!available)return 8;
    int count=count_fn(),locale_errors=0,nls_errors=0;
    for(int i=0;i<count;++i) {
        const char *name=available(i);char normalized[157]={0},language[12]={0};int32_t error=0;
        int32_t n=canonicalize(name,normalized,sizeof(normalized),&error);
        int32_t l=language_fn(name,language,sizeof(language),&error);
        if(error>0||error==-124)log_line("locale=%s normalized=%s lang=%s n=%ld l=%ld error=%ld\n",name,normalized,language,(long)n,(long)l,(long)error);
        if(error>0||error==-124)locale_errors++;
        WCHAR os_name[157]={0},os_result[157]={0};
        for(int j=0;j<n && j<156;++j)os_name[j]=normalized[j]=='_'?'-':(unsigned char)normalized[j];
        if(!GetLocaleInfoEx(os_name,LOCALE_SNAME,os_result,157)) {
            log_line("NLS mismatch locale=%s normalized=%s error=%lu\n",name,normalized,GetLastError());
            nls_errors++;
        }
    }
    log_line("locales=%d ICU_errors=%d NLS_mismatches=%d\n",count,locale_errors,nls_errors);
    uint16_t text[8]={0};chars("abc",text,3);
    log_line("chars=%04x,%04x,%04x\n",text[0],text[1],text[2]);
    int32_t status=0;uint16_t src[2]={'i',0},dst[8]={0};
    int32_t length=upper(dst,8,src,1,"tr_TR",&status);
    log_line("upper length=%ld status=%ld value=%04x\n",(long)length,(long)status,dst[0]);
    int failures=locale_errors+nls_errors;
    failures+=text[0]!='a'||text[1]!='b'||text[2]!='c'||status>0||length!=1||dst[0]!=0x130;
    log_line("failures=%d\n",failures);CloseHandle(output);return failures;
}
