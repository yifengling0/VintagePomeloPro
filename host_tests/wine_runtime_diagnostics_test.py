import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=r'''
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "wine_runtime_diagnostics.h"
volatile sig_atomic_t ohos_runtime_diagnostics_enabled;
static volatile sig_atomic_t delivered;
static int output;
static void handler(int signal)
{
    int previous = errno;
    (void)signal;
    ++delivered;
    ohos_runtime_diagnostics_write(output,"event\n",6);
    if(errno != previous) _exit(21);
}
int main(void)
{
    int pipes[2], size;
    char data[1024];
    struct sigaction action;
    const char *quiet=getenv("WINEHUA_DIAG_QUIET");
    ohos_runtime_diagnostics_enabled = !(quiet && quiet[0]=='1') &&
        ohos_diagnostics_flag(getenv("WINEHUA_RUNTIME_DIAGNOSTICS"));
    if(pipe(pipes)) return 22;
    output=pipes[1];
    memset(&action,0,sizeof(action)); action.sa_handler=handler;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGUSR1,&action,0)) return 23;
    errno=EDOM;
    for(int i=0;i<100;++i) if(raise(SIGUSR1)) return 24;
    if(delivered!=100 || errno!=EDOM) return 25;
    close(pipes[1]);
    size=read(pipes[0],data,sizeof(data));
    if(size != (ohos_runtime_diagnostics_enabled ? 600 : 0)) return 26;
    ohos_runtime_diagnostics_write(-1,"error\n",6);
    if(errno!=EDOM) return 27;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wine-diag-test-') as d:
    d=Path(d)
    (d/'probe.c').write_text(source)
    subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-D__OHOS__','-Wall','-Wextra','-Werror','-I'+str(root/'scripts'),str(d/'probe.c'),'-o',str(d/'probe')],check=True)
    for flag,quiet in [(None,None),('0',None),('1',None),('10',None),('1','1'),('1','0')]:
        env=os.environ.copy()
        for key in ['WINEHUA_RUNTIME_DIAGNOSTICS','WINEHUA_DIAG_QUIET']: env.pop(key,None)
        if flag is not None: env['WINEHUA_RUNTIME_DIAGNOSTICS']=flag
        if quiet is not None: env['WINEHUA_DIAG_QUIET']=quiet
        subprocess.run([str(d/'probe')],env=env,check=True,timeout=5)
        print('signal delivery / output / errno PASS',flag,quiet)
print('Actual diagnostic header: default-off, explicit-on, quiet priority, exact flag and signal errno PASS')
