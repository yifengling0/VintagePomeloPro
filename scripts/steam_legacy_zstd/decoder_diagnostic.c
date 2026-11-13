/* Opt-in diagnostic overlay. Log only classifications, lengths, status and
 * module paths, never block bytes, keys, account data or network URLs. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { const char *stage; uint64_t detail; } decode_detail;
#define VP_DECODER_NAME VPDecodeDetailed
#define VP_DECODER_DETAILS , decode_detail *diagnostic
#define VP_DECODE_RESULT(result, why, value) do { \
    diagnostic->stage=(why); diagnostic->detail=(uint64_t)(value); return (result); \
} while (0)
#include "vpsteamzstd.c"

typedef int (*original_decoder)(const unsigned char *,unsigned,steam_buffer *,unsigned);
__declspec(dllexport) int VPDecodeDispatch(const unsigned char *,unsigned,steam_buffer *,unsigned,original_decoder);

typedef struct { char data[2304]; size_t used; } log_line;
static void text(log_line *line,const char *value) {
    while (*value && line->used+1<sizeof(line->data)) line->data[line->used++]=*value++;
}
static void number(log_line *line,uint64_t value) {
    char digits[24]; unsigned count=0;
    do {digits[count++]=(char)('0'+value%10);value/=10;} while (value);
    while (count && line->used+1<sizeof(line->data)) line->data[line->used++]=digits[--count];
}
static void field(log_line *line,const char *name,uint64_t value) {
    text(line,"\t");text(line,name);text(line,"=");number(line,value);
}
static void wide(log_line *line,const wchar_t *value) {
    char converted[768];
    int n=WideCharToMultiByte(CP_UTF8,0,value,-1,converted,sizeof(converted),NULL,NULL);
    if (n>0) text(line,converted); else text(line,"[path-too-long]");
}
static const char *format(const unsigned char *data,unsigned size) {
    if (!data) return "null";
    if (size>=4 && !memcmp(data,"VSZa",4)) return "VSZa";
    if (size>=3 && !memcmp(data,"VZa",3)) return "VZa";
    if (size>=4 && !memcmp(data,"PK\3\4",4)) return "PKZIP";
    if (size>=2 && data[0]==0x1f && data[1]==0x8b) return "gzip";
    if (size>=2 && (data[0]&15)==8 && (((unsigned)data[0]<<8)|data[1])%31==0) return "zlib";
    return size<4?"short":"other";
}
static void write_record(log_line *line) {
    /* A single write under the lock keeps concurrent call records intact.
     * 512 records and 2304-byte lines bound each file to less than 1.2 MiB. */
    static SRWLOCK lock=SRWLOCK_INIT;
    static HANDLE output=INVALID_HANDLE_VALUE;
    static unsigned attempts;
    char enabled[2];
    if (GetEnvironmentVariableA("VPP_STEAM_DECODE_DIAG",enabled,sizeof(enabled))!=1 || enabled[0]!='1') return;
    AcquireSRWLockExclusive(&lock);
    if (attempts>=512) {ReleaseSRWLockExclusive(&lock);return;}
    ++attempts; /* Bound retries and partial writes when the directory is unwritable. */
    if (output==INVALID_HANDLE_VALUE) {
        wchar_t path[MAX_PATH];
        HMODULE self=GetModuleHandleW(sizeof(void *)==8?L"vpsteamzstd64.dll":L"vpsteamzstd32.dll");
        DWORD length=self?GetModuleFileNameW(self,path,MAX_PATH):0;
        if (length && length<MAX_PATH) {
            while (length && path[length-1]!=L'\\' && path[length-1]!=L'/') --length;
            log_line name={0};text(&name,"vp-steam-decode-");number(&name,GetCurrentProcessId());
            text(&name,sizeof(void *)==8?"-64.log":"-32.log");
            if (length+name.used<MAX_PATH) {
                for (size_t i=0;i<name.used;i++) path[length+i]=(wchar_t)name.data[i];
                path[length+name.used]=0;
                output=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
            }
        }
    }
    if (output!=INVALID_HANDLE_VALUE) {
        DWORD written; text(line,"\r\n");
        WriteFile(output,line->data,(DWORD)line->used,&written,NULL);
    }
    ReleaseSRWLockExclusive(&lock);
}
__declspec(dllexport) int VPDecodeDispatch(const unsigned char *data,unsigned size,
        steam_buffer *buffer,unsigned max_output,original_decoder original) {
    decode_detail detail={"original",0};
    int before=buffer?buffer->put:0;
    unsigned flags=buffer?buffer->flags:0,error=buffer?buffer->error:0;
    const char *kind=format(data,size);
    int result;
    if (size>=4 && data && !memcmp(data,"VSZa",4))
        result=VPDecodeDetailed(data,size,buffer,max_output,&detail);
    else if (original) result=original(data,size,buffer,max_output);
    else {detail.stage="original-missing";result=2;}
    log_line line={0};text(&line,"decode-v1");field(&line,"bits",8*sizeof(void *));
    field(&line,"pid",GetCurrentProcessId());field(&line,"tid",GetCurrentThreadId());
    field(&line,"tick_ms",GetTickCount64());text(&line,"\tformat=");text(&line,kind);
    text(&line,"\tstage=");text(&line,detail.stage);field(&line,"detail",detail.detail);
    field(&line,"input",size);field(&line,"max_output",max_output);field(&line,"result",result);
    field(&line,"before_put",(uint32_t)before);field(&line,"before_flags",flags);field(&line,"before_error",error);
    if (buffer) {
        field(&line,"after_put",(uint32_t)buffer->put);field(&line,"allocated",(uint32_t)buffer->allocation_count);
        field(&line,"element_size",buffer->element_size);field(&line,"after_error",buffer->error);
    }
    wchar_t path[MAX_PATH];HMODULE module=GetModuleHandleW(MODULE);
    DWORD length=module?GetModuleFileNameW(module,path,MAX_PATH):0;
    text(&line,"\tclient=");if (length && length<MAX_PATH) wide(&line,path); else text(&line,"[unavailable]");
    write_record(&line);
    return result;
}
