#include "host_napi.h"
#include "launch_broker.h"
#include "proc/wine_process.h"
namespace steamohos {
namespace {
napi_value String(napi_env env,const std::string& text){napi_value value;napi_create_string_utf8(env,text.c_str(),text.size(),&value);return value;}
napi_value Boolean(napi_env env,bool value){napi_value result;napi_get_boolean(env,value,&result);return result;}
void Property(napi_env env,napi_value object,const char* key,const std::string& value){napi_set_named_property(env,object,key,String(env,value));}
void Array(napi_env env,napi_value object,const char* key,const std::vector<std::string>& values){
    napi_value array;napi_create_array_with_length(env,values.size(),&array);
    for(size_t i=0;i<values.size();++i)napi_set_element(env,array,i,String(env,values[i]));
    napi_set_named_property(env,object,key,array);
}
}
napi_value StartGameLaunchBroker(napi_env env,napi_callback_info info){
    size_t count=1;char path[4096];napi_value argv[1];size_t length=0;
    napi_get_cb_info(env,info,&count,argv,nullptr,nullptr);
    if(count!=1||napi_get_value_string_utf8(env,argv[0],path,sizeof(path),&length)!=napi_ok||length>=sizeof(path)-1)return Boolean(env,false);
    return Boolean(env,GameLaunchBroker().Start("/data/storage/el2/base/files/steamohos-native",path));
}
napi_value IsLinuxSteamProcess(napi_env env, napi_callback_info info) {
    size_t count = 1; napi_value argv[1]; int32_t pid = -1;
    napi_get_cb_info(env, info, &count, argv, nullptr, nullptr);
    return Boolean(env, count == 1 && napi_get_value_int32(env, argv[0], &pid) == napi_ok &&
        GameLaunchBroker().OwnsProcess(pid));
}
napi_value StopGameLaunchBroker(napi_env env,napi_callback_info){GameLaunchBroker().Stop();return Boolean(env,true);}
napi_value DrainGameLaunchBroker(napi_env env,napi_callback_info){
    auto events=GameLaunchBroker().Drain();napi_value result;napi_create_array_with_length(env,events.size(),&result);
    for(size_t i=0;i<events.size();++i){const auto& event=events[i];napi_value object,id,cancel;napi_create_object(env,&object);
        napi_create_uint32(env,event.id,&id);napi_get_boolean(env,event.cancel,&cancel);
        napi_set_named_property(env,object,"id",id);napi_set_named_property(env,object,"cancel",cancel);
        Property(env,object,"appId",event.appId);Property(env,object,"cwd",event.cwd);Property(env,object,"install",event.install);
        Array(env,object,"argv",event.argv);Array(env,object,"environment",event.environment);napi_set_element(env,result,i,object);}
    return result;
}
napi_value ReplyGameLaunchBroker(napi_env env,napi_callback_info info){
    napi_value argv[3];size_t count=3;uint32_t id,kind,code;napi_get_cb_info(env,info,&count,argv,nullptr,nullptr);
    if(count!=3||napi_get_value_uint32(env,argv[0],&id)!=napi_ok||napi_get_value_uint32(env,argv[1],&kind)!=napi_ok||napi_get_value_uint32(env,argv[2],&code)!=napi_ok)return Boolean(env,false);
    return Boolean(env,GameLaunchBroker().Reply(id,kind,code));
}
napi_value PollGameLaunchSession(napi_env env,napi_callback_info info){
    napi_value argv[2],result;size_t count=2;uint32_t id=0;char session[128];size_t length=0;
    napi_get_cb_info(env,info,&count,argv,nullptr,nullptr);
    int state=-1;
    if(count==2&&napi_get_value_uint32(env,argv[0],&id)==napi_ok&&
       napi_get_value_string_utf8(env,argv[1],session,sizeof(session),&length)==napi_ok&&length<sizeof(session)-1){
        WineProcessEntry process;
        if(QueryWineSession(session,&process)){
            state=process.running?1:0;
            if(!process.running){bool known=process.exitCodeSource=="waitpid"||process.exitCodeSource=="signal";
                GameLaunchBroker().Reply(id,known?2:4,known?static_cast<uint32_t>(process.exitCode):0);}
        }else GameLaunchBroker().Reply(id,4,0);
    }
    napi_create_int32(env,state,&result);return result;
}
}
