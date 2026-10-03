/* Inject failures around the production transport; real sockets own real fds. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int send_calls, receive_calls, successful_rights, inject_send, inject_receive, fail_after_first;
static ssize_t fault_sendmsg(int fd, const struct msghdr *message, int flags)
{
    struct msghdr copy = *message;
    struct iovec iov = message->msg_iov[0];
    ssize_t result;
    ++send_calls;
    if (inject_send && send_calls <= 2) { errno = send_calls == 1 ? EINTR : EAGAIN; return -1; }
    if (fail_after_first && send_calls > 1) { errno = EIO; return -1; }
    if (inject_send && iov.iov_len > 1) iov.iov_len = 1;
    copy.msg_iov = &iov; copy.msg_iovlen = 1;
    result = sendmsg(fd, &copy, flags);
    if (result > 0 && message->msg_controllen) ++successful_rights;
    return result;
}
static ssize_t fault_recvmsg(int fd, struct msghdr *message, int flags)
{
    ++receive_calls;
    if (inject_receive && receive_calls <= 2) { errno = receive_calls == 1 ? EINTR : EAGAIN; return -1; }
    if (inject_receive && message->msg_iov[0].iov_len > 1) message->msg_iov[0].iov_len = 1;
    return recvmsg(fd, message, flags);
}
#define sendmsg fault_sendmsg
#define recvmsg fault_recvmsg
#include "proc/spawn_protocol.h"
#undef sendmsg
#undef recvmsg

static int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd"); struct dirent *entry; int count = 0;
    assert(directory);
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
static void reset(void)
{ send_calls = receive_calls = successful_rights = inject_send = inject_receive = fail_after_first = 0; }
static const char *args[] = {"wine", "", "a|b\nUTF-8:中文"};
static const char *env[] = {"TOKEN=private|value\n中文"};
static const char *names[] = {"wineserver_sock"};
static vp_spawn_spec spec = {"", "/bin", args, env, names, 3, 1, 1};
struct writer { int socket, passed; };
static void *send_request(void *context)
{
    struct writer *writer = context;
    int result = vp_send_request(writer->socket, &spec, &writer->passed, vp_now_ms() + 1000);
    assert(result == (fail_after_first ? -1 : 0));
    if (fail_after_first) shutdown(writer->socket, SHUT_WR);
    return NULL;
}
static void fragments(void)
{
    int pair[2], original = open("/dev/null", O_RDONLY); pthread_t thread;
    vp_spawn_spec decoded; vp_received_fds received = {{0}, 0};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair) && original >= 0);
    reset(); inject_send = inject_receive = 1;
    struct writer writer = {pair[0], original};
    assert(!pthread_create(&thread, NULL, send_request, &writer));
    assert(!vp_recv_request(pair[1], &decoded, &received, vp_now_ms() + 1000));
    assert(!pthread_join(thread, NULL));
    assert(successful_rights == 1 && send_calls > 12 && receive_calls > 12);
    assert(received.count == 1 && (fcntl(received.items[0], F_GETFD) & FD_CLOEXEC));
    assert(decoded.argc == 3 && !decoded.argv[1][0] && !strcmp(decoded.argv[2], args[2]));
    assert(decoded.envc == 1 && !strcmp(decoded.env[0], env[0]));
    assert(fcntl(original, F_GETFD) >= 0);
    vp_free_spec(&decoded); vp_close_fds(&received); close(original); close(pair[0]); close(pair[1]);
}
static void failed_write(void)
{
    int pair[2], original = open("/dev/null", O_RDONLY); pthread_t thread;
    vp_spawn_spec decoded; vp_received_fds received = {{0}, 0};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    reset(); fail_after_first = 1; inject_send = 1;
    /* Start after the injected pre-write failures: one byte transfers the fd. */
    send_calls = 2; fail_after_first = 0;
    assert(!vp_send_bytes(pair[0], "V", 1, &original, 1, vp_now_ms()+1000));
    fail_after_first = 1;
    struct writer writer = {pair[0], original};
    assert(!pthread_create(&thread, NULL, send_request, &writer));
    assert(vp_recv_request(pair[1], &decoded, &received, vp_now_ms()+1000) == -1);
    assert(!pthread_join(thread, NULL));
    assert(received.count == 1); vp_free_spec(&decoded); vp_close_fds(&received);
    close(original); close(pair[0]); close(pair[1]);
}
static void ancillary_errors(void)
{
    int pair[2], originals[17]; vp_spawn_spec decoded; vp_received_fds received = {{0}, 0};
    union { char bytes[CMSG_SPACE(sizeof(originals))]; struct cmsghdr align; } control;
    struct iovec iov = {(void *)"VPBR", 4}; struct msghdr msg = {0}; struct cmsghdr *cmsg;
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair)); reset();
    for (int i=0; i<17; ++i) { originals[i] = open("/dev/null", O_RDONLY); assert(originals[i]>=0); }
    memset(&control, 0, sizeof(control)); msg.msg_iov=&iov; msg.msg_iovlen=1;
    msg.msg_control=control.bytes; msg.msg_controllen=sizeof(control.bytes); cmsg=CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level=SOL_SOCKET; cmsg->cmsg_type=SCM_RIGHTS; cmsg->cmsg_len=CMSG_LEN(sizeof(originals));
    memcpy(CMSG_DATA(cmsg), originals, sizeof(originals));
    assert(sendmsg(pair[0], &msg, MSG_NOSIGNAL)==4);
    assert(vp_recv_request(pair[1], &decoded, &received, vp_now_ms()+1000)==-1 && errno==EPROTO);
    vp_free_spec(&decoded); vp_close_fds(&received);
    for (int i=0; i<17; ++i) close(originals[i]);
    close(pair[0]); close(pair[1]);
    /* A second ancillary batch exceeding the total budget must also close all delivered fds. */
    received.count=0; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    int original=open("/dev/null", O_RDONLY); int many[10];
    for (int i=0; i<10; ++i) many[i]=original;
    unsigned char header[12]; memcpy(header,"VPBR",4); vp_store32(header+4,2); vp_store32(header+8,20);
    assert(!vp_send_bytes(pair[0],header,sizeof(header),many,10,vp_now_ms()+1000));
    assert(!vp_send_bytes(pair[0],"01234567890123456789",20,many,10,vp_now_ms()+1000));
    assert(vp_recv_request(pair[1],&decoded,&received,vp_now_ms()+1000)==-1 && errno==EPROTO);
    vp_free_spec(&decoded); vp_close_fds(&received); close(original); close(pair[0]); close(pair[1]);
}
static void malformed_headers(void)
{
    unsigned char header[12]; int pair[2]; vp_spawn_spec decoded; vp_received_fds received = {{0},0};
    reset();
    for (int mode=0; mode<3; ++mode) {
        assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
        memcpy(header, mode==0 ? "SPAW" : "VPBR", 4); vp_store32(header+4, mode==1 ? 3 : 2);
        vp_store32(header+8, VP_PAYLOAD_CAP+1);
        assert(!vp_send_bytes(pair[0],header,sizeof(header),NULL,0,vp_now_ms()+1000));
        assert(vp_recv_request(pair[1],&decoded,&received,vp_now_ms()+1000)==-1);
        assert(errno==(mode==2 ? E2BIG : EPROTONOSUPPORT)); vp_free_spec(&decoded);
        close(pair[0]); close(pair[1]);
    }
}
static void *trickle(void *context)
{
    int fd=*(int*)context; unsigned char bytes[12]; memcpy(bytes,"VPBR",4); vp_store32(bytes+4,2); vp_store32(bytes+8,20);
    for (size_t i=0; i<sizeof(bytes); ++i) {
        struct timespec delay={0,8000000}; nanosleep(&delay,NULL);
        if (send(fd,bytes+i,1,MSG_NOSIGNAL)!=1) break;
    }
    return NULL;
}
static void deadline(void)
{
    int pair[2]; pthread_t thread; vp_spawn_spec decoded; vp_received_fds received={{0},0}; reset();
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair)); assert(!pthread_create(&thread,NULL,trickle,&pair[0]));
    int64_t start=vp_now_ms();
    assert(vp_recv_request(pair[1],&decoded,&received,start+30)==-1 && errno==ETIMEDOUT);
    assert(vp_now_ms()-start < 300); vp_free_spec(&decoded);
    assert(!pthread_join(thread,NULL)); close(pair[0]); close(pair[1]);
}
int main(void)
{
    int baseline=fd_count(); fragments(); assert(fd_count()==baseline);
    failed_write(); assert(fd_count()==baseline); ancillary_errors(); assert(fd_count()==baseline);
    malformed_headers(); assert(fd_count()==baseline); deadline(); assert(fd_count()==baseline);
    puts("transport: byte fragments, EINTR/EAGAIN, short/failed writes, ancillary truncation/overflow, versions and total deadline PASS; no fd leaks");
}
