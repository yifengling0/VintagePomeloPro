/* GNU ALSA playback plugin -> existing OHOS AudioBroker. No OHOS ABI enters GNU. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include "../../protocols/audio_ipc_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <unistd.h>

typedef struct {
    snd_pcm_ioplug_t io;
    int control, timer;
    uint32_t sequence, stream;
    WinehuaAudioRingBuffer *ring;
    size_t mapping_size;
} HmosAudio;

static int send_packet(int fd,const void *bytes,size_t size,int attached) {
    struct iovec iov={(void*)bytes,size};
    char ancillary[CMSG_SPACE(sizeof(int))]={0};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1};
    if(attached>=0) {
        msg.msg_control=ancillary;msg.msg_controllen=sizeof(ancillary);
        struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c),&attached,sizeof(int));
    }
    ssize_t count;
    do {count=sendmsg(fd,&msg,MSG_NOSIGNAL);}while(count<0&&errno==EINTR);
    return count==(ssize_t)size?0:-(count<0?errno:EIO);
}
static int request(HmosAudio *a,uint16_t cmd,const void *body,size_t size,void *reply,size_t reply_size,int *attached) {
    unsigned char packet[WINEHUA_AUDIO_PACKET_MAX];
    WinehuaAudioIpcHeader head={WINEHUA_AUDIO_CONTROL_MAGIC,WINEHUA_AUDIO_PROTOCOL_VERSION,cmd,++a->sequence,(uint32_t)size};
    if(sizeof(head)+size>sizeof(packet))return -EINVAL;
    memcpy(packet,&head,sizeof(head));memcpy(packet+sizeof(head),body,size);
    int err=send_packet(a->control,packet,sizeof(head)+size,-1);if(err)return err;
    struct iovec iov={packet,sizeof(packet)};char ancillary[CMSG_SPACE(sizeof(int)*4)]={0};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=ancillary,.msg_controllen=sizeof(ancillary)};
    ssize_t count;do {count=recvmsg(a->control,&msg,MSG_CMSG_CLOEXEC);}while(count<0&&errno==EINTR);
    int got=-1;
    if(count>=0)for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)) {
        if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS) {
            size_t n=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);int *fds=(int*)CMSG_DATA(c);
            for(size_t i=0;i<n;i++){if(got<0)got=fds[i];else close(fds[i]);}
        }
    }
    WinehuaAudioIpcHeader answer={0};if(count>=(ssize_t)sizeof(answer))memcpy(&answer,packet,sizeof(answer));
    if(count!=(ssize_t)(sizeof(answer)+reply_size)||msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)||
       answer.magic!=head.magic||answer.version!=head.version||answer.cmd!=cmd||answer.seq!=head.seq||answer.payload_size!=reply_size) {
        if(got>=0)close(got);
        return count<0?-errno:-EPROTO;
    }
    memcpy(reply,packet+sizeof(answer),reply_size);
    if(attached)*attached=got;else if(got>=0)close(got);
    return 0;
}
static int command(HmosAudio *a,uint16_t cmd) {
    if(!a->stream)return 0;
    WinehuaAudioStreamCmdReq req={a->stream};WinehuaAudioStreamCmdResp resp;
    int err=request(a,cmd,&req,sizeof(req),&resp,sizeof(resp),NULL);
    return err?err:resp.result;
}
static int release_stream(HmosAudio *a) {
    int err=command(a,WINEHUA_AUDIO_CMD_CLOSE);a->stream=0;
    if(a->ring){munmap(a->ring,a->mapping_size);a->ring=NULL;a->mapping_size=0;}
    return err;
}
static int open_control(HmosAudio *a) {
    int bootstrap=-1,owned=0;
    const char *endpoint=getenv("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET");
    if(endpoint&&endpoint[0]=='/'&&strlen(endpoint)<sizeof(((struct sockaddr_un*)0)->sun_path)) {
        struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,endpoint);
        bootstrap=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
        if(bootstrap>=0&&connect(bootstrap,(struct sockaddr*)&address,sizeof(address))) {close(bootstrap);bootstrap=-1;}
        if(bootstrap>=0)owned=1;
    }
    if(bootstrap<0) {
        const char *value=getenv("STEAMOHOS_AUDIO_BOOTSTRAP_FD");char *end=NULL;
        if(!value||!*value)return -ENODEV;
        long fd=strtol(value,&end,10);if(*end||fd<3||fd>1048576||fcntl(fd,F_GETFD)<0)return -ENODEV;
        bootstrap=(int)fd;
    }
    int sockets[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets)){int err=-errno;if(owned)close(bootstrap);return err;}
    struct timeval timeout={2,0};setsockopt(sockets[0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(sockets[0],SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    WinehuaAudioBootstrapReq hello_bootstrap={.magic=WINEHUA_AUDIO_BOOTSTRAP_MAGIC,.version=WINEHUA_AUDIO_PROTOCOL_VERSION,
        .cmd=WINEHUA_AUDIO_BOOTSTRAP_OPEN_CONTROL,.client_pid=(uint32_t)getpid()};
    strcpy(hello_bootstrap.process_name,"hmosdeck-linux-alsa");
    int err=send_packet(bootstrap,&hello_bootstrap,sizeof(hello_bootstrap),sockets[1]);close(sockets[1]);if(owned)close(bootstrap);
    if(err){close(sockets[0]);return err;}a->control=sockets[0];
    WinehuaAudioHelloReq req={.client_pid=(uint32_t)getpid()};strcpy(req.process_name,hello_bootstrap.process_name);
    WinehuaAudioHelloResp resp;err=request(a,WINEHUA_AUDIO_CMD_HELLO,&req,sizeof(req),&resp,sizeof(resp),NULL);
    if(err)return err;
    return resp.result?resp.result:(resp.protocol_version==1&&resp.mix_sample_rate==48000&&resp.mix_channels==2&&resp.mix_sample_format==1?0:-EPROTO);
}
static int hw_params(snd_pcm_ioplug_t *io,snd_pcm_hw_params_t *params) {
    (void)params;HmosAudio *a=io->private_data;release_stream(a);
    WinehuaAudioOpenStreamReq req={48000,2,WINEHUA_AUDIO_SAMPLE_S16LE,(uint32_t)io->buffer_size,(uint32_t)io->period_size,0};
    WinehuaAudioOpenStreamResp resp;int fd=-1;
    int err=request(a,WINEHUA_AUDIO_CMD_OPEN_STREAM,&req,sizeof(req),&resp,sizeof(resp),&fd);
    if(err)return err;
    if(resp.result){if(fd>=0)close(fd);return resp.result;}
    a->stream=resp.stream_id;
    struct stat st;
    if(fd<0||resp.mix_sample_rate!=48000||resp.mix_channels!=2||resp.mix_sample_format!=1||resp.mix_frame_size!=4||
       resp.ring_capacity_frames<io->buffer_size||resp.ring_capacity_frames>48000*4||
       resp.ring_mapping_size!=winehua_audio_ring_header_size()+resp.ring_capacity_frames*4||
       fstat(fd,&st)||st.st_size<(off_t)resp.ring_mapping_size) {
        if(fd>=0)close(fd);
        release_stream(a);return -EPROTO;
    }
    void *view=mmap(NULL,resp.ring_mapping_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(view==MAP_FAILED){err=-errno;release_stream(a);return err;}
    a->ring=view;a->mapping_size=resp.ring_mapping_size;
    if(a->ring->magic!=WINEHUA_AUDIO_RING_MAGIC||a->ring->version!=1||a->ring->capacity_frames!=resp.ring_capacity_frames||
       a->ring->frame_size!=4||a->ring->sample_rate!=48000||a->ring->channels!=2||a->ring->sample_format!=1) {
        release_stream(a);return -EPROTO;
    }
    fprintf(stderr,"HMOSDECK_AUDIO: GNU ALSA connected to OHOS AudioBroker 48000/S16LE/stereo\n");return 0;
}
static int prepare(snd_pcm_ioplug_t *io) {return command(io->private_data,WINEHUA_AUDIO_CMD_RESET);}
static int start(snd_pcm_ioplug_t *io) {return command(io->private_data,WINEHUA_AUDIO_CMD_START);}
static int stop(snd_pcm_ioplug_t *io) {return command(io->private_data,WINEHUA_AUDIO_CMD_STOP);}
static int hw_free(snd_pcm_ioplug_t *io) {return release_stream(io->private_data);}
static snd_pcm_sframes_t position(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;if(!a->ring||!io->buffer_size)return 0;
    if(winehua_audio_ring_load_state(a->ring)==WINEHUA_AUDIO_STREAM_CLOSED)return -ENODEV;
    return winehua_audio_ring_load_read_index(a->ring)%io->buffer_size;
}
static int delay(snd_pcm_ioplug_t *io,snd_pcm_sframes_t *frames) {
    HmosAudio *a=io->private_data;*frames=a->ring?winehua_audio_ring_used_frames(a->ring):0;return 0;
}
static snd_pcm_sframes_t transfer(snd_pcm_ioplug_t *io,const snd_pcm_channel_area_t *areas,snd_pcm_uframes_t offset,snd_pcm_uframes_t frames) {
    HmosAudio *a=io->private_data;if(!a->ring)return -ENODEV;
    uint64_t read=winehua_audio_ring_load_read_index(a->ring),write=winehua_audio_ring_load_write_index(a->ring);
    uint32_t capacity=a->ring->capacity_frames;
    if(write<read||write-read>capacity)return -EPROTO;
    uint64_t free=capacity-(write-read);if(frames>free)frames=free;
    for(unsigned channel=0;channel<2;channel++) {
        if(areas[channel].first%8||areas[channel].step%8)return -EINVAL;
        const unsigned char *source=(const unsigned char*)areas[channel].addr+areas[channel].first/8+offset*(areas[channel].step/8);
        for(snd_pcm_uframes_t i=0;i<frames;i++)memcpy(a->ring->data+((write+i)%capacity)*4+channel*2,source+i*(areas[channel].step/8),2);
    }
    winehua_audio_ring_store_write_index(a->ring,write+frames);return (snd_pcm_sframes_t)frames;
}
static int poll_revents(snd_pcm_ioplug_t *io,struct pollfd *fds,unsigned count,unsigned short *events) {
    HmosAudio *a=io->private_data;if(count!=1)return -EINVAL;
    uint64_t ticks;while(read(a->timer,&ticks,sizeof(ticks))==(ssize_t)sizeof(ticks)){}
    *events=fds[0].revents&(POLLERR|POLLHUP|POLLNVAL);
    if(a->ring&&winehua_audio_ring_free_frames(a->ring)>=io->period_size)*events|=POLLOUT;
    return 0;
}
static int close_pcm(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;release_stream(a);if(a->control>=0)close(a->control);if(a->timer>=0)close(a->timer);free(a);return 0;
}
static const snd_pcm_ioplug_callback_t callbacks={.start=start,.stop=stop,.pointer=position,.transfer=transfer,
    .close=close_pcm,.hw_params=hw_params,.hw_free=hw_free,.prepare=prepare,.poll_revents=poll_revents,.delay=delay};
SND_PCM_PLUGIN_DEFINE_FUNC(hmosdeck) {
    (void)root;(void)conf;if(stream!=SND_PCM_STREAM_PLAYBACK)return -ENODEV;
    HmosAudio *a=calloc(1,sizeof(*a));if(!a)return -ENOMEM;a->control=-1;a->timer=-1;
    int err=open_control(a);if(err){close_pcm(&((snd_pcm_ioplug_t){.private_data=a}));return err;}
    a->timer=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
    if(a->timer<0){err=-errno;close_pcm(&((snd_pcm_ioplug_t){.private_data=a}));return err;}
    struct itimerspec tick={{0,5000000},{0,5000000}};
    if(timerfd_settime(a->timer,0,&tick,NULL)){err=-errno;close_pcm(&((snd_pcm_ioplug_t){.private_data=a}));return err;}
    a->io=(snd_pcm_ioplug_t){.version=SND_PCM_IOPLUG_VERSION,.name="HmosDeck OHOS AudioBroker",.flags=SND_PCM_IOPLUG_FLAG_MONOTONIC,
        .poll_fd=a->timer,.poll_events=POLLIN,.callback=&callbacks,.private_data=a};
    if((err=snd_pcm_ioplug_create(&a->io,name,stream,mode))<0){close_pcm(&a->io);return err;}
    const unsigned access[]={SND_PCM_ACCESS_RW_INTERLEAVED},format[]={SND_PCM_FORMAT_S16_LE},rate[]={48000},channels[]={2};
    if((err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_ACCESS,1,access))<0||
       (err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_FORMAT,1,format))<0||
       (err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_RATE,1,rate))<0||
       (err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_CHANNELS,1,channels))<0||
       (err=snd_pcm_ioplug_set_param_minmax(&a->io,SND_PCM_IOPLUG_HW_PERIOD_BYTES,256,4096))<0||
       (err=snd_pcm_ioplug_set_param_minmax(&a->io,SND_PCM_IOPLUG_HW_BUFFER_BYTES,1024,16384))<0||
       (err=snd_pcm_ioplug_set_param_minmax(&a->io,SND_PCM_IOPLUG_HW_PERIODS,2,16))<0) {snd_pcm_ioplug_delete(&a->io);return err;}
    *pcmp=a->io.pcm;return 0;
}
SND_PCM_PLUGIN_SYMBOL(hmosdeck)
