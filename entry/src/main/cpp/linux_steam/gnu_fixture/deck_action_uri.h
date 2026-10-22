/* Shared fixed Steam URI contract for cold argv and live supervisor dispatch. */
#ifndef HMOSDECK_ACTION_URI_H
#define HMOSDECK_ACTION_URI_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline const char *deck_uri(const char *action,const char *app,char *uri,size_t size) {
    if(!strcmp(action,"launch")) {
        size_t n=strlen(app);char *end=NULL;unsigned long long id=strtoull(app,&end,10);
        if(!n||n>10||app[0]=='0'||strspn(app,"0123456789")!=n||!end||*end||id>4294967295ULL)return NULL;
        if(snprintf(uri,size,"steam://rungameid/%s",app)>=(int)size)return NULL;
        return uri;
    }
    if(strcmp(app,"0"))return NULL;
    if(!strcmp(action,"library"))return "steam://open/library";
    if(!strcmp(action,"downloads"))return "steam://open/downloads";
    if(!strcmp(action,"store"))return "steam://store";
    if(!strcmp(action,"account"))return "steam://open/settings/account";
    if(!strcmp(action,"desktop"))return "steam://close/bigpicture";
    if(!strcmp(action,"bigPicture"))return "steam://open/bigpicture";
    if(!strcmp(action,"shutdown"))return "steam://exit";
    return NULL;
}
#endif
