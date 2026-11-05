#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>
static FILE*logfile;
static void ck(HRESULT hr,const char*op){if(FAILED(hr)){fprintf(logfile,"FAIL %s hr=%08lx\n",op,(unsigned long)hr);throw std::runtime_error(op);}}
int main(){
 logfile=fopen("C:\\spatial-audio-result.txt","wb");if(!logfile)return 10;setvbuf(logfile,nullptr,_IONBF,0);
 try{
 ck(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
 IMMDeviceEnumerator*e=nullptr;IMMDevice*d=nullptr;ISpatialAudioClient*a=nullptr;
 ck(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&e),"enumerator");
 ck(e->GetDefaultAudioEndpoint(eRender,eConsole,&d),"endpoint");
 ck(d->Activate(__uuidof(ISpatialAudioClient),CLSCTX_ALL,nullptr,(void**)&a),"spatial client");
 IAudioFormatEnumerator*formats=nullptr;WAVEFORMATEX*format=nullptr;
 ck(a->GetSupportedAudioObjectFormatEnumerator(&formats),"formats");ck(formats->GetFormat(0,&format),"format");
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
 SpatialAudioObjectRenderStreamActivationParams ap={};ap.ObjectFormat=format;ap.EventHandle=event;
 ap.StaticObjectTypeMask=(AudioObjectType)(AudioObjectType_FrontLeft|AudioObjectType_FrontRight|AudioObjectType_FrontCenter|AudioObjectType_LowFrequency|AudioObjectType_SideLeft|AudioObjectType_SideRight|AudioObjectType_BackLeft|AudioObjectType_BackRight|AudioObjectType_TopFrontLeft|AudioObjectType_TopFrontRight|AudioObjectType_TopBackLeft|AudioObjectType_TopBackRight);
 PROPVARIANT prop={};prop.vt=VT_BLOB;prop.blob.cbSize=sizeof(ap);prop.blob.pBlobData=(BYTE*)&ap;
 ISpatialAudioObjectRenderStream*s=nullptr;ck(a->ActivateSpatialAudioStream(&prop,__uuidof(ISpatialAudioObjectRenderStream),(void**)&s),"activate stream");
 ISpatialAudioObject*left=nullptr;ck(s->ActivateSpatialAudioObject(AudioObjectType_FrontLeft,&left),"front-left object");
 ck(s->Start(),"start");DWORD begin=GetTickCount();UINT64 submitted=0;unsigned events=0;
 while(events<300){
   if(GetTickCount()-begin>10000)throw std::runtime_error("render deadline");
   if(WaitForSingleObject(event,1000)!=WAIT_OBJECT_0)throw std::runtime_error("event timeout");
   UINT32 dyn=0,frames=0;HRESULT hr=s->BeginUpdatingAudioObjects(&dyn,&frames);
   if(FAILED(hr)){fprintf(logfile,"events=%u submitted=%llu elapsed_ms=%lu\n",events,submitted,GetTickCount()-begin);ck(hr,"BeginUpdatingAudioObjects");}
   BYTE*data=nullptr;UINT32 bytes=0;ck(left->GetBuffer(&data,&bytes),"object buffer");
   if(bytes<frames*sizeof(float))throw std::runtime_error("object buffer size");
   for(UINT32 i=0;i<frames;i++)((float*)data)[i]=.15f*std::sin(float((submitted+i)%48000)*6.28318530718f*440/48000);
   ck(s->EndUpdatingAudioObjects(),"EndUpdatingAudioObjects");submitted+=frames;events++;
 }
 DWORD elapsed=GetTickCount()-begin;ck(s->Stop(),"stop");
 if(elapsed<2500)throw std::runtime_error("events are not paced by playback");
 fprintf(logfile,"PASS 12-channel spatial stream events=%u frames=%llu elapsed_ms=%lu\n",events,submitted,elapsed);
 left->Release();s->Release();formats->Release();a->Release();d->Release();e->Release();CloseHandle(event);CoUninitialize();fclose(logfile);return 0;
 }catch(const std::exception&e){fprintf(logfile,"FAIL %s\n",e.what());fclose(logfile);return 1;}
}
