// Exercise process-local Steam bootstrap without launching a Steam executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static const char *path="Software\\Valve\\Steam\\ActiveProcess";
static int query(char* out,DWORD capacity){
 HKEY key;DWORD size=capacity,type=0;out[0]=0;
 LONG e=RegOpenKeyExA(HKEY_CURRENT_USER,path,0,KEY_READ,&key);if(e)return e;
 e=RegQueryValueExA(key,"SteamClientDll",NULL,&type,(BYTE*)out,&size);RegCloseKey(key);
 return e?e:(type==REG_SZ?0:ERROR_INVALID_DATA);
}
int main(void){
 char before[1024],after[1024],active[1024],invalid[1024];
 const char* marker="32123:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef:695630";
 char previous[160];DWORD previousSize=GetEnvironmentVariableA("HMOSDECK_STEAM_BRIDGE",previous,sizeof(previous));
 if(previousSize)return 3;
 int beforeError=query(before,sizeof(before));
 SetLastError(0);HMODULE dll=LoadLibraryA("lsteamclient.dll");DWORD noMarkerError=GetLastError();if(dll)FreeLibrary(dll);
 SetEnvironmentVariableA("SteamAppId","695630");SetEnvironmentVariableA("HMOSDECK_STEAM_BRIDGE",marker);
 int activeError=query(active,sizeof(active));
 int privateView=!activeError&&!strcmp(active,"C:\\windows\\syswow64\\lsteamclient.dll");
 HKEY root=0,key=0;DWORD type=0,size=sizeof(active);WCHAR wide[1024];
 int nested=!RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",0,KEY_READ,&root)&&
  !RegOpenKeyExW(root,L"ActiveProcess",0,KEY_READ,&key)&&
  !RegQueryValueExW(key,L"SteamClientDll64",NULL,&type,(BYTE*)wide,&size)&&
  !wcscmp(wide,L"C:\\windows\\system32\\lsteamclient.dll");
 if(key)RegCloseKey(key);if(root)RegCloseKey(root);
 dll=LoadLibraryA("lsteamclient.dll");int bridgeLoad=dll!=NULL;if(dll)FreeLibrary(dll);
 SetEnvironmentVariableA("SteamAppId","1");int invalidError=query(invalid,sizeof(invalid));
 int mismatch=invalidError==beforeError&&(beforeError||!strcmp(before,invalid));
 SetEnvironmentVariableA("HMOSDECK_STEAM_BRIDGE",NULL);
 int afterError=query(after,sizeof(after));int unchanged=afterError==beforeError&&(beforeError||!strcmp(before,after));
 int unscoped=!dll&&noMarkerError==ERROR_DLL_INIT_FAILED; // recomputed below; dll now scoped result
 unscoped=noMarkerError==ERROR_DLL_INIT_FAILED;
 FILE* out=fopen("C:/windows/temp/hmosdeck-steam-session-registry.json","wb");if(!out)return 2;
 int ok=privateView&&nested&&bridgeLoad&&mismatch&&unchanged&&unscoped;
 fprintf(out,"{\"privateView\":%s,\"nestedUnicodeView\":%s,\"scopedBridgeLoad\":%s,\"wrongAppRejected\":%s,\"sharedValveValuesUnchanged\":%s,\"unscopedBridgeRejected\":%s,\"beforeError\":%d,\"activeError\":%d,\"passed\":%s,\"authenticationVerified\":false}\n",privateView?"true":"false",nested?"true":"false",bridgeLoad?"true":"false",mismatch?"true":"false",unchanged?"true":"false",unscoped?"true":"false",beforeError,activeError,ok?"true":"false");
 if(fclose(out))return 2;return ok?0:1;
}
