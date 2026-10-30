// Native GNU entry identity shared by access preflight and the user-space exec
// handoff. Reading a file, directory or shared library is not enough to run it.
#pragma once
#include <stddef.h>
static int SteamOhosNativeExecutableHeader(const unsigned char *header,size_t bytes) {
#if defined(__aarch64__)
    const unsigned machine=183;
#elif defined(__x86_64__)
    const unsigned machine=62;
#else
    const unsigned machine=0;
#endif
    if(bytes!=64||header[0]!=127||header[1]!='E'||header[2]!='L'||header[3]!='F'||
       header[4]!=2||header[5]!=1||header[6]!=1||
       (header[16]!=2&&header[16]!=3)||header[17]!=0||
       (header[18]|(header[19]<<8))!=machine||header[52]!=64||header[53]!=0)return 0;
    unsigned nonzero=0;
    for(unsigned i=24;i<32;++i)nonzero|=header[i];
    return nonzero!=0;
}
