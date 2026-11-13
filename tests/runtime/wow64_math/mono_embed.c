#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
 if(argc != 4) return 1;
 FILE* f=fopen("C:/windows/temp/mono-cpu-features.txt","w");
 if(f){fprintf(f,"SSE=%d SSE2=%d SSE3=%d MMX=%d\n",IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE),IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE),IsProcessorFeaturePresent(PF_SSE3_INSTRUCTIONS_AVAILABLE),IsProcessorFeaturePresent(PF_MMX_INSTRUCTIONS_AVAILABLE));fclose(f);}
 HMODULE m=LoadLibraryA(argv[1]);
 if(!m) return 2;
 void (__cdecl *setpath)(const char*)=(void*)GetProcAddress(m,"mono_set_assemblies_path");
 void* (__cdecl *init)(const char*)=(void*)GetProcAddress(m,"mono_jit_init");
 void* (__cdecl *open)(void*,const char*)=(void*)GetProcAddress(m,"mono_domain_assembly_open");
 int (__cdecl *exec)(void*,void*,int,char**)=(void*)GetProcAddress(m,"mono_jit_exec");
 if(!setpath||!init||!open||!exec) return 3;
 setpath(argv[2]);
 void* d=init("vpp-math");if(!d)return 4;
 char* exe=argv[3];
 void* a=open(d,exe);if(!a)return 5;
 return exec(d,a,1,&exe);
}