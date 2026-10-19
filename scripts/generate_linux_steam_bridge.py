#!/usr/bin/env python3
"""Generate experimental lsteamclient ABI wrappers from pinned Proton SDK headers.

The ABI comes from Clang's AST, including x86 thiscall / struct-return handling.
Only explicitly supported wire types are invoked; other methods report unsupported.
No downloaded SDK header or Valve binary is copied into the application package.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

PIN = "e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6"
OUTPUT_METHODS = {("ISteamUserStats", name) for name in [
    "GetStat", "GetAchievement", "GetAchievementAndUnlockTime", "GetGlobalStat"]}

def records(text):
    decoder = json.JSONDecoder()
    at = 0
    while at < len(text):
        while at < len(text) and text[at].isspace():
            at += 1
        if at == len(text):
            return
        value, at = decoder.raw_decode(text, at)
        if value.get("kind") == "CXXRecordDecl" and value.get("completeDefinition"):
            yield value

def declare(typ, name):
    if "(*)" in typ:
        return typ.replace("(*)", "(*" + name + ")")
    if "(&)" in typ:
        return typ.replace("(&)", "(&" + name + ")")
    return typ + " " + name

def generate(reference, compiler, output):
    lock = json.loads((Path(__file__).resolve().parents[1] / "docs/linux-steam/steam_bridge_sdk.lock.json").read_text())
    if lock["protonCommit"] != PIN:
        raise ValueError("Unexpected Proton SDK lock revision")
    for name, expected in lock["sdkSources"].items():
        if hashlib.sha256((reference / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Pinned SDK input changed: " + name)
    sdk = reference / "lsteamclient/steamworks_sdk_150"
    legacy = reference / "lsteamclient/steamworks_sdk_142/isteamclient.h"
    output.mkdir(parents=True, exist_ok=True)
    old = legacy.read_text(encoding="utf-8-sig")
    start = old.index("class ISteamClient\n")
    end = old.index("\n};", start) + 3
    old = old[start:end].replace("ISteamClient", "ISteamClient017")
    (output / "sdk.h").write_text('#pragma once\n#define STEAM_API_EXPORTS\n#include "steam_api.h"\nclass ISteamUnifiedMessages;\n' + old + '\n')
    source = output / "ast.cpp"
    source.write_text('#include "sdk.h"\n')
    ast = output / "ast.private.json"
    with ast.open("w") as stream:
        subprocess.run([str(compiler), "-std=c++17", "-fms-extensions", "-I" + str(sdk),
                        "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang",
                        "-ast-dump-filter=ISteam", str(source)], stdout=stream, check=True)
    classes = {}
    nested_types = {}
    for obj in records(ast.read_text()):
        methods = [x for x in obj.get("inner", []) if x.get("kind") == "CXXMethodDecl" and x.get("virtual")]
        if methods:
            classes[obj["name"]] = methods
            nested_types[obj["name"]] = [x["name"] for x in obj.get("inner", []) if x.get("kind") in ["EnumDecl", "CXXRecordDecl", "TypedefDecl"] and x.get("name") != obj["name"] and x.get("name")]
    versions = {}
    for header in sorted(sdk.glob("isteam*.h")):
        text = header.read_text(errors="replace")
        names = re.findall(r"class\s+(ISteam\w+)\s*\n?\s*\{", text)
        values = re.findall(r'#define\s+\w*INTERFACE_VERSION\w*\s+"([^"]+)"', text)
        names = [name for name in names if name in classes and not name.endswith("Response")]
        if not names:
            continue
        if len(names) != len(values):
            if values:
                raise ValueError((header.name, names, values))
            continue
        versions.update(zip(names, values))
    versions["ISteamClient017"] = "SteamClient017"
    pe = []
    stubs = []
    server = []
    catalog = []
    for name, version in versions.items():
        stubs.append(f'class Stub{name}:public {name} {{ public:')
        pe.append(f'class Remote{name} final:public {name} {{ public: uint32_t object; explicit Remote{name}(uint32_t id):object(id){{}}')
        for method in classes[name]:
            signature = method["type"]["qualType"]
            result = signature[:signature.index("(")].strip()
            args = [x for x in method.get("inner", []) if x.get("kind") == "ParmVarDecl"]
            types = [x["type"]["qualType"] for x in args]
            for nested in nested_types[name]:
                types = [re.sub(r"\b" + re.escape(nested) + r"\b", name + "::" + nested, typ) for typ in types]
                result = re.sub(r"\b" + re.escape(nested) + r"\b", name + "::" + nested, result)
            names = [f"arg{i}" for i in range(len(args))]
            declarations = [declare(t, n) for t, n in zip(types, names)]
            number = int.from_bytes(hashlib.sha256((version + "::" + method["name"] + signature).encode()).digest()[:4], "big")
            if number < 256 or any(x["id"] == number for x in catalog):
                raise ValueError("Method ID collision")
            stubs.append(f'{result} {method["name"]}({", ".join(declarations)}) override {{' + ('return;' if result == "void" else 'return {};') + '}')
            root = name in ["ISteamClient", "ISteamClient017"]
            getter = root and method["name"].startswith("GetISteam") and result.endswith("*")
            outputs = (name, method["name"]) in OUTPUT_METHODS
            template = ("true" if outputs else "false") + ", " + result
            rest = (", " + ", ".join(names)) if names else ""
            pe.append(f'{result} {method["name"]}({", ".join(declarations)}) override {{')
            ticket = name == "ISteamUser" and method["name"] in ["GetAuthSessionTicket", "GetEncryptedAppTicket"]
            callback_check = root and method["name"] == "Set_SteamAPI_CCheckCallbackRegisteredInProcess"
            if callback_check:
                # Match Proton's explicit dispatch model: request all native
                # callbacks; the game's steam_api dispatches registered ones.
                # A Windows function address must never be invoked in GNU.
                pe.append(f' return Invoke<false,void>(object,{number}u,arg0 != nullptr);')
            elif ticket:
                pe.append(f' return Ticket<{result}>(object,{number}u,arg0,arg1,arg2);')
            elif getter:
                pe.append(f' return reinterpret_cast<{result}>(Wrap({names[-1]}, Invoke<false,uint32_t>(object,{number}u{rest})));')
            else:
                pe.append(f' return Invoke<{template}>(object,{number}u{rest});')
            pe.append('}')
            unverified_root = root and method["name"] in ["CreateSteamPipe", "BReleaseSteamPipe", "ConnectToGlobalUser", "ReleaseUser"]
            server.append(f'case {number}u: {{ auto& entry=session.Get(object,"{version}",{str(not unverified_root).lower()}); auto* target=static_cast<{name}*>(entry.pointer);')
            call = f'target->{method["name"]}({", ".join(names)})'
            if root and method["name"] in ["CreateSteamPipe", "BReleaseSteamPipe", "ConnectToGlobalUser", "ReleaseUser"]:
                hooks = {"CreateSteamPipe": "CreatePipe", "BReleaseSteamPipe": "ReleasePipe", "ConnectToGlobalUser": "ConnectUser", "ReleaseUser": "ReleaseUser"}
                call = f'session.{hooks[method["name"]]}(target' + (", " + ", ".join(names) if names else "") + ')'
            if callback_check:
                server.append('bool present=GetScalar<bool>(input);input.End();target->Set_SteamAPI_CCheckCallbackRegisteredInProcess(present?AllCallbacks:nullptr);return Ok;')
            elif ticket:
                call = "AuthTicket" if method["name"] == "GetAuthSessionTicket" else "EncryptedTicket"
                server.append(f'return session.{call}(entry,target,input,output);')
            elif getter:
                server.append(' '.join(f'auto {n}=ReadArgument<{t}>(input);' for t, n in zip(types, names)))
                parameters = ', '.join(n + '.Get()' for n in names)
                pipe = names[0] if len(names) == 2 else names[1]
                user = "0" if len(names) == 2 else names[0] + '.Get()'
                server.append(f'input.End();session.ValidatePair({pipe}.Get(),{user}); auto* ptr=target->{method["name"]}({parameters});')
                pipe = names[0] if len(names) == 2 else names[1]
                user = "0" if len(names) == 2 else names[0] + '.Get()'
                server.append(f'output.Put(session.Add({names[-1]}.Get(),ptr,{pipe}.Get(),{user}));return Ok;')
            else:
                typestr = ", " + ", ".join(types) if types else ""
                if name == "ISteamUser" and method["name"] == "CancelAuthTicket":
                    call = "session.CancelTicket(target,arg0)"
                server.append(f'return Serve<{template}{typestr}>([&]({", ".join(declarations)}) -> {result} {{ return {call}; }},input,output);')
            server.append('}')
            catalog.append({"id": number, "interface": version, "class": name, "method": method["name"], "signature": signature, "scalarOutputsAllowed": outputs})
        pe.append('};')
        stubs.append('};')
    pe.append('void* Wrap(const char* version,uint32_t object) { if(!object||!version)return nullptr; auto key=std::make_pair(std::string(version),object);auto found=interfaces.find(key);if(found!=interfaces.end())return found->second; void* result=nullptr;')
    for name, version in versions.items():
        pe.append(f'if(!strcmp(version,"{version}"))result=new Remote{name}(object);')
    pe.append('if(result)interfaces.emplace(key,result);return result; }')
    (output / "mock_interfaces.inc").write_text("\n".join(stubs) + "\n")
    (output / "pe_methods.inc").write_text("\n".join(pe) + "\n")
    (output / "server_methods.inc").write_text("\n".join(server) + "\n")
    (output / "versions.inc").write_text("\n".join(f'"{v}",' for v in versions.values()) + "\n")
    manifest = {"schemaVersion": 1, "protonCommit": PIN, "sdk": "1.50", "legacyClientSdk": "1.42", "methods": catalog,
                "sdkSources": {str(p.relative_to(reference)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [*sorted(sdk.glob("*.h")), legacy]}}
    (output / "abi-catalog.json").write_text(json.dumps(manifest, indent=2) + "\n")
    ast.unlink()
    source.unlink()
    print(json.dumps({"interfaces": len(versions), "methods": len(catalog), "output": str(output)}))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.reference, args.clang, args.output)
