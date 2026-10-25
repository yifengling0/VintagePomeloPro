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
#include <time.h>
#include <unistd.h>

typedef struct {
    snd_pcm_ioplug_t io;
    int control, timer;
    snd_pcm_uframes_t boundary;
    uint32_t sequence, stream;
    WinehuaAudioRingBuffer *ring;
    size_t mapping_size;
    int timing;
    uint64_t report_us, last_write_us, write_frames, write_calls, short_writes;
    uint64_t max_write_gap_us, max_copy_us, appl_expected, appl_rewinds;
    unsigned char *capture;
    size_t capture_frames;
} HmosAudio;

#define CAPTURE_FRAMES (48000u * 12u)
#define PROBE_PATH "/data/storage/el2/base/files/steamohos-native/proton-probe/"

/* Diagnostic only: copy at most 12 seconds in memory, then flush on close.
 * The playback thread never performs file I/O while transferring PCM. */
static void save_capture(HmosAudio *a) {
    if (!a->capture) return;
    if (a->capture_frames && a->stream) {
        char path[256];
        snprintf(path,sizeof(path),PROBE_PATH "audio-source-%u.pcm",a->stream);
        int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
        size_t done=0,bytes=a->capture_frames*4;
        if(fd>=0) {
            while(done<bytes) {
                ssize_t n=write(fd,a->capture+done,bytes-done);
                if(n<0&&errno==EINTR)continue;
                if(n<=0)break;
                done+=(size_t)n;
            }
            close(fd);
        }
        fprintf(stderr,"HMOSDECK_AUDIO_CAPTURE stream=%u rate=48000 channels=2 frames=%zu bytes=%zu saved=%zu\n",
            a->stream,a->capture_frames,bytes,done);
    }
    free(a->capture);a->capture=NULL;a->capture_frames=0;
}

static uint64_t monotonic_us(void) {
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000+now.tv_nsec/1000;
}
static void timing_report(HmosAudio *a,uint64_t now,int force) {
    if(!a->timing||!a->ring)return;
    if(!a->report_us)a->report_us=now;
    if(!force&&now-a->report_us<1000000)return;
    uint64_t read=winehua_audio_ring_load_read_index(a->ring),write=winehua_audio_ring_load_write_index(a->ring);
    fprintf(stderr,"HMOSDECK_AUDIO_TIMING pid=%u stream=%u elapsed_us=%llu writes=%llu frames=%llu max_gap_us=%llu max_copy_us=%llu short=%llu rewinds=%llu read=%llu write=%llu queued=%llu alsa_appl=%lu alsa_hw=%lu underruns=%u state=%u\n",
        (unsigned)getpid(),a->stream,(unsigned long long)(now-a->report_us),(unsigned long long)a->write_calls,
        (unsigned long long)a->write_frames,(unsigned long long)a->max_write_gap_us,(unsigned long long)a->max_copy_us,
        (unsigned long long)a->short_writes,(unsigned long long)a->appl_rewinds,(unsigned long long)read,
        (unsigned long long)write,(unsigned long long)(write>=read?write-read:0),
        (unsigned long)a->io.appl_ptr,(unsigned long)a->io.hw_ptr,winehua_audio_ring_load_underrun_count(a->ring),winehua_audio_ring_load_state(a->ring));
    a->report_us=now;a->write_frames=a->write_calls=a->short_writes=a->appl_rewinds=0;
    a->max_write_gap_us=a->max_copy_us=0;
}

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
    timing_report(a,monotonic_us(),1);
    save_capture(a);
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
    fprintf(stderr,"HMOSDECK_AUDIO: hw_params rate=%u channels=%u format=%d buffer=%lu period=%lu\n",
        io->rate,io->channels,io->format,(unsigned long)io->buffer_size,(unsigned long)io->period_size);
    WinehuaAudioOpenStreamReq req={48000,2,WINEHUA_AUDIO_SAMPLE_S16LE,(uint32_t)io->buffer_size,(uint32_t)io->period_size,0};
    WinehuaAudioOpenStreamResp resp;int fd=-1;
    int err=request(a,WINEHUA_AUDIO_CMD_OPEN_STREAM,&req,sizeof(req),&resp,sizeof(resp),&fd);
    if(err){fprintf(stderr,"HMOSDECK_AUDIO: open_stream transport_error=%d\n",err);return err;}
    if(resp.result){if(fd>=0)close(fd);fprintf(stderr,"HMOSDECK_AUDIO: open_stream result=%d\n",resp.result);return resp.result;}
    a->stream=resp.stream_id;
    struct stat st={0};
    int stat_result=fd<0?-1:fstat(fd,&st),stat_error=errno;
    /* The phone sandbox can deny stat metadata on an SCM_RIGHTS memfd while
     * allowing the descriptor itself. Obtain its real length with lseek;
     * retain the size check and restore the private descriptor's offset. */
    if(stat_result&&(stat_error==EACCES||stat_error==EPERM)) {
        off_t position=lseek(fd,0,SEEK_CUR);
        off_t length=position<0?-1:lseek(fd,0,SEEK_END);
        if(position>=0&&length>=0&&lseek(fd,position,SEEK_SET)==position) {
            st.st_size=length;stat_result=0;
        }
    }
    if(fd<0||resp.mix_sample_rate!=48000||resp.mix_channels!=2||resp.mix_sample_format!=1||resp.mix_frame_size!=4||
       !resp.ring_capacity_frames||resp.ring_capacity_frames>48000*4||
       resp.ring_mapping_size!=winehua_audio_ring_header_size()+resp.ring_capacity_frames*4||
       stat_result||st.st_size<(off_t)resp.ring_mapping_size) {
        if(fd>=0)close(fd);
        fprintf(stderr,"HMOSDECK_AUDIO: invalid ring fd=%d capacity=%u mapping=%u buffer=%lu period=%lu rate=%u channels=%u format=%u frame=%u header=%u stat=%d errno=%d size=%lld\n",
            fd,resp.ring_capacity_frames,resp.ring_mapping_size,(unsigned long)io->buffer_size,(unsigned long)io->period_size,
            resp.mix_sample_rate,resp.mix_channels,resp.mix_sample_format,resp.mix_frame_size,
            winehua_audio_ring_header_size(),stat_result,stat_error,(long long)st.st_size);
        release_stream(a);return -EPROTO;
    }
    void *view=mmap(NULL,resp.ring_mapping_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(view==MAP_FAILED){err=-errno;release_stream(a);return err;}
    a->ring=view;a->mapping_size=resp.ring_mapping_size;
    if(a->ring->magic!=WINEHUA_AUDIO_RING_MAGIC||a->ring->version!=1||a->ring->capacity_frames!=resp.ring_capacity_frames||
       a->ring->frame_size!=4||a->ring->sample_rate!=48000||a->ring->channels!=2||a->ring->sample_format!=1) {
        release_stream(a);return -EPROTO;
    }
    if(access(PROBE_PATH "audio-pcm-debug",R_OK)==0)a->capture=malloc(CAPTURE_FRAMES*4);
    /* ALSA's logical buffer and the transport ring have independent sizes.
     * transfer() handles partial writes; position() uses absolute consumed
     * frames modulo ALSA's buffer, so the ring need not fit that whole buffer. */
    fprintf(stderr,"HMOSDECK_AUDIO: GNU ALSA connected to OHOS AudioBroker 48000/S16LE/stereo ring=%u buffer=%lu period=%lu receiver_period=%u\n",
        resp.ring_capacity_frames,(unsigned long)io->buffer_size,(unsigned long)io->period_size,resp.preferred_period_frames);return 0;
}
static int prepare(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;
    if(a->timing)fprintf(stderr,"HMOSDECK_AUDIO_EVENT stream=%u event=prepare\n",a->stream);
    a->appl_expected=0;a->last_write_us=0;a->report_us=monotonic_us();
    return command(a,WINEHUA_AUDIO_CMD_RESET);
}
static int start(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;
    if(a->timing)fprintf(stderr,"HMOSDECK_AUDIO_EVENT stream=%u event=start\n",a->stream);
    return command(a,WINEHUA_AUDIO_CMD_START);
}
static int stop(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;
    if(a->timing)fprintf(stderr,"HMOSDECK_AUDIO_EVENT stream=%u event=stop\n",a->stream);
    return command(a,WINEHUA_AUDIO_CMD_STOP);
}
static int hw_free(snd_pcm_ioplug_t *io) {return release_stream(io->private_data);}
static int sw_params(snd_pcm_ioplug_t *io,snd_pcm_sw_params_t *params) {
    HmosAudio *a=io->private_data;return snd_pcm_sw_params_get_boundary(params,&a->boundary);
}
static snd_pcm_sframes_t position(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;if(!a->ring||!io->buffer_size)return 0;
    if(winehua_audio_ring_load_state(a->ring)==WINEHUA_AUDIO_STREAM_CLOSED)return -ENODEV;
    if(a->timing)timing_report(a,monotonic_us(),0);
    /* A renderer callback may consume the entire short ALSA buffer between
     * pointer polls. Report the real monotonic cursor up to ALSA's boundary
     * so a complete wrap is not mistaken for no playback progress. */
    return winehua_audio_ring_load_read_index(a->ring)%(a->boundary?a->boundary:io->buffer_size);
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
    uint64_t begin=a->timing?monotonic_us():0;
    if(a->timing) {
        if(a->last_write_us&&frames&&begin-a->last_write_us>a->max_write_gap_us)a->max_write_gap_us=begin-a->last_write_us;
        if(frames)a->last_write_us=begin;
        if(io->appl_ptr<a->appl_expected&&a->appl_expected-io->appl_ptr<(a->boundary?a->boundary/2:UINT64_MAX))a->appl_rewinds++;
        a->appl_expected=io->appl_ptr+frames;
        a->write_calls++;a->write_frames+=frames;
        if(!frames)a->short_writes++;
    }
    for(unsigned channel=0;channel<2;channel++) {
        if(areas[channel].first%8||areas[channel].step%8)return -EINVAL;
        const unsigned char *source=(const unsigned char*)areas[channel].addr+areas[channel].first/8+offset*(areas[channel].step/8);
        for(snd_pcm_uframes_t i=0;i<frames;i++)memcpy(a->ring->data+((write+i)%capacity)*4+channel*2,source+i*(areas[channel].step/8),2);
    }
    winehua_audio_ring_store_write_index(a->ring,write+frames);
    if(a->capture&&a->capture_frames<CAPTURE_FRAMES) {
        size_t count=frames;
        if(count>CAPTURE_FRAMES-a->capture_frames)count=CAPTURE_FRAMES-a->capture_frames;
        for(size_t i=0;i<count;i++)
            memcpy(a->capture+(a->capture_frames+i)*4,a->ring->data+((write+i)%capacity)*4,4);
        a->capture_frames+=count;
    }
    if(a->timing) {
        uint64_t now=monotonic_us(),copy=now-begin;if(copy>a->max_copy_us)a->max_copy_us=copy;
        timing_report(a,now,0);
    }
    return (snd_pcm_sframes_t)frames;
}
static int poll_revents(snd_pcm_ioplug_t *io,struct pollfd *fds,unsigned count,unsigned short *events) {
    HmosAudio *a=io->private_data;if(count!=1)return -EINVAL;
    uint64_t ticks;while(read(a->timer,&ticks,sizeof(ticks))==(ssize_t)sizeof(ticks)){}
    *events=fds[0].revents&(POLLERR|POLLHUP|POLLNVAL);
    if(a->ring&&winehua_audio_ring_free_frames(a->ring)>0)*events|=POLLOUT;
    return 0;
}
static int close_pcm(snd_pcm_ioplug_t *io) {
    HmosAudio *a=io->private_data;release_stream(a);if(a->control>=0)close(a->control);if(a->timer>=0)close(a->timer);free(a);return 0;
}
static const snd_pcm_ioplug_callback_t callbacks={.start=start,.stop=stop,.pointer=position,.transfer=transfer,
    .close=close_pcm,.hw_params=hw_params,.hw_free=hw_free,.sw_params=sw_params,.prepare=prepare,.poll_revents=poll_revents,.delay=delay};
SND_PCM_PLUGIN_DEFINE_FUNC(hmosdeck) {
    (void)root;(void)conf;if(stream!=SND_PCM_STREAM_PLAYBACK)return -ENODEV;
    HmosAudio *a=calloc(1,sizeof(*a));if(!a)return -ENOMEM;a->control=-1;a->timer=-1;
    a->timing=access("/data/storage/el2/base/files/steamohos-native/proton-probe/audio-timing-debug",R_OK)==0;
    int err=open_control(a);if(err){close_pcm(&((snd_pcm_ioplug_t){.private_data=a}));return err;}
    a->timer=timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC|TFD_NONBLOCK);
    if(a->timer<0){err=-errno;close_pcm(&((snd_pcm_ioplug_t){.private_data=a}));return err;}
    struct itimerspec tick={{0,5000000},{0,5000000}};
    if(timerfd_settime(a->timer,0,&tick,NULL)){err=-errno;close_pcm(&((snd_pcm_ioplug_t){.private_data=a}));return err;}
    a->io=(snd_pcm_ioplug_t){.version=SND_PCM_IOPLUG_VERSION,.name="HmosDeck OHOS AudioBroker",.flags=SND_PCM_IOPLUG_FLAG_MONOTONIC|SND_PCM_IOPLUG_FLAG_BOUNDARY_WA,
        .poll_fd=a->timer,.poll_events=POLLIN,.callback=&callbacks,.private_data=a};
    if((err=snd_pcm_ioplug_create(&a->io,name,stream,mode))<0){close_pcm(&a->io);return err;}
    /* plug's format/rate converters use mmap on their slave. ioplug's
     * mmap_commit routes actual MMAP access through transfer(). Keep
     * mmap_rw off so ordinary writei also reaches transfer(), rather than
     * writing an unconsumed local emulation buffer. */
    const unsigned access[]={SND_PCM_ACCESS_RW_INTERLEAVED,SND_PCM_ACCESS_MMAP_INTERLEAVED},format[]={SND_PCM_FORMAT_S16_LE},rate[]={48000},channels[]={2};
    if((err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_ACCESS,2,access))<0||
       (err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_FORMAT,1,format))<0||
       (err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_RATE,1,rate))<0||
       (err=snd_pcm_ioplug_set_param_list(&a->io,SND_PCM_IOPLUG_HW_CHANNELS,1,channels))<0||
       /* Explicit 2048-frame period comparison for the GNU/FEX media path.
        * Four periods provide 8192 frames (171 ms), matching the transport
        * ring. The receiver keeps its own native callback size. */
       (err=snd_pcm_ioplug_set_param_minmax(&a->io,SND_PCM_IOPLUG_HW_PERIOD_BYTES,8192,8192))<0||
       (err=snd_pcm_ioplug_set_param_minmax(&a->io,SND_PCM_IOPLUG_HW_BUFFER_BYTES,32768,32768))<0||
       (err=snd_pcm_ioplug_set_param_minmax(&a->io,SND_PCM_IOPLUG_HW_PERIODS,2,16))<0) {snd_pcm_ioplug_delete(&a->io);return err;}
    *pcmp=a->io.pcm;return 0;
}
SND_PCM_PLUGIN_SYMBOL(hmosdeck)
