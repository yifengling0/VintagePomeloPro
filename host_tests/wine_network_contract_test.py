from pathlib import Path
import shutil, subprocess, tempfile
repo=Path(__file__).resolve().parents[1]
names=['dlls/nsiproxy.sys/ndis.c','dlls/nsiproxy.sys/ip.c','dlls/nsiproxy.sys/Makefile.in',
       'dlls/iphlpapi/iphlpapi_main.c','dlls/nsiproxy.sys/network-route-linux.c','dlls/nsiproxy.sys/network-route-linux.h']
with tempfile.TemporaryDirectory(prefix='wine-network-contract-') as folder:
    tree=Path(folder)
    for name in names:
        original=repo/'thirdparty/wine-valve'/name
        if original.exists():
            (tree/name).parent.mkdir(parents=True,exist_ok=True);shutil.copy2(original,tree/name)
    patch=(repo/'patches/wine/0061-iphlpapi-native-network-contract.patch').read_bytes()
    reverse=subprocess.run(['patch','-p1','--batch','--force','--dry-run','-R'],input=patch,cwd=tree,capture_output=True)
    if reverse.returncode:
        subprocess.run(['patch','-p1','--batch','--force'],input=patch,cwd=tree,check=True,stdout=subprocess.DEVNULL)
    for mode in ['linux','ohos']:
        binary=tree/('route-test-'+mode)
        args=['gcc','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
              '-fno-omit-frame-pointer','-I'+str(tree/'dlls/nsiproxy.sys')]
        if mode=='ohos':args+=['-D__OHOS__']
        subprocess.run(args+[str(tree/'dlls/nsiproxy.sys/network-route-linux.c'),
            str(repo/'host_tests/network_route_contract_test.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        print('PASS',mode,'network overlay and route parser contract')
