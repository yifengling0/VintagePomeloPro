"""ASan/UBSan fixtures compile the real musl DNS overlay functions."""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=Path(os.environ.get('WINE_SRC', ROOT / 'thirdparty/wine-valve')))
args = parser.parse_args()


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


def run_c(tree, name, code):
    source = tree / (name + '.c')
    binary = tree / name
    source.write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('PASS', name)


COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int NTSTATUS;
'''
with tempfile.TemporaryDirectory(prefix='dns-transport-contract-') as folder:
    tree = Path(folder)
    name = 'dlls/dnsapi/libresolv_musl.c'
    target = tree / name
    target.parent.mkdir(parents=True)
    shutil.copy2(args.wine_src / name, target)
    patch = (ROOT / 'patches/wine/0067-dnsapi-musl-transport-safety.patch').read_bytes()
    subprocess.run(['patch', '-p1', '--batch', '--force'], input=patch, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['patch', '-p1', '--batch', '--force', '--dry-run', '-R'], input=patch, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    text = target.read_text()
    transport = ''.join(function(text, sig) for sig in ['static int64_t dns_now_ms(', 'static int dns_wait(', 'static int dns_transfer(', 'static int tcp_query_one('])
    run_c(tree, 'tcp-shorts-and-deadline', COMMON + r'''
static int64_t now;
static unsigned polls, sends, recvs, close_count, received, written;
static int socket_error_value, interrupted_poll, slow_poll;
static int first_budget, last_budget;
static unsigned char wire[14]={0,12,0x12,0x34,0x80}, output[64];
static int fake_clock_gettime(clockid_t c,struct timespec *t) { assert(c==CLOCK_MONOTONIC);t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0; }
static int fake_socket(int f,int t,int p) { assert(f==AF_INET && (t&SOCK_STREAM));(void)p;return 8; }
static int fake_fcntl(int fd,int cmd,int arg) { (void)cmd;(void)arg;assert(fd==8);return 0; }
static int fake_connect(int fd,const struct sockaddr *sa,socklen_t size) { assert(fd==8 && sa->sa_family==AF_INET && size==sizeof(struct sockaddr_in));errno=EINPROGRESS;return -1; }
static int fake_poll(struct pollfd *p,nfds_t n,int budget) {
    assert(n==1 && p->fd==8 && budget>0);if(!polls)first_budget=budget;last_budget=budget;polls++;
    if(interrupted_poll && polls==1){now++;errno=EINTR;return -1;}
    if(slow_poll){now+=budget;return 0;} now++;p->revents=p->events;return 1;
}
static int fake_getsockopt(int fd,int l,int o,void *v,socklen_t *n) { assert(fd==8 && l==SOL_SOCKET && o==SO_ERROR && *n==sizeof(int));*(int *)v=socket_error_value;return 0; }
static ssize_t fake_send(int fd,const void *buf,size_t n,int flags) {
    assert(fd==8 && flags==MSG_NOSIGNAL);sends++;
    if(sends==1){errno=EINTR;return -1;}if(sends==2){errno=EAGAIN;return -1;}
    if(n>3){n=3;}assert(written+n<=sizeof(output));memcpy(output+written,buf,n);written+=n;return n;
}
static ssize_t fake_recv(int fd,void *buf,size_t n,int flags) {
    assert(fd==8 && !flags);recvs++;
    if(recvs==1){errno=EAGAIN;return -1;}if(recvs==2){errno=EINTR;return -1;}
    if(n>2){n=2;}assert(received+n<=sizeof(wire));memcpy(buf,wire+received,n);received+=n;return n;
}
static int fake_close(int fd) { assert(fd==8);close_count++;return 0; }
#define clock_gettime fake_clock_gettime
#define socket fake_socket
#define fcntl fake_fcntl
#define connect fake_connect
#define poll fake_poll
#define getsockopt fake_getsockopt
#define send fake_send
#define recv fake_recv
#define close fake_close
''' + transport + r'''
int main(void) {
    struct sockaddr_in sa={.sin_family=AF_INET}; unsigned char query[9]={1,2,3,4,5,6,7,8,9}, reply[16]={0};
    interrupted_poll=1;
    assert(tcp_query_one((struct sockaddr *)&sa,sizeof(sa),query,9,reply,sizeof(reply),0x1234,100)==12);
    assert(close_count==1 && received==14 && written==11 && output[0]==0 && output[1]==9 && !memcmp(output+2,query,9));
    assert(first_budget==100 && last_budget<first_budget && now<100 && sends>3 && recvs>3);
    now=0;polls=sends=recvs=0;socket_error_value=ECONNREFUSED;
    assert(tcp_query_one((struct sockaddr *)&sa,sizeof(sa),query,9,reply,sizeof(reply),0x1234,100)==-1 && !sends && !recvs && close_count==2);
    now=0;polls=0;socket_error_value=0;slow_poll=1;interrupted_poll=0;
    assert(tcp_query_one((struct sockaddr *)&sa,sizeof(sa),query,9,reply,sizeof(reply),0x1234,7)==-1 && now==7 && close_count==3);
    return 0;
}
''')
    run_c(tree, 'udp-tc-and-family', COMMON + r'''
static struct sockaddr *expected;
static unsigned udp_calls,tcp_calls;
static int truncated=1;
static int udp_query_one(struct sockaddr *sa,socklen_t len,const unsigned char *q,int qn,unsigned char *r,int rn,unsigned short id,int t) {
    assert(sa==expected && len==(sa->sa_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6)));
    (void)q;(void)qn;(void)rn;(void)id;(void)t;udp_calls++;r[2]=truncated?2:0;return 12;
}
static int tcp_query_one(struct sockaddr *sa,socklen_t len,const unsigned char *q,int qn,unsigned char *r,int rn,unsigned short id,int t) {
    assert(sa==expected && len==(sa->sa_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6)));
    (void)q;(void)qn;(void)r;(void)rn;(void)id;(void)t;tcp_calls++;return 15;
}
''' + function(text, 'static socklen_t dns_sockaddr_len(') + function(text, 'static int query_server(') + r'''
int main(void) {
    struct sockaddr_in a4={.sin_family=AF_INET};struct sockaddr_in6 a6={.sin6_family=AF_INET6};unsigned char q[12]={0},r[32]={0};
    expected=(struct sockaddr *)&a4;assert(query_server(expected,q,12,r,32,1,100,0)==15 && udp_calls==1 && tcp_calls==1);
    expected=(struct sockaddr *)&a6;assert(query_server(expected,q,12,r,32,1,100,0)==15 && udp_calls==2 && tcp_calls==2);
    truncated=0;assert(query_server(expected,q,12,r,32,1,100,0)==12 && tcp_calls==2);
    assert(query_server(expected,q,12,r,32,1,100,1)==15 && udp_calls==3 && tcp_calls==3);
    return 0;
}
''')
    run_c(tree, 'synthesis-size-and-caller', COMMON + r'''
#define ERROR_INVALID_NAME 123
#define ERROR_MORE_DATA 234
#define ERROR_SUCCESS 0
#define DNS_ERROR_RCODE_NAME_ERROR 9003
#define DNS_ERROR_RCODE_SERVER_FAILURE 9002
#define DNS_QUERY_USE_TCP_ONLY 2
struct query_params { const char *name; WORD type; DWORD options; void *buf; DWORD *len; };
static unsigned count=300,freed;
static int fake_getaddrinfo(const char *name,const char *service,const struct addrinfo *hints,struct addrinfo **out) {
    (void)name;(void)service;struct addrinfo **tail=out;*out=NULL;
    for(unsigned i=0;i<count;i++){
        struct addrinfo *p=calloc(1,sizeof(*p));assert(p);p->ai_family=hints->ai_family;
        p->ai_addrlen=hints->ai_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6);
        p->ai_addr=calloc(1,p->ai_addrlen);assert(p->ai_addr);p->ai_addr->sa_family=hints->ai_family;
        if(hints->ai_family==AF_INET)((struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr=htonl(i+1);
        else ((struct sockaddr_in6 *)p->ai_addr)->sin6_addr.s6_addr[15]=(i+1)&255;
        *tail=p;tail=&p->ai_next;
    }return 0;
}
static void fake_freeaddrinfo(struct addrinfo *p) {freed++;while(p){struct addrinfo *next=p->ai_next;free(p->ai_addr);free(p);p=next;}}
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
''' + function(text, 'static int synthesize_from_getaddrinfo(') + r'''
static struct sockaddr_in g_override[1];static int g_override_count;
static struct {int server_count;struct sockaddr_storage servers[1];} g_config;
static unsigned short next_id(void) {return 123;}
static void ensure_config(void) {}
static int map_rcode(int n) {return n;}
static int query_server(struct sockaddr *sa,const unsigned char *q,int qn,unsigned char *r,int rn,unsigned short id,int t,int force) {
    (void)sa;assert(qn>=12 && q[10]==0 && q[11]==0);(void)r;(void)rn;(void)id;(void)t;(void)force;return -1;
}
''' + '#define MAX_NAME_LEN 511\n' + function(text, 'static int encode_qname(') + function(text, 'static NTSTATUS resolv_query(') + r'''
int main(void) {
    unsigned char query[17]={0}, small[17], *large;DWORD required=0,len;struct query_params p={"test",1,0,small,&len};
    memset(small,0xa5,sizeof(small));
    assert(synthesize_from_getaddrinfo("test",1,small,sizeof(small),query,17,&required)==-2 && required==4817 && freed==1);
    for(unsigned i=0;i<sizeof(small);i++)assert(small[i]==0xa5);
    large=malloc(required);assert(large);
    assert(synthesize_from_getaddrinfo("test",1,large,required,query,17,&required)==4817);
    assert(large[6]==1 && large[7]==44 && large[17+11]==4 && large[17+15]==1 && large[17+299*16+14]==1 && large[17+299*16+15]==44);
    free(large);count=3;required=0;
    assert(synthesize_from_getaddrinfo("test",28,small,sizeof(small),query,17,&required)==-2 && required==101);
    large=malloc(required);assert(synthesize_from_getaddrinfo("test",28,large,required,query,17,&required)==101);
    assert(large[7]==3 && large[17+11]==16 && large[17+27]==1 && large[17+2*28+27]==3);free(large);
    g_config.server_count=1;len=sizeof(small);assert(resolv_query(&p)==ERROR_MORE_DATA && len==70);
    assert(freed==5);
    count=0;assert(synthesize_from_getaddrinfo("test",1,small,sizeof(small),query,17,&required)==-1 && freed==5);
    {
        unsigned char guarded[257];char name[512];size_t pos=0;
        memset(guarded,0xa5,sizeof(guarded));
        for(unsigned i=0;i<4;i++){unsigned n=i==3?61:63;memset(name+pos,'a',n);pos+=n;if(i<3)name[pos++]='.';}
        name[pos]=0;assert(encode_qname(guarded+1,name)==255 && guarded[0]==0xa5 && guarded[256]==0xa5);
        name[pos++]='a';name[pos]=0;memset(guarded,0xa5,sizeof(guarded));
        assert(encode_qname(guarded+1,name)==-1 && guarded[0]==0xa5 && guarded[256]==0xa5);
        memset(name,'a',sizeof(name));for(unsigned i=63;i<511;i+=64)name[i]='.';name[511]=0;
        assert(encode_qname(guarded+1,name)==-1 && guarded[256]==0xa5);
    }
    return 0;
}
''')
