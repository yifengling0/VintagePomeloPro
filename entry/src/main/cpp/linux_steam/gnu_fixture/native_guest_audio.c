// Keep original GNU Wine/Steam PCM transport unchanged. Only the x86 guest
// needs a metadata-free size query for the broker's seekable shared-ring fd.
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int native_guest_ring_stat(int fd,struct stat *status) {
    int saved=errno;
    off_t position=lseek(fd,0,SEEK_CUR);
    off_t length=position<0?-1:lseek(fd,0,SEEK_END);
    if(position>=0&&length>=0&&lseek(fd,position,SEEK_SET)==position) {
        memset(status,0,sizeof(*status));status->st_size=length;errno=saved;return 0;
    }
    return fstat(fd,status);
}
#define fstat native_guest_ring_stat
#include "alsa_hmosdeck.c"
#undef fstat
