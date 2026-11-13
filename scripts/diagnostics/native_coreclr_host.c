/* SPDX-License-Identifier: MIT
 * Host an owned managed fixture through the installed game's actual CoreCLR.
 * Never load game assemblies, call Steam authentication or edit game files.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv) {
    (void)argc;alarm(120);setvbuf(stdout,NULL,_IONBF,0);
    char base[PATH_MAX],host[PATH_MAX],main_assembly[PATH_MAX],assembly[PATH_MAX];
    if(strlen(argv[0])>=sizeof(base))return 120;
    strcpy(base,argv[0]);char *slash=strrchr(base,'/');if(!slash)return 121;*slash=0;
    if(snprintf(host,sizeof(host),"%s/data_sts2_linuxbsd_x86_64/libhostfxr.so",base)>=(int)sizeof(host)||
       snprintf(main_assembly,sizeof(main_assembly),"%s/data_sts2_linuxbsd_x86_64/sts2.dll",base)>=(int)sizeof(main_assembly)||
       snprintf(assembly,sizeof(assembly),"%s/CoreClrContract.dll",base)>=(int)sizeof(assembly))return 122;
    void *lib=dlopen(host,RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"CORECLR_CONTRACT_HOST stage=dlopen error=%s\n",dlerror());return 123;}
    int32_t (*initialize)(int,const char**,const void*,void**)=dlsym(lib,"hostfxr_initialize_for_dotnet_command_line");
    int32_t (*delegate)(void*,int,void**)=dlsym(lib,"hostfxr_get_runtime_delegate");
    int32_t (*close_host)(void*)=dlsym(lib,"hostfxr_close");
    if(!initialize||!delegate||!close_host)return 124;
    // As in exported Godot, use the command-line hosting API for this
    // self-contained deployment. Its app descriptor selects the installed
    // runtime/dependencies; we never call run_app or the game's entry point.
    const char *args[]={main_assembly};
    void *context=NULL;int32_t rc=initialize(1,args,NULL,&context);
    printf("CORECLR_CONTRACT_HOST stage=initialize code=%#x\n",(unsigned)rc);
    if(rc<0||!context)return 125;
    int32_t (*load)(const char*,const char*,const char*,const char*,void*,void**)=NULL;
    rc=delegate(context,5,(void**)&load);close_host(context);
    printf("CORECLR_CONTRACT_HOST stage=delegate code=%#x\n",(unsigned)rc);
    if(rc<0||!load)return 126;
    int (*entry)(void*,int)=NULL;
    rc=load(assembly,"CoreClrContract, CoreClrContract","Run",(const char*)(intptr_t)-1,NULL,(void**)&entry);
    printf("CORECLR_CONTRACT_HOST stage=load code=%#x\n",(unsigned)rc);
    if(rc<0||!entry)return 127;
    return entry(NULL,0);
}
