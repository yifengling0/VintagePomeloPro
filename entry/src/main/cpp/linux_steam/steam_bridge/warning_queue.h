// Code pointers stay local to their ABI. Only bounded warning text crosses.
#pragma once
#include "wire.h"
#include <deque>
#include <mutex>
namespace hmosbridge {
constexpr uint32_t WarningMessagesMethod=5;
constexpr size_t MaxWarningMessages=32, MaxWarningText=4096;
struct WarningMessage {int level;std::string text;};
class WarningQueue {
 std::mutex mutex;std::deque<WarningMessage> pending;bool enabled=false;
public:
 void Reset(bool value){std::lock_guard<std::mutex> guard(mutex);enabled=value;pending.clear();}
 void Push(int level,const char* message){
  if(!message)return;
  std::lock_guard<std::mutex> guard(mutex);if(!enabled)return;
  if(pending.size()==MaxWarningMessages)pending.pop_front();
  pending.push_back({level,std::string(message,strnlen(message,MaxWarningText))});
 }
 void Drain(Buffer& output){
  std::lock_guard<std::mutex> guard(mutex);output.Put(pending.size());
  for(auto& warning:pending){PutScalar(output,warning.level);output.String(warning.text.c_str());}
  pending.clear();
 }
};
}
