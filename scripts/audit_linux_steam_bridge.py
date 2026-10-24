#!/usr/bin/env python3
"""Compare generated transport capabilities with a pinned Proton Git tree.

This is static coverage, not a claim of device or Steam authentication success.
No account, game data, tickets or Steam session are read.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path, PurePosixPath
import re

PIN = "e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6"

def verify_windows_vtables(catalog,upstream,reference,blobs=None):
    """Check actual MSVC/Proton slot order, including overload ordinals."""
    for version,layout in catalog['interfaceLayouts'].items():
        family=upstream[version];relative=f'lsteamclient/win{family}.c'
        data=(reference/relative).read_bytes()
        if blobs is not None:
            assert hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()==blobs[relative],relative
        entries=re.findall(r'VTABLE_ADD_FUNC\(win'+re.escape(family+'_'+version)+r'_(\w+)\)',data.decode())
        if len(entries)!=layout['windowsSlots']:raise ValueError((version,'vtable size',len(entries),layout['windowsSlots']))
        ordinal=Counter()
        for method in [m for m in catalog['methods'] if m['interface']==version]:
            ordinal[method['method']]+=1
            expected=method['method']+('_'+str(ordinal[method['method']]) if ordinal[method['method']]>1 else '')
            actual=entries[method['windowsVtableSlot']]
            if expected!=actual:raise ValueError((version,method['windowsVtableSlot'],expected,actual))
        ids={m['id']:m for m in catalog['methods'] if m['interface']==version}
        for slot,identifier in enumerate(layout['windowsOrder']):
            if identifier is None and entries[slot]!='destructor':raise ValueError((version,'destructor',slot))
            if identifier is not None and ids[identifier]['windowsVtableSlot']!=slot:raise ValueError((version,'slot identity',slot))
    return len(catalog['interfaceLayouts'])


def audit(catalog, tree):
    if catalog["protonCommit"] != PIN or tree["sha"] != PIN or tree.get("truncated"):
        raise ValueError("Expected the complete, pinned Proton Git tree")
    upstream = {}
    for entry in tree["tree"]:
        path = PurePosixPath(entry["path"])
        match = re.fullmatch(r"cpp(ISteam\w+?)_(.+)\.cpp", path.name)
        if path.parent.as_posix() == "lsteamclient" and entry["type"] == "blob" and match:
            upstream[match[2]] = match[1]
    if not upstream:
        raise ValueError("No upstream Steamworks interfaces found")
    methods = catalog["methods"]
    if any(type(m.get("wireSupported")) is not bool for m in methods):
        raise ValueError("Run the compiled capability audit first")
    generated = {m["interface"] for m in methods}
    declared = Counter(m["interface"] for m in methods)
    supported = Counter(m["interface"] for m in methods if m["wireSupported"])
    families = sorted(set(upstream.values()))
    rows = []
    for family in families:
        versions = {v for v, f in upstream.items() if f == family}
        rows.append({"family": family, "upstreamVersions": len(versions),
                     "generatedVersions": len(versions & generated),
                     "declaredMethods": sum(declared[v] for v in versions),
                     "wireSupportedMethods": sum(supported[v] for v in versions)})
    return {
        "schemaVersion": 1, "protonCommit": PIN, "kind": "static_transport_coverage",
        "authenticationVerified": False, "remotePcGameVerified": False,
        "summary": {"upstreamVersions": len(upstream), "generatedVersions": len(generated),
                    "declaredMethods": len(methods), "wireSupportedMethods": sum(supported.values()),
                    "recordCodecs": len(catalog["codecs"]["records"]),
                    "callbackCodecs": catalog["codecs"]["callbacks"]},
        "families": rows,
        "missingVersions": [{"interface": v, "family": upstream[v]} for v in sorted(set(upstream) - generated)],
        "generatedWithoutUpstreamWrapper": sorted(generated - set(upstream)),
        "unsupportedMethods": [{k: m[k] for k in ("id", "interface", "method", "signature")} for m in methods if not m["wireSupported"]],
        "excludedRecords": catalog["codecs"]["excludedRecords"],
        "limitations": [
            "A generated interface or compilable codec does not prove the official client implements that version or method.",
            "Historical callback layouts outside the pinned common type set require separate ABI review.",
            "Pointer-bearing HTML/network messages, callback objects, custom signaling, and mutable SDK-owned objects require explicit ownership/lifetime implementations.",
            "SteamParamStringArray_t input arrays are copied into call-owned storage; this does not imply support for arbitrary pointer arrays.",
            "The current RPC helper accepts one connection; launcher-to-child and simultaneous game connections remain unverified.",
            "A wire frame is limited to 1 MiB; larger cloud data requires a separately reviewed transfer mechanism.",
            "Steam ownership and authentication results remain official; no successful ticket verification is inferred from source verification.",
        ],
    }


def markdown(report):
    s = report["summary"]
    lines = ["# Steamworks 接口桥静态覆盖审计", "",
             f"上游：Valve Proton `{PIN}`。以下为独立 ABI 和传输编解码统计，不能作为游戏实机通过或票据认证成功的证明。", "",
             f"上游 {s['upstreamVersions']} 个接口版本；已生成 {s['generatedVersions']} 个；声明 {s['declaredMethods']} 个方法，其中 {s['wireSupportedMethods']} 个满足实际 C++ 传输约束；{s['recordCodecs']} 个结构、{s['callbackCodecs']} 个回调支持按字段转换。方法数包含不同接口版本的重复方法。", "",
             "| 接口族 | 上游版本 | 已生成版本 | 声明方法 | 可编解码方法 |",
             "|---|---:|---:|---:|---:|"]
    for r in report["families"]:
        lines.append(f"| {r['family']} | {r['upstreamVersions']} | {r['generatedVersions']} | {r['declaredMethods']} | {r['wireSupportedMethods']} |")
    lines += ["", "## 当前边界", "",
              "- `SteamUser023` 已具备独立虚表、新版认证票据身份参数和 Web API 票据回调；远端欧卡仍须使用新候选实测。",
              "- 数据结构按字段转换；Windows 指针、函数地址及 GNU 原始结构内存不能直接跨进程传递。",
              "- 已补齐上游所有接口版本的声明及独立虚表；Windows 同名重载按 MSVC 规则分组倒序，GNU 保留原声明顺序；析构槽位、结构返回缓冲区和 CGameID 参数按各自 ABI 生成。不能将这一点等同于每个方法都可用。",
              "- StringArray 输入已逐字符串复制到服务端本次调用拥有的存储；缓冲区按已审核的方向和长度处理，容量指针支持输入／输出以及空缓冲区查询。",
              "- 未实现的方法明确报错。HTML 像素与网络消息对象所有权、函数回调／自定义信令、多进程启动器生命周期、完整历史回调 ABI 和超过 1 MiB 的存档传输仍需独立实现；逐项缺口见 JSON。",
              "- 官方客户端是否提供某个接口，只有运行时才能确认；源核验通过不等于游戏服务器票据核验通过。", "",
              "## 重建审计", "", "```sh",
              "python3 scripts/audit_linux_steam_bridge.py --catalog workspace_temp/steam-linux-bridge/generated/abi-catalog.json --upstream-tree <pinned-proton-tree.json> --reference <pinned-proton-cache> --output <audit.json> --markdown docs/linux-steam/STEAMWORKS_COVERAGE.zh-CN.md",
              "```", "",
              "先运行 `build_linux_steam_bridge.py`，它编译并执行与两端相同的 C++ wire traits。JSON 报告列出每个缺失版本和不可编解码的方法，不能仅按声明存在判断支持。",
              "", "来源：[Proton lsteamclient](https://github.com/ValveSoftware/Proton/tree/" + PIN + "/lsteamclient)、[Steamworks API](https://partner.steamgames.com/doc/sdk/api)、[身份认证与拥有权](https://partner.steamgames.com/doc/features/auth)。", ""]
    if report.get('windowsVtablesVerified'):
        lines[6:6] = [f"已按 Git blob 摘要核验固定上游的 Windows 包装源文件，并逐槽比对 {report['windowsVtablesVerified']} 个虚表，包含重载序号及析构占位。", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--upstream-tree", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    parser.add_argument('--reference',type=Path,help='Also verify every Windows slot against checksum-pinned Proton wrappers')
    args = parser.parse_args()
    result = audit(json.loads(args.catalog.read_text()), json.loads(args.upstream_tree.read_text()))
    if args.reference:
        tree=json.loads(args.upstream_tree.read_text())
        blobs={entry['path']:entry['sha'] for entry in tree['tree'] if entry['type']=='blob'}
        upstream={r['interface']:r['family'] for r in result['missingVersions']}
        for entry in tree['tree']:
            path=PurePosixPath(entry['path']);match=re.fullmatch(r'cpp(ISteam\w+?)_(.+)\.cpp',path.name)
            if path.parent.as_posix()=='lsteamclient' and match:upstream[match[2]]=match[1]
        result['windowsVtablesVerified']=verify_windows_vtables(json.loads(args.catalog.read_text()),upstream,args.reference,blobs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True)
        args.markdown.write_text(markdown(result))
    print(json.dumps(result["summary"]))


if __name__ == "__main__":
    main()
