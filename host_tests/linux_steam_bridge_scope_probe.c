// Verify that the experimental DLL rejects ordinary Wine processes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(void){
 char marker[160];DWORD markerSize=GetEnvironmentVariableA("HMOSDECK_STEAM_BRIDGE",marker,sizeof(marker));
 SetLastError(0);HMODULE module=LoadLibraryA("lsteamclient.dll");DWORD error=GetLastError();
 if(module)FreeLibrary(module);
 FILE* result=fopen("C:/windows/temp/hmosdeck-steam-bridge-scope.json","wb");if(!result)return 2;
 int verified=!markerSize&&!module&&error==ERROR_DLL_INIT_FAILED;
 fprintf(result,"{\"experimentalMarkerPresent\":%s,\"moduleLoaded\":%s,\"error\":%lu,\"unscopedBridgeDisabled\":%s,\"windowsSteamStarted\":false}\n",markerSize?"true":"false",module?"true":"false",(unsigned long)error,verified?"true":"false");
 if(fclose(result))return 2;return verified?0:1;
}
