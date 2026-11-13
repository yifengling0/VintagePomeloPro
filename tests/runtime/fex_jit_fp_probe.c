#include <windows.h>
#include <stdint.h>
#include <string.h>
static const unsigned char caller[]={
  0x48,0x83,0xec,0x38,                 /* sub rsp,56 */
  0x0f,0x57,0xc9,                      /* xorps xmm1,xmm1 */
  0xf3,0x0f,0x11,0x4c,0x24,0x20,
  0xf3,0x0f,0x10,0x0d,0,0,0,0,         /* movss xmm1,[rip+const -1] */
  0xf3,0x0f,0x11,0x4c,0x24,0x28,
  0xf3,0x0f,0x10,0x0d,0,0,0,0,         /* movss xmm1,[rip+800] */
  0xf3,0x0f,0x10,0x15,0,0,0,0,         /* movss xmm2,[rip+600] */
  0x0f,0x57,0xdb,0x0f,0x57,0xc0,
  0xe8,0,0,0,0,                       /* call callee */
  0x48,0x83,0xc4,0x38,0xc3};
static const unsigned char callee[]={
  0xf3,0x0f,0x11,0x01,                 /* movss [rcx],xmm0 */
  0xf3,0x0f,0x11,0x49,0x04,
  0xf3,0x0f,0x11,0x51,0x08,
  0xf3,0x0f,0x11,0x59,0x0c,
  0xf3,0x0f,0x10,0x44,0x24,0x28,
  0xf3,0x0f,0x11,0x41,0x10,
  0xf3,0x0f,0x10,0x44,0x24,0x30,
  0xf3,0x0f,0x11,0x41,0x14,0xc3};
static void rel(unsigned char*p,int where,int target) {int x=target-(where+4);memcpy(p+where,&x,4);}
int main(void) {
  float results[4][6]={{0}};DWORD old=0;
  unsigned char *code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
  if(!code)return 2;
  memcpy(code,caller,sizeof(caller));memcpy(code+256,callee,sizeof(callee));
  const float values[3]={-1,800,600};memcpy(code+512,values,sizeof(values));
  rel(code,17,512);rel(code,31,516);rel(code,39,520);rel(code,50,256);
  FlushInstructionCache(GetCurrentProcess(),code,4096);
  ((void(*)(float*))code)(results[0]);
  VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old);
  FlushInstructionCache(GetCurrentProcess(),code,4096);
  ((void(*)(float*))code)(results[1]);
  VirtualProtect(code,4096,PAGE_READWRITE,&old);
  const float swapped[3]={-2,1600,1200};memcpy(code+512,swapped,sizeof(swapped));
  VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old);
  FlushInstructionCache(GetCurrentProcess(),code,4096);
  ((void(*)(float*))code)(results[2]);
  ((void(*)(float*))code)(results[3]);
  HANDLE f=CreateFileW(L"C:\\vp-dynamic-fp.bin",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  DWORD w=0;BOOL ok=WriteFile(f,results,sizeof(results),&w,NULL);CloseHandle(f);
  int failures=0;
  for (int row=0;row<4;++row) {
    const float factor = row<2 ? 1.0f : 2.0f;
    const float expected[6]={0,800*factor,600*factor,0,0,-factor};
    if(memcmp(results[row],expected,sizeof(expected))) ++failures;
  }
  VirtualFree(code,0,MEM_RELEASE);return ok&&w==sizeof(results)?failures:3;
}
