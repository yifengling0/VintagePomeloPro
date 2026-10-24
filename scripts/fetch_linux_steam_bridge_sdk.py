#!/usr/bin/env python3
"""Fetch only checksum-pinned Valve/Proton headers required by the bridge.

Reuse a reference cache; --offline verifies it without network access. This is
an explicit developer preparation step, never run by the installed application.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path, PurePosixPath
import urllib.request

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference",type=Path,required=True)
    parser.add_argument("--offline",action="store_true")
    args=parser.parse_args()
    lock=json.loads((ROOT/"docs/linux-steam/steam_bridge_sdk.lock.json").read_text())
    base=args.reference.resolve()
    def prepare(item):
        name,expected=item
        relative=PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or not name.startswith("lsteamclient/steamworks_sdk_") or not name.endswith(".h"):raise ValueError("Invalid locked SDK path")
        path=base/name
        if path.is_symlink() or not path.resolve().is_relative_to(base):raise ValueError("Unsafe reference path")
        if path.is_file() and path.stat().st_size<=2*1024*1024 and hashlib.sha256(path.read_bytes()).hexdigest()==expected:return False
        if args.offline:raise ValueError("Missing or changed pinned input: "+name)
        url="https://raw.githubusercontent.com/ValveSoftware/Proton/"+lock["protonCommit"]+"/"+name
        with urllib.request.urlopen(url,timeout=30) as response:raw=response.read(2*1024*1024+1)
        if len(raw)>2*1024*1024 or hashlib.sha256(raw).hexdigest()!=expected:raise ValueError("Upstream checksum mismatch: "+name)
        path.parent.mkdir(parents=True,exist_ok=True)
        pending=path.with_suffix(path.suffix+".pending")
        if pending.is_symlink():raise ValueError("Unsafe pending path")
        pending.write_bytes(raw);pending.replace(path)
        return True
    with ThreadPoolExecutor(max_workers=4) as pool:downloaded=sum(pool.map(prepare,lock["sdkSources"].items()))
    print(json.dumps({"protonCommit":lock["protonCommit"],"verifiedInputs":len(lock["sdkSources"]),"downloaded":downloaded}))

if __name__=="__main__":main()
