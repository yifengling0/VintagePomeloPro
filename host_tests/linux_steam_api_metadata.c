// Read-only diagnostic: inspect Steam interface version strings without loading
// or changing any game DLL, registry entry, account state or save file.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static int version(const unsigned char *data, size_t size, size_t at, char *out) {
    static const char *prefixes[] = {"SteamClient", "SteamUser", "SteamFriends", "SteamUtils",
        "SteamMatchMaking", "SteamMatchMakingServers", "SteamNetworking", "SteamRemoteStorage",
        "SteamScreenshots", "SteamHTTP", "SteamController", "SteamUGC", "STEAMAPPS_INTERFACE_VERSION",
        "STEAMUSERSTATS_INTERFACE_VERSION", "STEAMAPPLIST_INTERFACE_VERSION",
        "STEAMREMOTESTORAGE_INTERFACE_VERSION", "STEAMSCREENSHOTS_INTERFACE_VERSION",
        "STEAMHTTP_INTERFACE_VERSION", "STEAMUGC_INTERFACE_VERSION"};
    for (size_t i=0;i<sizeof(prefixes)/sizeof(*prefixes);++i) {
        size_t n=strlen(prefixes[i]);
        if (at+n+4>size || memcmp(data+at,prefixes[i],n)) continue;
        if (data[at+n]<'0'||data[at+n]>'9'||data[at+n+1]<'0'||data[at+n+1]>'9'||
            data[at+n+2]<'0'||data[at+n+2]>'9'||data[at+n+3]) continue;
        memcpy(out,data+at,n+3);out[n+3]=0;return 1;
    }
    return 0;
}
int main(void) {
    const char *input="Z:/SteamLinuxARM64/library/steamapps/common/NewPAL/steam_api.dll";
    const char *output="C:/windows/temp/hmosdeck-game-steam-api-metadata.json";
    FILE *f=fopen(input,"rb"); unsigned char *bytes=NULL;
    const char *status="unavailable"; long length=0; unsigned machine=0,count=0;
    char names[128][96];
    if(f) {
        if(!fseek(f,0,SEEK_END) && (length=ftell(f))>=64 && length<=32*1024*1024 && !fseek(f,0,SEEK_SET)) {
            bytes=malloc((size_t)length);
            if(bytes && fread(bytes,1,(size_t)length,f)==(size_t)length) {
                uint32_t pe=(uint32_t)bytes[60]|(uint32_t)bytes[61]<<8|(uint32_t)bytes[62]<<16|(uint32_t)bytes[63]<<24;
                if(bytes[0]=='M'&&bytes[1]=='Z'&&pe<=(uint32_t)length-6&&!memcmp(bytes+pe,"PE\0\0",4)) {
                    machine=bytes[pe+4]|bytes[pe+5]<<8;status="read_only_metadata_collected";
                    for(size_t at=0;at<(size_t)length&&count<128;++at) {
                        char text[96];if(!version(bytes,(size_t)length,at,text))continue;
                        unsigned i;for(i=0;i<count;++i)if(!strcmp(names[i],text))break;
                        if(i==count)strcpy(names[count++],text);
                    }
                } else status="invalid_pe";
            } else status="read_failed";
        } else status="size_invalid";
        fclose(f);
    }
    free(bytes);f=fopen(output,"wb");if(!f)return 2;
    fprintf(f,"{\"status\":\"%s\",\"module\":\"steam_api.dll\",\"bytes\":%ld,\"peMachine\":%u,\"versions\":[",status,length,machine);
    for(unsigned i=0;i<count;++i)fprintf(f,"%s\"%s\"",i?",":"",names[i]);
    fputs("],\"gameFilesModified\":false,\"gameDllExecuted\":false}\n",f);
    int failed=fclose(f);return failed?2:strcmp(status,"read_only_metadata_collected")?1:0;
}
