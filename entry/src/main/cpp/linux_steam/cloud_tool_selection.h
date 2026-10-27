#pragma once
#include <stddef.h>
#include <string.h>
#include <strings.h>

// Read only CompatToolMapping. Steam owns all tool selections and options.
// 0 = other/native tool (leave its compatdata alone), 1 = OHOS, 2 = GNU.
static int CloudToolKind(const char *name) {
    if (!strcmp(name, "hmosdeck") || !strcmp(name, "hmosdeck_proton")) return 1;
    if (!strcmp(name, "hmosdeck_gnu_proton") || !strcmp(name, "hmosdeck_gnu_proton_gl")) return 2;
    return 0;
}
static int CloudVdfToken(const char **cursor, char *token, size_t capacity) {
    const char *p=*cursor; size_t used=0;
    for (;;) {
        while (*p && (unsigned char)*p<=32) ++p;
        if (p[0]=='/' && p[1]=='/') { while (*p && *p!='\n') ++p; continue; }
        break;
    }
    if (!*p) { *cursor=p; return 0; }
    if (*p=='{' || *p=='}') { token[0]=*p++; token[1]=0; *cursor=p; return 2; }
    int quoted=*p=='"'; if (quoted) ++p;
    while (*p && (quoted ? *p!='"' : (unsigned char)*p>32 && *p!='{' && *p!='}')) {
        if (quoted && *p=='\\' && (p[1]=='"' || p[1]=='\\')) ++p;
        // Unrelated settings can contain long launch options or URLs. Scan
        // the entire value while keeping only a bounded comparison token.
        if (used+1<capacity) token[used++]=*p;
        ++p;
    }
    if (quoted && *p!='"') return -1;
    if (quoted) ++p;
    token[used]=0; *cursor=p; return 1;
}
static int CloudVdfObject(const char **cursor,int scope,int depth,const char *app,int *fallback,int *exact) {
    static const char *hierarchy[]={"InstallConfigStore","Software","Valve","Steam","CompatToolMapping"};
    char key[256],value[256];int token;
    if(depth>32)return 0;
    while((token=CloudVdfToken(cursor,key,sizeof(key)))>0) {
        if(token==2)return key[0]=='}'&&depth>0;
        int type=CloudVdfToken(cursor,value,sizeof(value));
        if(type<=0||(type==2&&value[0]=='}'))return 0;
        if(type==2) {
            int child=-1;
            if(scope>=0&&scope<5&&!strcasecmp(key,hierarchy[scope]))child=scope+1;
            else if(scope==5) {
                if(!strcmp(key,"0"))child=6;
                else if(!strcmp(key,app))child=7;
            }
            if(!CloudVdfObject(cursor,child,depth+1,app,fallback,exact))return 0;
        } else if((scope==6||scope==7)&&!strcasecmp(key,"name")) {
            if(scope==6)*fallback=CloudToolKind(value);else *exact=CloudToolKind(value);
        }
    }
    return token==0&&depth==0;
}
static int CloudToolFromVdf(const char *text, const char *app) {
    int fallback=0,exact=-1;
    return CloudVdfObject(&text,0,0,app,&fallback,&exact)?(exact>=0?exact:fallback):0;
}
