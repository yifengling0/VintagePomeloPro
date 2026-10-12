#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "network-route-linux.h"
#define SEQ 0x1234
union {struct nlmsghdr align;unsigned char bytes[2048];} data;
static void addattr(struct nlmsghdr *hdr,unsigned type,uint32_t value) {
    struct rtattr *r=(struct rtattr *)(data.bytes+NLMSG_ALIGN(hdr->nlmsg_len));
    r->rta_type=type;r->rta_len=RTA_LENGTH(sizeof(value));memcpy(RTA_DATA(r),&value,sizeof(value));
    hdr->nlmsg_len=NLMSG_ALIGN(hdr->nlmsg_len)+RTA_ALIGN(r->rta_len);
}
static size_t route(unsigned family,unsigned table,unsigned iface) {
    struct nlmsghdr *h=(struct nlmsghdr *)data.bytes;
    struct rtmsg *r;
    memset(&data,0,sizeof(data));h->nlmsg_len=NLMSG_LENGTH(sizeof(*r));h->nlmsg_seq=SEQ;h->nlmsg_type=RTM_NEWROUTE;
    r=NLMSG_DATA(h);r->rtm_family=family;r->rtm_type=RTN_UNICAST;r->rtm_table=table;r->rtm_protocol=RTPROT_STATIC;
    addattr(h,RTA_OIF,iface);addattr(h,RTA_GATEWAY,0x01020304);addattr(h,RTA_PRIORITY,123);
    return h->nlmsg_len;
}
static int parse(size_t len,uint32_t seq,struct network_ipv4_route **r,size_t *count) {
    int done=0;return network_ipv4_parse_routes(data.bytes,len,seq,r,count,&done);
}
int main(void) {
    struct network_ipv4_route *r=NULL;size_t count=0,len=route(AF_INET,RT_TABLE_MAIN,7);int done=0;
    assert(!parse(len,SEQ,&r,&count)&&count==1&&r[0].ifindex==7&&r[0].gateway==0x01020304&&r[0].metric==123);
    assert(!parse(len,SEQ,&r,&count)&&count==1); /* Duplicate table projection. */
    len=route(AF_INET,100,7);assert(!parse(len,SEQ,&r,&count)&&count==1);
    len=route(AF_INET,100,8);assert(!parse(len,SEQ,&r,&count));
#ifdef __OHOS__
    assert(count==2);
#else
    assert(count==1);
#endif
    len=route(AF_INET6,RT_TABLE_MAIN,9);assert(!parse(len,SEQ,&r,&count));
    len=route(AF_INET,RT_TABLE_LOCAL,9);assert(!parse(len,SEQ,&r,&count));
    len=route(AF_INET,RT_TABLE_MAIN,9);assert(parse(len,SEQ+1,&r,&count)==EBADMSG);
    assert(parse(len-1,SEQ,&r,&count)==EBADMSG);
    ((struct nlmsghdr *)data.bytes)->nlmsg_flags=NLM_F_DUMP_INTR;assert(parse(len,SEQ,&r,&count)==EAGAIN);
    len=route(AF_INET,RT_TABLE_MAIN,9);
    ((struct rtattr *)(data.bytes+NLMSG_LENGTH(sizeof(struct rtmsg))))->rta_len=RTA_LENGTH(1);
    assert(parse(len,SEQ,&r,&count)==EBADMSG);
    memset(&data,0,sizeof(data));struct nlmsghdr *h=(struct nlmsghdr *)data.bytes;
    h->nlmsg_len=NLMSG_LENGTH(sizeof(struct nlmsgerr));h->nlmsg_type=NLMSG_ERROR;h->nlmsg_seq=SEQ;
    ((struct nlmsgerr *)NLMSG_DATA(h))->error=-EACCES;assert(parse(h->nlmsg_len,SEQ,&r,&count)==EACCES);
    h->nlmsg_len=NLMSG_LENGTH(sizeof(int));h->nlmsg_type=NLMSG_DONE;*(int *)NLMSG_DATA(h)=0;
    assert(!network_ipv4_parse_routes(data.bytes,h->nlmsg_len,SEQ,&r,&count,&done)&&done);
    free(r);return 0;
}
