#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int main(int argc,char **argv) {
    FILE *out=fopen(argc>1?argv[1]:"network-contract.txt","w");
    MIB_IF_TABLE2 *links=NULL;
    MIB_IPINTERFACE_TABLE *ipv4=NULL;
    MIB_IPFORWARD_TABLE2 *routes=NULL;
    DWORD ret,link_ret=GetIfTable2(&links),ip_ret=GetIpInterfaceTable(AF_INET,&ipv4);
    unsigned expected=0,count=0,failures=0;
    if (!ip_ret && !link_ret) for (ULONG i=0;i<ipv4->NumEntries;i++)
        for (ULONG j=0;j<links->NumEntries;j++)
            if (ipv4->Table[i].InterfaceLuid.Value==links->Table[j].InterfaceLuid.Value &&
                links->Table[j].Type!=IF_TYPE_SOFTWARE_LOOPBACK) expected++;
    ULONG size=0;ret=GetAdaptersInfo(NULL,&size);
    fprintf(out,"links ret=%lu count=%lu ipv4 ret=%lu count=%lu expected-adapters=%u\n",
            link_ret,link_ret?0:links->NumEntries,ip_ret,ip_ret?0:ipv4->NumEntries,expected);
    fprintf(out,"size-query ret=%lu bytes=%lu bits=%u\n",ret,size,(unsigned)(sizeof(void*)*8));
    if (ret==ERROR_BUFFER_OVERFLOW && size && size<1024*1024) {
        unsigned char *data=calloc(1,size+16);memset(data+size,0xa5,16);
        ULONG bytes=size;ret=GetAdaptersInfo((IP_ADAPTER_INFO *)data,&bytes);
        if (ret) failures++;
        for (unsigned i=0;i<16;i++) if (data[size+i]!=0xa5) failures++;
        for (IP_ADAPTER_INFO *p=(IP_ADAPTER_INFO *)data;!ret&&p;p=p->Next) {
            if ((uintptr_t)p<(uintptr_t)data || (uintptr_t)p+sizeof(*p)>(uintptr_t)data+size || ++count>1024) {failures++;break;}
            MIB_IF_ROW2 row={0};row.InterfaceIndex=p->Index;
            MIB_IPINTERFACE_ROW ip={0};ip.Family=AF_INET;ip.InterfaceIndex=p->Index;
            if (GetIfEntry2(&row) || row.Type==IF_TYPE_SOFTWARE_LOOPBACK || GetIpInterfaceEntry(&ip)) failures++;
        }
        fprintf(out,"allocated-query ret=%lu count=%u guard=checked\n",ret,count);
        if (count!=expected) failures++;
        unsigned char tiny[sizeof(IP_ADAPTER_INFO)+16];memset(tiny,0xa5,sizeof(tiny));
        bytes=1;ret=GetAdaptersInfo((IP_ADAPTER_INFO *)tiny,&bytes);
        if (ret!=ERROR_BUFFER_OVERFLOW || bytes!=size) failures++;
        for (unsigned i=1;i<sizeof(tiny);i++) if (tiny[i]!=0xa5) failures++;
        free(data);
    } else if (ret!=ERROR_NO_DATA || expected) failures++;
    unsigned char fixed[0x2800+16];memset(fixed,0xa5,sizeof(fixed));ULONG bytes=0x2800;
    ret=GetAdaptersInfo((IP_ADAPTER_INFO *)fixed,&bytes);
    if (expected && ret!=(size<=0x2800?NO_ERROR:ERROR_BUFFER_OVERFLOW)) failures++;
    for (unsigned i=0x2800;i<sizeof(fixed);i++) if (fixed[i]!=0xa5) failures++;
    fprintf(out,"fixed-query ret=%lu bytes=%lu\n",ret,bytes);
    ret=GetIpForwardTable2(AF_INET,&routes);
    fprintf(out,"routes ret=%lu count=%lu\n",ret,ret?0:routes->NumEntries);
    if (ret) failures++;
    if (links) FreeMibTable(links);if (ipv4) FreeMibTable(ipv4);if (routes) FreeMibTable(routes);
    fprintf(out,"RESULT failures=%u\n",failures);fclose(out);return failures?1:0;
}
