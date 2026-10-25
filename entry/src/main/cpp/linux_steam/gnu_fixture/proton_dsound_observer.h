/* Private fixture only: observe the installed Wine DirectSound implementation.
 * Never injected into games or shipped as a DLL override. */
#define COBJMACROS
#include <dsound.h>

static HRESULT (WINAPI *observed_position)(IDirectSoundBuffer *,DWORD *,DWORD *);
static HRESULT (WINAPI *observed_seek)(IDirectSoundBuffer *,DWORD);
static HRESULT (WINAPI *observed_stop)(IDirectSoundBuffer *);
static HRESULT (WINAPI *observed_lock)(IDirectSoundBuffer *,DWORD,DWORD,void **,DWORD *,void **,DWORD *,DWORD);
static HRESULT (WINAPI *observed_unlock)(IDirectSoundBuffer *,void *,DWORD,void *,DWORD);
static CRITICAL_SECTION observer_mutex;
static struct {DWORD thread,bytes;LARGE_INTEGER begin;} observer_locks[8];
static struct {
    IDirectSoundBuffer *buffer;
    DWORD size, rate, align, last, first_tick, last_tick;
    unsigned long long advanced, written, reads, seeks, stops, max_lock_us;
    unsigned long long erases, erase_total_us, erase_max_us;
    int positioned;
} observer;

static void observe_buffer(IDirectSoundBuffer *buffer)
{
    if (observer.buffer == buffer) return;
    DSBCAPS caps = {.dwSize=sizeof(caps)};
    WAVEFORMATEX format;
    DWORD bytes;
    if (FAILED(IDirectSoundBuffer_GetCaps(buffer,&caps)) || caps.dwBufferBytes < 100000 ||
        FAILED(IDirectSoundBuffer_GetFormat(buffer,&format,sizeof(format),&bytes))) return;
    memset(&observer,0,sizeof(observer));
    observer.buffer=buffer; observer.size=caps.dwBufferBytes;
    observer.rate=format.nSamplesPerSec; observer.align=format.nBlockAlign;
    observer.first_tick=GetTickCount();
}

static HRESULT WINAPI observe_position(IDirectSoundBuffer *self,DWORD *read,DWORD *write)
{
    /* Serialize observations so two callers cannot manufacture a wrap by
       recording the same cursor out of order. */
    EnterCriticalSection(&observer_mutex);
    HRESULT hr=observed_position(self,read,write);
    observe_buffer(self);
    if (SUCCEEDED(hr) && read && observer.buffer==self) {
        if (observer.positioned) observer.advanced+=(*read+observer.size-observer.last)%observer.size;
        observer.last=*read; observer.last_tick=GetTickCount();
        observer.positioned=1; observer.reads++;
    }
    LeaveCriticalSection(&observer_mutex);
    return hr;
}

static HRESULT WINAPI observe_seek(IDirectSoundBuffer *self,DWORD pos)
{
    EnterCriticalSection(&observer_mutex);
    HRESULT hr=observed_seek(self,pos);
    if (SUCCEEDED(hr) && observer.buffer==self) {
        observer.positioned=0; observer.seeks++;
    }
    LeaveCriticalSection(&observer_mutex);
    return hr;
}

static HRESULT WINAPI observe_stop(IDirectSoundBuffer *self)
{
    HRESULT hr=observed_stop(self);
    EnterCriticalSection(&observer_mutex);
    if (SUCCEEDED(hr) && observer.buffer==self) observer.stops++;
    LeaveCriticalSection(&observer_mutex);
    return hr;
}

static HRESULT WINAPI observe_lock(IDirectSoundBuffer *self,DWORD start,DWORD count,
                                  void **p1,DWORD *n1,void **p2,DWORD *n2,DWORD flags)
{
    LARGE_INTEGER begin,end,freq;
    QueryPerformanceCounter(&begin);
    HRESULT hr=observed_lock(self,start,count,p1,n1,p2,n2,flags);
    QueryPerformanceCounter(&end); QueryPerformanceFrequency(&freq);
    EnterCriticalSection(&observer_mutex);
    observe_buffer(self);
    if (SUCCEEDED(hr) && observer.buffer==self) {
        /* VLC erases the rest of the ring with large locks; count the small
           decoded blocks separately from those erases. */
        if (count<100000) observer.written+=*n1+(n2?*n2:0);
        unsigned long long elapsed=(end.QuadPart-begin.QuadPart)*1000000/freq.QuadPart;
        if (elapsed>observer.max_lock_us) observer.max_lock_us=elapsed;
        DWORD thread=GetCurrentThreadId();
        for (unsigned i=0;i<8;i++) if (!observer_locks[i].thread || observer_locks[i].thread==thread) {
            observer_locks[i].thread=thread; observer_locks[i].bytes=count;
            observer_locks[i].begin=end; break;
        }
    }
    LeaveCriticalSection(&observer_mutex);
    return hr;
}

static HRESULT WINAPI observe_unlock(IDirectSoundBuffer *self,void *p1,DWORD n1,void *p2,DWORD n2)
{
    LARGE_INTEGER now,freq; QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
    DWORD thread=GetCurrentThreadId();
    EnterCriticalSection(&observer_mutex);
    if (self==observer.buffer) for (unsigned i=0;i<8;i++) if (observer_locks[i].thread==thread) {
        if (observer_locks[i].bytes>100000) {
            unsigned long long elapsed=(now.QuadPart-observer_locks[i].begin.QuadPart)*1000000/freq.QuadPart;
            observer.erases++; observer.erase_total_us+=elapsed;
            if (elapsed>observer.erase_max_us) observer.erase_max_us=elapsed;
        }
        observer_locks[i].thread=0;break;
    }
    LeaveCriticalSection(&observer_mutex);
    return observed_unlock(self,p1,n1,p2,n2);
}

static void observe_report(void)
{
    EnterCriticalSection(&observer_mutex);
    if (observer.buffer) printf("HMOSDECK_DS_CURSOR age_ms=%lu rate=%lu align=%lu size=%lu pos=%lu advanced=%llu written=%llu reads=%llu seeks=%llu stops=%llu max_lock_us=%llu erases=%llu erase_total_us=%llu erase_max_us=%llu\n",
        GetTickCount()-observer.first_tick,observer.rate,observer.align,observer.size,
        observer.last,observer.advanced,observer.written,observer.reads,observer.seeks,
        observer.stops,observer.max_lock_us,observer.erases,observer.erase_total_us,observer.erase_max_us);
    LeaveCriticalSection(&observer_mutex);
}

static int observe_install(void)
{
    HMODULE ds=LoadLibraryA("dsound.dll");
    typedef HRESULT (WINAPI *Create)(const GUID *,IDirectSound **,IUnknown *);
    Create create=ds?(Create)(void *)GetProcAddress(ds,"DirectSoundCreate"):NULL;
    IDirectSound *device=NULL;
    IDirectSoundBuffer *buffer=NULL;
    WAVEFORMATEX format={WAVE_FORMAT_PCM,2,44100,176400,4,16,0};
    DSBUFFERDESC desc={.dwSize=sizeof(desc),.dwBufferBytes=4096,.lpwfxFormat=&format};
    InitializeCriticalSection(&observer_mutex);
    if (!create || FAILED(create(NULL,&device,NULL))) return 0;
    HRESULT hr=IDirectSound_CreateSoundBuffer(device,&desc,&buffer,NULL);
    if (SUCCEEDED(hr)) {
        IDirectSoundBufferVtbl *table=(IDirectSoundBufferVtbl *)buffer->lpVtbl;
        DWORD previous;
        if (VirtualProtect(table,sizeof(*table),PAGE_READWRITE,&previous)) {
            observed_position=table->GetCurrentPosition; table->GetCurrentPosition=observe_position;
            observed_seek=table->SetCurrentPosition; table->SetCurrentPosition=observe_seek;
            observed_stop=table->Stop; table->Stop=observe_stop;
            observed_lock=table->Lock; table->Lock=observe_lock;
            observed_unlock=table->Unlock; table->Unlock=observe_unlock;
            DWORD ignored; VirtualProtect(table,sizeof(*table),previous,&ignored);
        }
        IDirectSoundBuffer_Release(buffer);
    }
    IDirectSound_Release(device);
    return observed_position!=NULL;
}
