"""Compile production overlay functions, not reimplementations, with syscall fixtures."""
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int BOOL;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef uint16_t INTERNET_PORT;
typedef uint32_t ULONG;
typedef uintptr_t UINT_PTR;
typedef void *HANDLE;
typedef struct timeval TIMEVAL;
typedef fd_set FD_SET;
#define TRUE 1
#define FALSE 0
#define ERROR_SUCCESS 0
#define ERROR_OUTOFMEMORY 14
#define ERROR_INVALID_PARAMETER 87
#define ERROR_WINHTTP_TIMEOUT 12002
#define ERROR_WINHTTP_CANNOT_CONNECT 12029
#define ERROR_WINHTTP_NAME_NOT_RESOLVED 12007
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 1
#define WSA_FLAG_OVERLAPPED 0
#define SOCKET_ERROR -1
#define INFINITE UINT32_MAX
static inline void trace(const char *fmt, ...) { (void)fmt; }
#define TRACE trace
#define ERR trace
#define WARN trace
struct hostdata { void *hostname; };
struct netconn { int refs, socket; struct hostdata *host; struct sockaddr_storage sockaddr; };
struct netconn_address { struct netconn_address *next; struct sockaddr_storage addr; };
typedef struct addrinfo_fake {
    int ai_family, ai_socktype;
    size_t ai_addrlen;
    struct sockaddr *ai_addr;
    struct addrinfo_fake *ai_next;
} ADDRINFOW;
'''

with tempfile.TemporaryDirectory(prefix='wine-connect-contract-') as folder:
    tree = Path(folder)
    names = ['dlls/winhttp/net.c', 'dlls/winhttp/request.c', 'dlls/winhttp/winhttp_private.h', 'dlls/wininet/utility.c']
    for name in names:
        target = tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.wine_src / name, target)
    patch = (ROOT / 'patches/wine/0066-winhttp-wininet-bounded-connect.patch').read_bytes()
    subprocess.run(['patch', '-p1', '--batch', '--force'], input=patch, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['patch', '-p1', '--batch', '--force', '--dry-run', '-R'], input=patch, cwd=tree, check=True, stdout=subprocess.DEVNULL)
    net = (tree / names[0]).read_text()
    utility = (tree / names[3]).read_text()
    run_c(tree, 'connect-so-error', COMMON + r'''
static int active_sockets, ioctl_calls, ioctl_fail_at, connect_calls;
static void winsock_init(void) {}
static int WSASocketW(int family, int type, int protocol, void *p, int group, int flags) {
    (void)p; (void)group; (void)flags; int fd=socket(family, type, protocol);if(fd>=0)active_sockets++;return fd;
}
static int WSAGetLastError(void) { return errno; }
static int SetFileCompletionNotificationModes(HANDLE h, int f) { (void)h; (void)f; return 1; }
static void closesocket(int fd) { close(fd);active_sockets--; }
static int ioctlsocket(int fd, int op, ULONG *value) { if(++ioctl_calls==ioctl_fail_at){errno=EIO;return -1;}return ioctl(fd, op, value); }
static int tracked_connect(int fd,const struct sockaddr *addr,socklen_t len) {connect_calls++;return connect(fd,addr,len);}
#define connect tracked_connect
static int socket_error(int fd, int level, int opt, char *val, int *len) {
    socklen_t size = *len; int ret = getsockopt(fd, level, opt, val, &size); *len = size; return ret;
}
#define getsockopt socket_error
''' + function(net, 'static int set_blocking(') + function(net, 'DWORD netconn_create(') + r'''
int main(void) {
    struct sockaddr_in address = { .sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(address);
    struct sockaddr_storage storage = {0};
    struct netconn *conn = NULL;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0 && !bind(listener, (struct sockaddr *)&address, sizeof(address)));
    assert(!getsockname(listener, (struct sockaddr *)&address, &len));
    memcpy(&storage, &address, sizeof(address));
    /* A bound, non-listening port refuses TCP without an ephemeral-port race. */
    assert(netconn_create(NULL, &storage, 1000, &conn) == ERROR_WINHTTP_CANNOT_CONNECT && !conn);
    assert(!listen(listener, 1));
    assert(netconn_create(NULL, &storage, 1000, &conn) == 0 && conn);
    closesocket(conn->socket); free(conn);conn=NULL;
    int accepted=accept(listener,NULL,NULL);assert(accepted>=0);close(accepted);
    ioctl_calls=connect_calls=0;ioctl_fail_at=1;
    assert(netconn_create(NULL,&storage,1000,&conn)==ERROR_WINHTTP_CANNOT_CONNECT && !conn && !connect_calls && !active_sockets);
    ioctl_calls=connect_calls=0;ioctl_fail_at=2;
    assert(netconn_create(NULL,&storage,1000,&conn)==ERROR_WINHTTP_CANNOT_CONNECT && !conn && connect_calls==1 && !active_sockets);
    close(listener);
    return 0;
}
''')
    collect = function(net, 'void netconn_free_addresses(') + function(net, 'static BOOL same_address(') + function(net, 'static DWORD collect_addresses(')
    run_c(tree, 'candidate-budget', COMMON + collect + r'''
static ULONGLONG now;
static unsigned attempts, fail_count, resolutions;
static int budgets[8], modes[8];
static DWORD resolve_error;
static struct sockaddr_storage a4(unsigned value) {
    struct sockaddr_storage out = {0}; struct sockaddr_in *a=(struct sockaddr_in *)&out;
    a->sin_family=AF_INET; a->sin_addr.s_addr=htonl(value); a->sin_port=htons(443); return out;
}
static struct netconn_address *new_list(unsigned count) {
    struct netconn_address *head=NULL;
    for (unsigned n=count; n; n--) { struct netconn_address *a=calloc(1,sizeof(*a)); assert(a); a->addr=a4(n); a->next=head; head=a; }
    return head;
}
static ULONGLONG GetTickCount64(void) { return now; }
static DWORD netconn_create(struct hostdata *host, const struct sockaddr_storage *addr, int timeout, struct netconn **out) {
    (void)host; assert(attempts < 8); budgets[attempts]=timeout;
    modes[attempts]=ntohl(((const struct sockaddr_in *)addr)->sin_addr.s_addr); attempts++;
    if (attempts <= fail_count) { now += timeout > 0 && timeout < 40 ? timeout : 40; return ERROR_WINHTTP_CANNOT_CONNECT; }
    *out=calloc(1,sizeof(**out)); (*out)->sockaddr=*addr; return 0;
}
static DWORD netconn_resolve(void *name, INTERNET_PORT port, struct netconn_address **out, int timeout) {
    (void)name; (void)port; (void)timeout; resolutions++; if(resolve_error) return resolve_error; *out=new_list(3); return 0;
}
''' + function(net, 'DWORD netconn_create_multi(') + r'''
int main(void) {
    struct sockaddr_storage values[5] = {a4(1),a4(2),a4(1),{0},{0}};
    struct sockaddr_in6 *v6=(struct sockaddr_in6 *)&values[3];
    v6->sin6_family=AF_INET6; v6->sin6_addr.s6_addr[15]=3;
    values[4]=values[3]; ((struct sockaddr_in6 *)&values[4])->sin6_scope_id=2;
    ADDRINFOW ai[6]={0}; int order[]={3,0,4,1,2};
    for(int i=0;i<5;i++) { ai[i].ai_family=values[order[i]].ss_family; ai[i].ai_addr=(struct sockaddr *)&values[order[i]]; ai[i].ai_addrlen=sizeof(values[0]); ai[i].ai_next=&ai[i+1]; }
    ai[5].ai_family=AF_UNIX;
    struct netconn_address *list=NULL, *p; struct netconn *conn=NULL; struct hostdata host={0};
    assert(!collect_addresses(ai,443,&list)); p=list;
    assert(same_address(&p->addr,&values[0])); p=p->next; assert(same_address(&p->addr,&values[1])); p=p->next;
    assert(p->addr.ss_family==AF_INET6 && p->next && !p->next->next); netconn_free_addresses(list);
    fail_count=1; assert(!netconn_create_multi(&host,NULL,new_list(3),443,0,100,&conn));
    assert(attempts==2 && budgets[0]==100 && budgets[1]==60); free(conn);
    attempts=0; now=0; fail_count=9;
    assert(netconn_create_multi(&host,NULL,new_list(5),443,0,75,&conn)==ERROR_WINHTTP_TIMEOUT);
    assert(attempts==2 && now==75 && budgets[1]==35);
    for(int timeout=-1;timeout<=0;timeout++) { attempts=0; now=0; fail_count=1;
        assert(!netconn_create_multi(&host,NULL,new_list(2),443,0,timeout,&conn));
        assert(budgets[0]==timeout && budgets[1]==timeout); free(conn); }
    attempts=0; now=0; resolutions=0; fail_count=1;
    assert(!netconn_create_multi(&host,&values[0],NULL,443,0,100,&conn));
    assert(resolutions==1 && attempts==2 && modes[0]==1 && modes[1]==2); free(conn);
    attempts=0; now=0; resolutions=0; fail_count=9; resolve_error=ERROR_WINHTTP_NAME_NOT_RESOLVED;
    assert(netconn_create_multi(&host,&values[0],NULL,443,0,100,&conn)==resolve_error && attempts==1);
    return 0;
}
''')
    async_code = net[net.index('struct async_resolve\n'):net.index('/* Called only before the request owns')]
    run_c(tree, 'resolver-ownership', COMMON + r'''
#include <wchar.h>
typedef wchar_t WCHAR;
typedef int LONG, TP_CALLBACK_INSTANCE;
#define CALLBACK
#define WAIT_OBJECT_0 0
static unsigned allocations, releases;
static void *counted_malloc(size_t n) { void *p=malloc(n);if(p)allocations++;return p; }
static void *counted_calloc(size_t n,size_t z) { void *p=calloc(n,z);if(p)allocations++;return p; }
static void counted_free(void *p) { if(p)releases++;free(p); }
static WCHAR *counted_wcsdup(const WCHAR *s) { size_t n=(wcslen(s)+1)*sizeof(*s);WCHAR *p=counted_malloc(n);if(p)memcpy(p,s,n);return p; }
#define malloc counted_malloc
#define calloc counted_calloc
#define free counted_free
#define wcsdup counted_wcsdup
static LONG InterlockedIncrement(LONG *v) { return ++*v; }
static LONG InterlockedDecrement(LONG *v) { return --*v; }
static HANDLE CreateEventW(void *a,int b,int c,void *d) { (void)a;(void)b;(void)c;(void)d;return calloc(1,sizeof(int)); }
static void CloseHandle(HANDLE h) {free(h);}
static void SetEvent(HANDLE h) {*(int *)h=1;}
static void (*pending)(TP_CALLBACK_INSTANCE *,void *);
static void *pending_ctx;
static int submit_fail,timeout_wait,resolve_fail;
static int TrySubmitThreadpoolCallback(void (*fn)(TP_CALLBACK_INSTANCE *,void *),void *ctx,void *env) {
    (void)env;if(submit_fail)return 0;pending=fn;pending_ctx=ctx;return 1;
}
static void finish_worker(void) { assert(pending);pending(NULL,pending_ctx);pending=NULL;pending_ctx=NULL; }
static DWORD WaitForSingleObject(HANDLE h,int timeout) { (void)timeout;if(timeout_wait)return 258;finish_worker();assert(*(int *)h);return WAIT_OBJECT_0; }
static DWORD GetLastError(void) {return 42;}
static DWORD resolve_hostname(const WCHAR *h,INTERNET_PORT p,struct netconn_address **out) {
    (void)h;(void)p;if(resolve_fail)return ERROR_WINHTTP_NAME_NOT_RESOLVED;
    *out=calloc(1,sizeof(**out));assert(*out);return 0;
}
''' + function(net, 'void netconn_free_addresses(') + async_code + r'''
int main(void) {
    struct netconn_address *addresses=(void *)1;
    assert(!netconn_resolve(L"test",443,&addresses,100) && addresses && !pending);
    assert(allocations==releases+1);netconn_free_addresses(addresses);assert(allocations==releases);
    timeout_wait=1;addresses=(void *)1;
    assert(netconn_resolve(L"test",443,&addresses,1)==ERROR_WINHTTP_TIMEOUT && !addresses && pending);
    assert(allocations>releases);finish_worker();assert(allocations==releases);
    timeout_wait=0;submit_fail=1;addresses=(void *)1;
    assert(netconn_resolve(L"test",443,&addresses,10)==42 && !addresses && !pending && allocations==releases);
    submit_fail=0;resolve_fail=1;addresses=(void *)1;
    assert(netconn_resolve(L"test",443,&addresses,10)==ERROR_WINHTTP_NAME_NOT_RESOLVED && !addresses && allocations==releases);
    resolve_fail=0;assert(!netconn_resolve(L"test",443,&addresses,0));netconn_free_addresses(addresses);
    assert(allocations==releases);return 0;
}
''')

    # Execute the unchanged WinINet multi-address and promotion logic with its
    # real corrected timeval expression, avoiding any Windows-only ABI tricks.
    run_c(tree, 'wininet-budget', COMMON + r'''
typedef struct server_addr { struct sockaddr_storage addr; int addr_len; char addr_str[64]; struct server_addr *next; } server_addr_t;
typedef int object_header_t; typedef uintptr_t DWORD_PTR;
typedef struct { long long QuadPart; } LARGE_INTEGER;
static unsigned calls, closed;
static long long tick;
static TIMEVAL seen[8];
static void QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart=1000; }
static void QueryPerformanceCounter(LARGE_INTEGER *v) { v->QuadPart=tick; }
static void INTERNET_SendCallback(object_header_t *h,DWORD_PTR c,int s,char *a,size_t n) { (void)h;(void)c;(void)s;(void)a;(void)n; }
#define INTERNET_STATUS_CONNECTING_TO_SERVER 1
#define INTERNET_STATUS_CONNECTED_TO_SERVER 2
static int fake_socket(int f,int t,int p) { (void)f;(void)t;(void)p; return 5; }
static int fake_connect(int s,const struct sockaddr *a,int l) { (void)s;(void)a;(void)l; return -1; }
static int WSAGetLastError(void) { return WSAEINPROGRESS; }
static int ioctlsocket(int s,int o,ULONG *v) { (void)s;(void)o;(void)v;return 0; }
static void closesocket(int s) { (void)s; closed++; }
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *tv) {
    (void)n;(void)r;(void)w;(void)e; if(tv) { assert(tv->tv_usec<1000000);seen[calls]=*tv; } else seen[calls].tv_sec=-1;
    calls++; tick+=100; return 1;
}
static int fake_getsockopt(int s,int l,int o,void *v,socklen_t *len) { (void)s;(void)l;(void)o;(void)len; *(int *)v=calls==1?ECONNREFUSED:0; return 0; }
#define socket fake_socket
#define connect fake_connect
#define select fake_select
#define getsockopt fake_getsockopt
''' + function(utility, 'static int try_create_connect_socket(') + function(utility, 'int create_connect_socket(') + r'''
int main(void) {
    server_addr_t a={0}, b={0}; a.next=&b; a.addr.ss_family=b.addr.ss_family=AF_INET;
    strcpy(a.addr_str,"first");strcpy(b.addr_str,"second");
    assert(create_connect_socket(&a,AF_UNSPEC,2501,NULL,0)==5);
    assert(calls==2 && closed==1 && seen[0].tv_sec==2 && seen[0].tv_usec==501000);
    assert(seen[1].tv_sec==2 && seen[1].tv_usec==401000 && !strcmp(a.addr_str,"second"));
    calls=0;tick=0;assert(create_connect_socket(&a,AF_UNSPEC,50,NULL,0)==-1 && calls==1);
    calls=1;assert(create_connect_socket(&a,AF_UNSPEC,INFINITE,NULL,0)==5 && seen[1].tv_sec==-1);
    return 0;
}
''')
    request = (tree / names[1]).read_text()
    send_request = function(request, 'static DWORD send_request(')
    assert send_request.count('open_connection( request )') == 1
    assert send_request.index('open_connection( request )') < send_request.index('netconn_send(')
    assert request.count('netconn_create_multi(') == 1
    assert 'netconn_create_multi(' in function(request, 'static DWORD open_connection(')
    run_c(tree, 'pre-send-candidate-boundary', r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
typedef int BOOL; typedef unsigned DWORD; typedef wchar_t WCHAR; typedef unsigned short INTERNET_PORT;
#define TRUE 1
#define FALSE 0
#define ERROR_SUCCESS 0
#define ERROR_OUTOFMEMORY 14
#define ERROR_WINHTTP_SECURE_FAILURE 12175
#define WINHTTP_FLAG_SECURE 1
#define WINHTTP_CALLBACK_STATUS_RESOLVING_NAME 1
#define WINHTTP_CALLBACK_STATUS_NAME_RESOLVED 2
#define WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER 3
#define WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER 4
#define TRACE(...) ((void)0)
struct sockaddr_storage { unsigned ss_family; };
struct list { int unused; };
struct hdr { int flags; };
struct hostdata { unsigned port; WCHAR *hostname; int secure,ref; struct list entry,connections; };
struct netconn { struct list entry; struct sockaddr_storage sockaddr; int secure; };
struct netconn_address { struct sockaddr_storage addr; };
struct session { int proxy_server; };
struct connect { int resolved; INTERNET_PORT serverport; struct sockaddr_storage sockaddr; WCHAR *servername,*hostname; struct session *session; };
struct request { struct hdr hdr; struct netconn *netconn; struct connect *connect; int resolve_timeout,connect_timeout,send_timeout,security_flags,cred_handle,check_revocation; void *server_cert; unsigned read_pos,read_size,read_chunked,read_chunked_size,read_chunked_eof; };
static struct hostdata global_host={443,L"test",1,1,{0},{0}};
static int connection_pool_cs, creates, released, tls_error, proxy_error;
#define LIST_FOR_EACH_ENTRY(it,list,type,field) for(it=&global_host;it;it=NULL)
#define LIST_ENTRY(p,type,field) ((struct netconn *)(p))
static void EnterCriticalSection(int *v) { (void)v; }
static void LeaveCriticalSection(int *v) { (void)v; }
static void list_init(struct list *v) { (void)v; }
static void list_add_head(void *a,void *b) { (void)a;(void)b; }
static void *list_head(struct list *l) { return l; }
static int list_empty(struct list *l) { (void)l;return 1; }
static void list_remove(struct list *l) { (void)l; }
static int connection_pool;
static int lstrlenW(const WCHAR *s) { return wcslen(s); }
static int wcsicmp(const WCHAR *a,const WCHAR *b) { return wcscmp(a,b); }
static void release_host(struct hostdata *h) { (void)h; }
static int netconn_is_alive(struct netconn *c) { (void)c;return 1; }
static void netconn_release(struct netconn *c) { free(c);released++; }
static void send_callback(struct hdr *h,int s,void *p,DWORD n) { (void)h;(void)s;(void)p;(void)n; }
static int netconn_resolve(WCHAR *h,INTERNET_PORT p,struct netconn_address **out,int t) { (void)h;(void)p;(void)t;*out=calloc(1,sizeof(**out));return 0; }
static void netconn_free_addresses(struct netconn_address *a) { free(a); }
static WCHAR *addr_to_str(struct sockaddr_storage *a) { (void)a;return wcsdup(L"address"); }
static int netconn_create_multi(struct hostdata *h,struct sockaddr_storage *c,struct netconn_address *a,INTERNET_PORT p,int r,int t,struct netconn **out) {
    (void)h;(void)c;(void)p;(void)r;(void)t;free(a);creates++;*out=calloc(1,sizeof(**out));return 0;
}
static void netconn_set_timeout(struct netconn *c,int b,int t) { (void)c;(void)b;(void)t; }
static int request_receive_response_timeout(struct request *r) { (void)r;return 0; }
static int secure_proxy_connect(struct request *r) { (void)r;return proxy_error; }
static void CertFreeCertificateContext(void *p) { (void)p; }
static int ensure_cred_handle(struct request *r) { (void)r;return 0; }
static int netconn_secure_connect(struct netconn *c,WCHAR *h,int f,int *v,int r) { (void)c;(void)h;(void)f;(void)v;(void)r;return tls_error; }
static void *netconn_get_certificate(struct netconn *c) { (void)c;return (void *)1; }
''' + function(request, 'static DWORD open_connection(') + r'''
int main(void) {
    struct session session={0}; struct connect connect={1,443,{2},L"test",L"test",&session};
    struct request request={0};request.hdr.flags=1;request.connect=&connect;
    tls_error=ERROR_WINHTTP_SECURE_FAILURE;
    assert(open_connection(&request)==ERROR_WINHTTP_SECURE_FAILURE && creates==1 && released==1 && !request.netconn);
    /* Proxy CONNECT is HTTP: its failure must not cycle endpoint candidates. */
    tls_error=0;proxy_error=123;session.proxy_server=1;connect.hostname=L"target";
    assert(open_connection(&request)==123 && creates==2 && released==2 && !request.netconn);
    session.proxy_server=0;proxy_error=0;
    assert(!open_connection(&request) && creates==3 && request.netconn);
    /* An existing socket bypasses candidate creation. This is not a simulated
       HTTP send/read error and does not assert a higher-level replay policy. */
    assert(!open_connection(&request) && creates==3);
    netconn_release(request.netconn);
    return 0;
}
''')
    smoke = (ROOT / 'smoke/winehua_platform_network.c').read_text()
    run_c(tree, 'smoke-nonblocking-failure', COMMON + r'''
typedef int SOCKET;
#define INVALID_SOCKET -1
#define WSAHOST_NOT_FOUND 11001
#define WSAETIMEDOUT 10060
typedef struct addrinfo_a { int ai_family,ai_socktype,ai_protocol;struct sockaddr *ai_addr;size_t ai_addrlen;struct addrinfo_a *ai_next; } ADDRINFOA;
static unsigned connected,closed,freed;
static struct sockaddr_in address={.sin_family=AF_INET};
static ADDRINFOA info={AF_INET,SOCK_STREAM,IPPROTO_TCP,(struct sockaddr *)&address,sizeof(address),NULL};
static ULONGLONG GetTickCount64(void) {return 100;}
static int GetAddrInfoA(const char *h,const char *p,const ADDRINFOA *i,ADDRINFOA **out) {(void)h;(void)p;(void)i;*out=&info;return 0;}
static void FreeAddrInfoA(ADDRINFOA *p) {assert(p==&info);freed++;}
static int fake_socket(int f,int t,int p) {(void)f;(void)t;(void)p;return 8;}
static int ioctlsocket(int s,int op,u_long *v) {assert(s==8 && op==FIONBIO && *v==1);return -1;}
static DWORD WSAGetLastError(void) {return 10013;}
static void closesocket(int s) {assert(s==8);closed++;}
static int fake_connect(int s,const struct sockaddr *a,int len) {(void)s;(void)a;(void)len;connected++;return -1;}
static int fake_getsockopt(int s,int l,int o,char *p,int *n) {(void)s;(void)l;(void)o;(void)p;(void)n;return -1;}
#define socket fake_socket
#define connect fake_connect
#define getsockopt fake_getsockopt
''' + function(smoke, 'static DWORD probe_tcp(') + r'''
int main(void) {
    DWORD family=0;ULONGLONG elapsed=0;
    assert(probe_tcp("test",&family,&elapsed)==10013 && family==AF_INET && !connected && closed==1 && freed==1);
    return 0;
}
''')
