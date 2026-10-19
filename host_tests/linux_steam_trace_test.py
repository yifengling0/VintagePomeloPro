"""Bounded child-only tracing: normal exit, real seccomp kill, and timeout/reap."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
class NativeTrace(unittest.TestCase):
    def test_normal_denied_and_timeout(self):
        with tempfile.TemporaryDirectory(prefix="steamohos-trace-") as temporary:
            base=Path(temporary)
            source=base/"trace.cpp"
            source.write_text(r'''#include "linux_steam/native_exec.h"
#include <cassert>
#include <iostream>
#include <csignal>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
int main(int argc,char **argv){
    if(argc==2 && !strcmp(argv[1],"thread-fault")) {
        std::thread worker([] {raise(SIGSEGV);});worker.join();return 1;
    }
    if(argc==2 && !strcmp(argv[1],"deny")){
        sock_filter instructions[]={
            BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr)),
            BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_getppid,0,1),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
        sock_fprog filter{4,instructions};
        assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
        assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
        syscall(SYS_getppid);return 1;
    }
    char self[4096];auto n=readlink("/proc/self/exe",self,sizeof(self)-1);assert(n>0);self[n]=0;
    steamohos::NativeExecOptions o;o.traceSyscalls=true;
    auto normal=steamohos::RunNativeExec({"/bin/true"},{},3000,o);
    std::cerr<<steamohos::NativeExecJson(normal)<<"\n";
    assert(normal.reaped&&normal.exitCode==0&&!normal.waitError&&normal.output.find("syscall=")!=std::string::npos);
    auto denied=steamohos::RunNativeExec({self,"deny"},{},3000,o);
    std::cerr<<steamohos::NativeExecJson(denied)<<"\n";
    assert(denied.reaped&&denied.signal==SIGSYS&&!denied.waitError);
    assert(denied.output.find("syscall="+std::to_string(SYS_getppid)+" pc=")!=std::string::npos);
    auto timed=steamohos::RunNativeExec({"/bin/sleep","4"},{},150,o);
    assert(timed.reaped&&timed.timedOut&&timed.signal==SIGKILL);
    auto ordinary=steamohos::RunNativeExec({"/bin/true"},{},1000);
    assert(ordinary.exitCode==0&&ordinary.output.find("SYSCALL_TRACE")==std::string::npos);
    steamohos::NativeExecOptions signals;signals.traceSignals=true;
    auto fault=steamohos::RunNativeExec({self,"thread-fault"},{},3000,signals);
    std::cerr<<steamohos::NativeExecJson(fault)<<"\n";
    assert(fault.reaped&&fault.signal==SIGSEGV&&!fault.waitError&&!fault.timedOut);
    assert(fault.faultDiagnostics.find("signal=11 pc=0x")!=std::string::npos);
    auto signalTimeout=steamohos::RunNativeExec({"/bin/sleep","4"},{},100,signals);
    assert(signalTimeout.reaped&&signalTimeout.signal==SIGKILL&&signalTimeout.timedOut);
    auto signalNormal=steamohos::RunNativeExec({"/bin/true"},{},3000,signals);
    assert(signalNormal.reaped&&signalNormal.exitCode==0&&!signalNormal.waitError&&signalNormal.faultDiagnostics.empty());
    struct Progress {pid_t owner;void* tp;unsigned running=0,finished=0;};
    Progress progress{getpid(),__builtin_thread_pointer()};
    steamohos::NativeExecOptions monitored;
    monitored.context=&progress;
    monitored.parentPoll=[](void* value,int childPid,bool finished) {
        auto& p=*static_cast<Progress*>(value);
        assert(getpid()==p.owner&&__builtin_thread_pointer()==p.tp&&childPid>0&&childPid!=p.owner);
        if(finished)++p.finished;else ++p.running;
    };
    // A quiet child still emits a second live snapshot; callbacks do not
    // replace normal wait/reap or turn a successful child into a timeout.
    auto quiet=steamohos::RunNativeExec({"/bin/sleep","5.15"},{},7000,monitored);
    assert(quiet.reaped&&quiet.exitCode==0&&!quiet.timedOut&&quiet.hostTpUnchanged);
    assert(progress.running==2&&progress.finished==1);
    progress.running=progress.finished=0;
    auto monitoredTimeout=steamohos::RunNativeExec({"/bin/sleep","4"},{},100,monitored);
    assert(monitoredTimeout.reaped&&monitoredTimeout.timedOut&&monitoredTimeout.signal==SIGKILL);
    assert(progress.running==1&&progress.finished==1);
    int status;assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);
    std::cout<<"PASS child-only syscall history; seccomp SIGSYS preserved; timeout/reap; trace disabled by default\n";
}
''')
            native=ROOT/"entry/src/main/cpp/linux_steam"
            runner=base/"trace"
            subprocess.run(["g++","-std=c++17","-Wall","-Wextra","-Werror","-I"+str(native.parent),str(source),
                *[str(native/name) for name in ["native_exec.cpp","native_loader.cpp","native_loader_entry.S"]],"-pthread","-o",str(runner)],check=True,timeout=30)
            result=subprocess.run([str(runner)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout,end="")
if __name__=="__main__":unittest.main()
