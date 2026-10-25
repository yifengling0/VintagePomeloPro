#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

/* Mirror the original OHOS Wine locale/font fallback using standard prefix
 * configuration. Valve Wine regenerates SystemLink defaults on each GDI init;
 * missing legacy-family aliases let its internal links resolve real CJK faces.
 * Never replace installed fonts or an existing user substitute. */
static int set_string(HKEY key, const WCHAR* name, UINT page) {
    WCHAR value[16];swprintf(value,16,L"%u",page);
    LONG status=RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)value,(lstrlenW(value)+1)*sizeof(WCHAR));
    if(status)fprintf(stderr,"locale registry write failed status=%ld\n",status);
    return status==ERROR_SUCCESS;
}

/* Prefix provisioning must not initialize a GUI desktop: doing so before
 * the first application window can wait for explorer's display bootstrap.
 * Wine records installed families in the Fonts registry view. Check both
 * user and machine fonts, preserving real installed faces and user aliases. */
static int registered_family(HKEY hive,const WCHAR* family,int* found) {
    HKEY key;
    LONG status=RegOpenKeyExW(hive,L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
        0,KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
    if(status==ERROR_FILE_NOT_FOUND)return 1;
    if(status)return 0;
    const size_t length=wcslen(family);
    for(DWORD index=0;;++index) {
        WCHAR name[512];DWORD size=sizeof(name)/sizeof(name[0]);
        status=RegEnumValueW(key,index,name,&size,0,0,0,0);
        if(status==ERROR_NO_MORE_ITEMS)break;
        if(status==ERROR_MORE_DATA)continue;
        if(status){RegCloseKey(key);return 0;}
        for(const WCHAR* part=name;*part;) {
            while(*part==L' ')++part;
            if(!_wcsnicmp(part,family,length) &&
                (!part[length] || (part[length]==L' ' && (part[length+1]==L'(' || part[length+1]==L'&')))) {
                *found=1;RegCloseKey(key);return 1;
            }
            part=wcschr(part,L'&');if(!part)break;++part;
        }
    }
    RegCloseKey(key);return 1;
}

static int add_substitute(HKEY key,const WCHAR* name,const WCHAR* target) {
    DWORD bytes=0;
    LONG status=RegQueryValueExW(key,name,0,0,0,&bytes);
    if(status==ERROR_SUCCESS)return 1; /* Preserve user choices. */
    if(status!=ERROR_FILE_NOT_FOUND)return 0;
    return RegSetValueExW(key,name,0,REG_SZ,(const BYTE*)target,(lstrlenW(target)+1)*sizeof(WCHAR))==ERROR_SUCCESS;
}

static int configure_native_fonts(void) {
    HKEY key;
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
        0,0,0,KEY_QUERY_VALUE|KEY_SET_VALUE|KEY_WOW64_64KEY,0,&key,0))return 0;
    struct alias { const WCHAR* name; const WCHAR* target; };
    static const struct alias aliases[]={
        {L"SimSun",L"Noto Serif CJK SC"},{L"NSimSun",L"Noto Serif CJK SC"},
        {L"\x5b8b\x4f53",L"Noto Serif CJK SC"},{L"\x65b0\x5b8b\x4f53",L"Noto Serif CJK SC"},
        {L"SimHei",L"HarmonyOS Sans SC"},{L"\x9ed1\x4f53",L"HarmonyOS Sans SC"},
        {L"Microsoft YaHei",L"HarmonyOS Sans SC"},{L"Microsoft YaHei UI",L"HarmonyOS Sans SC"},
        {L"PMingLiU",L"Noto Serif CJK TC"},{L"MingLiU",L"Noto Serif CJK TC"},
        {L"Microsoft JhengHei",L"Noto Sans CJK TC"},{L"Microsoft JhengHei UI",L"Noto Sans CJK TC"},
        {L"MS UI Gothic",L"Noto Sans CJK JP"},{L"MS Gothic",L"Noto Sans CJK JP"},
        {L"MS Mincho",L"Noto Serif CJK JP"}
    };
    int ok=1;
    for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);i++) {
        int installed=0;
        if(!registered_family(HKEY_LOCAL_MACHINE,aliases[i].name,&installed) ||
           !registered_family(HKEY_CURRENT_USER,aliases[i].name,&installed)){ok=0;break;}
        if(installed)continue;
        if(!add_substitute(key,aliases[i].name,aliases[i].target))ok=0;
    }
    RegCloseKey(key);return ok;
}

static int configure(void) {
    HKEY key;UINT page=GetACP();
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"System\\CurrentControlSet\\Control\\Nls\\Codepage",
        0,0,0,KEY_SET_VALUE,0,&key,0))return 2;
    int ok=set_string(key,L"ACP",page) && set_string(key,L"OEMCP",GetOEMCP()) &&
        set_string(key,L"MACCP",page==936?10008u:page==950?10002u:page==932?10001u:10000u);
    RegCloseKey(key);if(!ok)return 2;
    if(!configure_native_fonts())return 2;
    puts("HMOSDECK_GNU_LOCALE_CONFIGURED HMOSDECK_GNU_X86_OK");return 0;
}

int main(int argc,char** argv) {
    /* Same GBK bytes used by legacy ANSI UIs. Verify decoding before fonts. */
    const char ansi[]="\xd6\xd0\xce\xc4\xb2\xe2\xca\xd4";
    const WCHAR expected[]={0x4e2d,0x6587,0x6d4b,0x8bd5,0};
    WCHAR decoded[16],locale[LOCALE_NAME_MAX_LENGTH];
    int n=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,ansi,-1,decoded,16);
    BOOL correct=n==5 && !memcmp(decoded,expected,sizeof(expected));
    GetSystemDefaultLocaleName(locale,LOCALE_NAME_MAX_LENGTH);
    char locale8[128]={0};WideCharToMultiByte(CP_UTF8,0,locale,-1,locale8,sizeof(locale8),0,0);
    printf("HMOSDECK_GNU_LOCALE acp=%u oemcp=%u system_lcid=%04lx locale=%s gbk_decode=%d\n",
           GetACP(),GetOEMCP(),(unsigned long)GetSystemDefaultLCID(),locale8,correct);
    if(argc>=2 && !strcmp(argv[1],"--configure"))
    {
        UINT required=argc==3 && !strcmp(argv[2],"zh_CN")?936u:
            argc==3 && !strcmp(argv[2],"zh_TW")?950u:
            argc==3 && !strcmp(argv[2],"ja_JP")?932u:1252u;
        if(argc==3 && GetACP()!=required) {
            fprintf(stderr,"locale unavailable requested=%s acp=%u expected=%u\n",argv[2],GetACP(),required);return 3;
        }
        return configure();
    }
    HDC dc=CreateCompatibleDC(NULL);
    const WCHAR* families[]={L"HarmonyOS Sans SC",L"Noto Serif CJK SC",L"SimSun",L"Tahoma",L"Arial",L"Times New Roman",L"this-font-does-not-exist"};
    BOOL cjk=FALSE,latin_link=FALSE;
    for(unsigned i=0;i<sizeof(families)/sizeof(families[0]);i++) {
        HFONT font=CreateFontW(20,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,families[i]);
        HGDIOBJ old=SelectObject(dc,font);WORD glyph[4]={0xffff,0xffff,0xffff,0xffff};
        DWORD result=GetGlyphIndicesW(dc,expected,4,glyph,GGI_MARK_NONEXISTING_GLYPHS);
        WCHAR face[128]={0};GetTextFaceW(dc,128,face);char face8[512]={0};
        WideCharToMultiByte(CP_UTF8,0,face,-1,face8,sizeof(face8),0,0);
        BOOL present=result!=GDI_ERROR;
        for(int j=0;j<4;j++)if(glyph[j]==0xffff||glyph[j]==0)present=FALSE;
        FONTSIGNATURE signature;int charset=GetTextCharsetInfo(dc,&signature,0);
        SIZE sizeA={0},sizeW={0};
        GetTextExtentPoint32A(dc,ansi,8,&sizeA);GetTextExtentPoint32W(dc,expected,4,&sizeW);
        printf("HMOSDECK_GNU_FONT index=%u resolved=%s cjk_glyphs=%d charset=%d ansi_width=%ld unicode_width=%ld\n",
               i,face8,present,charset,sizeA.cx,sizeW.cx);
        if(i==0)cjk=present;
        if(i==3)latin_link=charset==GB2312_CHARSET && sizeA.cx==sizeW.cx;
        SelectObject(dc,old);DeleteObject(font);
    }
    DeleteDC(dc);
    if(GetACP()==936 && correct && cjk && latin_link) {
        puts("HMOSDECK_GNU_LOCALE_OK HMOSDECK_GNU_X86_OK");return 0;
    }
    return 1;
}
