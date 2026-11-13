"""Real pipe/log regression: bounded file, restored stdio, bounded teardown."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='display-log-') as folder:
    root = Path(folder)
    source = root/'test.cpp'
    source.write_text(r'''#include "display/ncp/bounded_log.h"
#include <cstring>
int main(int argc, char** argv) {
    int retained = -1;
    {
        DisplayChildLog log;
        if (!log.Start(argv[1])) return 1;
        retained = dup(STDERR_FILENO); // Simulate a library retaining a writer.
        char data[4096]; memset(data, 'x', sizeof(data));
        for (int i=0;i<2500;++i) if (write(2,data,sizeof(data)) != sizeof(data)) return 2;
        fprintf(stderr,"TAIL\n");
    }
    close(retained);
    return 0;
}
''')
    exe = root/'test'
    subprocess.run(['c++','-std=c++17','-pthread','-I'+str(ROOT/'entry/src/main/cpp'),str(source),'-o',str(exe)],check=True)
    log = root/'bounded.log'
    subprocess.run([str(exe),str(log)],check=True,timeout=10)
    data = log.read_bytes()
    assert 0 < len(data) <= 4*1024*1024, len(data)
    assert data.endswith(b'TAIL\n'), 'queued tail lost during shutdown'
print('display child log: bounded file and teardown PASS')
