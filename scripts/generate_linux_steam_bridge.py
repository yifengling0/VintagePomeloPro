#!/usr/bin/env python3
"""Generate experimental lsteamclient ABI wrappers from pinned Proton SDK headers.

Method signatures come from Clang AST. PE vtables explicitly implement the MSVC
record-return ABI, which differs from MinGW C++ record returns on x86 and x64.
Only explicitly supported wire types are invoked; other methods report unsupported.
No downloaded SDK header or Valve binary is copied into the application package.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import steam_bridge_codecs
from steam_bridge_contracts import SCALAR_OUTPUTS, spans

PIN = "e91ca2be0df2cef4c230cbbc0b86604d73a0bbf6"
# Proton handle_method_c uses an explicit result buffer for record returns.
# Keep the PE vtable separate from the GNU SDK classes: MinGW inheritance would
# silently use its C++ return ABI for CSteamID and corrupt the game caller.
RECORD_RETURNS = {"CSteamID", "CGameID", "SteamIPAddress_t", "InputDigitalActionData_t",
                  "InputAnalogActionData_t", "InputMotionData_t"}
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
    output.mkdir(parents=True, exist_ok=True)
    legacy_classes = []
    legacy_headers = []
    legacy_interfaces = [
        {"class": "ISteamClient017", "sourceClass": "ISteamClient", "sdk": "142", "header": "isteamclient.h", "version": "SteamClient017"},
        {"class": "ISteamClient014", "sourceClass": "ISteamClient", "sdk": "129", "header": "isteamclient.h", "version": "SteamClient014"},
        *lock.get("legacyInterfaces", []), *lock.get("coreInterfaces", []),
        *lock.get("extendedInterfaces", [])]
    for item in legacy_interfaces:
        legacy = reference / f'lsteamclient/steamworks_sdk_{item["sdk"]}' / item["header"]
        legacy_headers.append(legacy)
        old = legacy.read_text(encoding="utf-8-sig", errors="replace")
        start = re.search(r"class\s+" + item["sourceClass"] + r"\s*\{", old).start()
        end = old.index("\n};", start) + 3
        legacy_classes.append(re.sub(r'\b' + item["sourceClass"] + r'\b', item["class"], old[start:end]))
    imports = []
    for item in lock.get("typeImports", []):
        text = (reference / item["source"]).read_text(encoding="utf-8-sig", errors="replace")
        if item["kind"] in ("forward", "classForward"):
            kind = 'struct' if item['kind'] == 'forward' else 'class'
            if not re.search(r'\b' + kind + r'\s+' + item["symbol"] + r'\b', text):
                raise ValueError(item["symbol"])
            imports.append(kind + ' ' + item["symbol"] + ';')
        elif item["kind"] == "typedefStruct":
            match = re.search(r'typedef\s+struct\s*\{[^{}]*\}\s*' + item["symbol"] + r'\s*;', text)
            if not match: raise ValueError(item["symbol"])
            imports.append('#pragma pack(push, ' + str(item['pack']) + ')\n' + match.group() + '\n#pragma pack(pop)')
        elif item["kind"] == "typedef":
            match = re.search(r'(?:extern "C" )?typedef[^;{}]+\b' + item["symbol"] + r'\b[^;{}]*;', text)
            if not match: raise ValueError(item["symbol"])
            imports.append(match.group())
        else:
            start = re.search(item["kind"] + r'\s+' + item["symbol"] + r'\s*\{', text).start()
            end = text.index("\n};", start) + 3 if item["kind"] == "struct" else text.index("};", start) + 2
            declaration=text[start:end]
            if item.get('pack')=='callback':
                declaration='#if defined(VALVE_CALLBACK_PACK_SMALL)\n#pragma pack(push,4)\n#else\n#pragma pack(push,8)\n#endif\n'+declaration+'\n#pragma pack(pop)'
            imports.append(declaration)
    annotations = ["ARRAY_COUNT", "OUT_ARRAY_COUNT", "OUT_ARRAY_CALL", "OUT_STRING", "OUT_STRING_COUNT", "OUT_STRUCT", "CALL_RESULT", "DESC", "BUFFER_COUNT", "OUT_BUFFER_COUNT", "IGNOREATTR", "METHOD_DESC"]
    compatibility = "\n".join("#ifndef " + name + "\n#define " + name + "(...)\n#endif" for name in annotations)
    (output / "sdk.h").write_text('#pragma once\n#define STEAM_API_EXPORTS\n#include "steam_api.h"\n#include "isteamappticket.h"\nclass ISteamUnifiedMessages;\nclass ISteamMasterServerUpdater;\n' + compatibility + "\n" + "\n".join(imports) + "\n" + "\n".join(legacy_classes) + '\n')
    source = output / "ast.cpp"
    source.write_text('#include "sdk.h"\n')
    ast = output / "ast.private.json"
    with ast.open("w") as stream:
        subprocess.run([str(compiler), "-std=c++17", "-fms-extensions", "-ferror-limit=0", "-I" + str(sdk),
                        "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang",
                        "-ast-dump-filter=ISteam", str(source)], stdout=stream, check=True)
    codecs = steam_bridge_codecs.generate(compiler, sdk, output, source)
    classes = {}
    nested_types = {}
    virtual_members = {}
    for obj in records(ast.read_text()):
        methods = [x for x in obj.get("inner", []) if x.get("kind") == "CXXMethodDecl" and x.get("virtual")]
        if methods:
            if obj.get('bases'):
                raise ValueError('Review inherited vtable slots: ' + obj['name'])
            classes[obj["name"]] = methods
            virtual_members[obj['name']] = [x for x in obj.get('inner', []) if
                (x.get('kind')=='CXXMethodDecl' and x.get('virtual')) or
                (x.get('kind')=='CXXDestructorDecl' and not x.get('isImplicit'))]
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
    overrides = {item["version"] for item in legacy_interfaces}
    versions = {name:version for name,version in versions.items() if version not in overrides}
    for item in legacy_interfaces:
        versions[item["class"]] = item["version"]
    aliases = lock.get("interfaceAliases", {})
    if any(value not in versions.values() for value in aliases.values()):raise ValueError("Alias has no canonical ABI")
    (output / "version_aliases.inc").write_text("\n".join(f'if(!strcmp(version,"{alias}"))return "{target}";' for alias,target in sorted(aliases.items()))+"\n")
    pe = []
    stubs = []
    server = []
    catalog = []
    capabilities = []
    interface_layouts = {}
    native_classes, native_stubs = ['#pragma once\n#include "sdk.h"'], ['#pragma once\n#include "gnu_interfaces.h"']
    for name, version in versions.items():
        family = re.sub(r"\d+$", "", name)
        stubs.append(f'class Stub{name}:public {name} {{ public:')
        pe.append(f'class Remote{name} {{ public: uint32_t object; explicit Remote{name}(uint32_t id):object(id){{}}')
        native_classes.append(f'class Gnu{name} {{ public:')
        native_stubs.append(f'class StubGnu{name}:public Gnu{name} {{ public:')
        remote_members = []
        windows_slot, gnu_slot = 0, 0
        for method in virtual_members[name]:
            if method['kind'] == 'CXXDestructorDecl':
                # MSVC has one entry, Itanium has two. Like Proton, the public
                # Steam singleton destructor is never forwarded or freed.
                remote_members.append((method['name'],['virtual void BridgeDestructor() {}'],None))
                native_classes.append(f'virtual ~Gnu{name}() {{}}')
                windows_slot += 1; gnu_slot += 2
                continue
            pe_begin=len(pe)
            signature = method["type"]["qualType"]
            result = signature[:signature.index("(")].strip()
            args = [x for x in method.get("inner", []) if x.get("kind") == "ParmVarDecl"]
            types = [x["type"]["qualType"] for x in args]
            for nested in nested_types[name]:
                types = [re.sub(r"\b" + re.escape(nested) + r"\b", name + "::" + nested, typ) for typ in types]
                result = re.sub(r"\b" + re.escape(nested) + r"\b", name + "::" + nested, result)
            names = [f"arg{i}" for i in range(len(args))]
            declarations = [declare(t, n) for t, n in zip(types, names)]
            # The Linux steamclient ABI takes CGameID parameters by reference
            # even where the public Windows SDK declares a by-value argument.
            # This is the same conversion as Proton's Class.write_definition.
            native_types = ['CGameID &' if t == 'CGameID' else t for t in types]
            native_declarations = [declare(t,n) for t,n in zip(native_types,names)]
            native_classes.append(f'virtual {result} {method["name"]}({", ".join(native_declarations)}) = 0;')
            native_stubs.append(f'{result} {method["name"]}({", ".join(native_declarations)}) override {{' + ('return;' if result=='void' else 'return {};') + '}')
            number = int.from_bytes(hashlib.sha256((version + "::" + method["name"] + signature).encode()).digest()[:4], "big")
            if number < 256 or any(x["id"] == number for x in catalog):
                raise ValueError("Method ID collision")
            stubs.append(f'{result} {method["name"]}({", ".join(declarations)}) override {{' + ('return;' if result == "void" else 'return {};') + '}')
            root = family == "ISteamClient"
            getter = root and method["name"].removeprefix("DEPRECATED_").startswith("GetISteam") and result.endswith("*")
            outputs = (family, method["name"]) in SCALAR_OUTPUTS
            plan = spans(family, method["name"], types)
            wiretypes, expressions = list(types), list(names)
            for index, (direction, extent, limited) in plan.items():
                if direction == 'inout':
                    if not types[index].endswith('*'): raise ValueError((name, method['name'], index))
                    wiretypes[index] = f'InOut<{types[index][:-1].strip()}>'
                    condition=',bool('+extent+')' if extent else ''
                    expressions[index] = f'InOutValue({names[index]}{condition})'
                    continue
                if direction == 'reference':
                    if not types[index].endswith('&'): raise ValueError((name, method['name'], index))
                    wiretypes[index] = f'OutputReferenceValue<{types[index][:-1].strip()}>'
                    expressions[index] = f'OutputReference({names[index]})'
                    continue
                if index >= len(types) or not types[index].endswith("*"):
                    raise ValueError((name, method["name"], index, types))
                element = types[index][:-1].strip()
                wiretypes[index] = f'Span<{element},{str(direction != "input").lower()},{str(direction == "text").lower()},{str(limited).lower()}>'
                function = {"input":"InputSpan", "output":"OutputSpan", "text":"TextSpan"}[direction]
                if limited: function += "<true>"
                expressions[index] = f"{function}({names[index]},{extent})"
            template = ("true" if outputs else "false") + ", " + result
            rest = (", " + ", ".join(names)) if names else ""
            mapped_rest = (", " + ", ".join(expressions)) if expressions else ""
            record_return = result in RECORD_RETURNS
            pe_declarations = ([result + "* returned"] if record_return else []) + declarations
            pe_result = result + "*" if record_return else result
            pe.append(f'static_assert((std::is_class_v<{result}> || std::is_union_v<{result}>) == {str(record_return).lower()}, "Review new SDK record return ABI");')
            pe.append(f'virtual {pe_result} {method["name"]}({", ".join(pe_declarations)}) {{ try {{')
            ticket = family == "ISteamUser" and method["name"] in ["GetAuthSessionTicket", "GetEncryptedAppTicket"]
            create_user = root and method['name'] in ('CreateLocalUser','CreateGlobalUser')
            api_result = family == "ISteamUtils" and method["name"] == "GetAPICallResult"
            callback_check = root and method["name"] == "Set_SteamAPI_CCheckCallbackRegisteredInProcess"
            warning_hook = family in ("ISteamClient", "ISteamUtils") and method["name"] == "SetWarningMessageHook"
            ownership_ticket = family == "ISteamAppTicket" and method["name"] == "GetAppOwnershipTicketData"
            path_output = (family in ["ISteamApps", "ISteamAppList"] and method["name"] == "GetAppInstallDir") or (family == "ISteamUser" and method["name"] == "GetUserDataFolder")
            if record_return:
                pe.append(f' if(!returned)Failed({number}u,Invalid); *returned=Invoke<{template}>(object,{number}u{rest}); return returned;')
            elif api_result:
                pe.append(f' return InterfaceCallResult(object,{number}u{rest});')
            elif callback_check:
                # Match Proton's explicit dispatch model: request all native
                # callbacks; the game's steam_api dispatches registered ones.
                # A Windows function address must never be invoked in GNU.
                pe.append(f' return RegisterCallbackCheck(object,{number}u,arg0);')
            elif warning_hook:
                pe.append(f' return RegisterWarningHook(object,{number}u,arg0);')
            elif path_output:
                leading = ',arg0' if len(args) == 3 else ''
                pe.append(f' return PathOutput<{result}>(object,{number}u,{names[-2]},{names[-1]}{leading});')
            elif ownership_ticket:
                pe.append(f' return AppOwnershipTicket(object,{number}u{rest});')
            elif ticket:
                pe.append(f' return Ticket<{result}>(object,{number}u{rest});')
            elif getter:
                pe.append(f' return reinterpret_cast<{result}>(Interface({names[-1]}, Invoke<false,uint32_t>(object,{number}u{rest})));')
            else:
                pe.append(f' return Invoke<{template}>(object,{number}u{mapped_rest});')
            pe.append(f'}}catch(const WireError&){{Failed({number}u,Invalid);}} }}')
            unverified_root = root and method["name"] in ["CreateSteamPipe", "BReleaseSteamPipe", "ConnectToGlobalUser", "ReleaseUser", "CreateLocalUser", "CreateGlobalUser"]
            server.append(f'case {number}u: {{ auto& entry=session.Get(object,"{version}",{str(not unverified_root).lower()}); [[maybe_unused]] auto* target=static_cast<Gnu{name}*>(entry.pointer);')
            call = f'target->{method["name"]}({", ".join(names)})'
            if root and method["name"] in ["CreateSteamPipe", "BReleaseSteamPipe", "ConnectToGlobalUser", "ReleaseUser"]:
                hooks = {"CreateSteamPipe": "CreatePipe", "BReleaseSteamPipe": "ReleasePipe", "ConnectToGlobalUser": "ConnectUser", "ReleaseUser": "ReleaseUser"}
                call = f'session.{hooks[method["name"]]}(target' + (", " + ", ".join(names) if names else "") + ')'
            if create_user:
                server.append('bool present=GetScalar<bool>(input);if(!present)return Invalid;')
                extra=''
                if len(types)==2:
                    server.append(f'auto account=GetValue<{types[1]}>(input);')
                    extra=',account'
                server.append(f'input.End();int pipe=0;auto user=session.CreateUser([&](int* created){{return target->{method["name"]}(created{extra});}},pipe);PutScalar(output,user);PutScalar(output,pipe);return Ok;')
            elif api_result:
                server.append('auto call=GetScalar<SteamAPICall_t>(input);int expected=GetScalar<int>(input);input.End();return session.CallResult(entry.pipe,call,expected,output);')
            elif callback_check:
                server.append('bool present=GetScalar<bool>(input);input.End();target->Set_SteamAPI_CCheckCallbackRegisteredInProcess(present?AllCallbacks:nullptr);return Ok;')
            elif warning_hook:
                server.append('bool enabled=GetScalar<bool>(input);input.End();session.RegisterWarningHook(entry,target,enabled);return Ok;')
            elif path_output:
                leading = 'true' if len(args) == 3 else 'false'
                parameters = 'app,buffer,capacity' if len(args) == 3 else 'buffer,capacity'
                server.append(f'return ServePathOutput<{result},{leading}>([&](AppId_t app,char* buffer,int capacity) -> {result} {{ return target->{method["name"]}({parameters}); }},input,output);')
            elif ownership_ticket:
                server.append('return ServeAppOwnershipTicket(target,input,output);')
            elif ticket:
                call = "AuthTicket<" + str(len(args) == 4).lower() + ">" if method["name"] == "GetAuthSessionTicket" else "EncryptedTicket"
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
                typestr = ", " + ", ".join(wiretypes) if types else ""
                if family == "ISteamUser" and method["name"] == "CancelAuthTicket":
                    call = "session.CancelTicket(target,arg0)"
                if family == "ISteamUser" and method["name"] == "GetAuthTicketForWebApi":
                    call = "session.TrackTicket(entry,target," + call + ")"
                check = ""
                if plan:
                    checks = []
                    for index, (direction, extent, _) in plan.items():
                        if direction in ('inout', 'reference'): continue
                        expected = re.sub(r"arg(\d+)", lambda m: "std::get<"+m[1]+">(arguments).Get()", extent)
                        checks.append(f'if(std::get<{index}>(arguments).count != (uint64_t)({expected}))throw WireError();')
                    check = ",[&](auto& arguments){" + "".join(checks) + "}"
                server.append(f'return Serve<{template}{typestr}>([&]({", ".join(declarations)}) -> {result} {{ return {call}; }},input,output{check});')
            server.append('}')
            if getter or callback_check or warning_hook or ownership_ticket or ticket or path_output or api_result or create_user:
                supported = "true"
            else:
                checks = [f'Argument<{str(outputs).lower()},{typ}>' for typ in wiretypes]
                checks += [f'(Value<{result}>||std::is_void_v<{result}>||std::is_same_v<{result},const char*>)']
                supported = " && ".join(checks)
            capabilities.append(f'printf("{number} %u\\n",unsigned({supported}));')
            catalog.append({"id": number, "interface": version, "class": name, "method": method["name"], "signature": signature, "windowsVtableSlot": windows_slot, "gnuVtableSlot": gnu_slot, "gnuParameterTypes": native_types, "scalarOutputsAllowed": outputs, "bufferContract": {str(k):list(v) for k,v in plan.items()}, "windowsRecordReturn": record_return, "windowsPathOutput": path_output})
            remote_members.append((method['name'],pe[pe_begin:],catalog[-1]))
            del pe[pe_begin:]
            windows_slot += 1; gnu_slot += 1
        # Match MSVC/Proton: group overloads at their first declaration, with
        # the last overload first. GNU retains the original declaration order.
        groups={}
        for method_name, body, metadata in remote_members:
            groups.setdefault(method_name,[]).append((body,metadata))
        windows_slot=0
        for group in groups.values():
            for body,metadata in reversed(group):
                pe.extend(body)
                if metadata is not None:metadata['windowsVtableSlot']=windows_slot
                windows_slot+=1
        interface_layouts[version]={'windowsSlots':windows_slot,'gnuSlots':gnu_slot,
            'windowsOrder':[metadata['id'] if metadata else None for group in groups.values() for _,metadata in reversed(group)]}
        pe.append('};')
        stubs.append('};')
        native_classes.append('};');native_stubs.append('};')
    (output / 'gnu_interfaces.h').write_text('\n'.join(native_classes)+'\n')
    (output / 'gnu_stubs.h').write_text('\n'.join(native_stubs)+'\n')
    # Test fixtures may still need the SDK150 base classes to model older
    # clients; they are not exported as a substitute for the canonical ABI.
    for name in sorted(set(classes) - set(versions)):
        stubs.append(f'class Stub{name}:public {name} {{ public:')
        for method in classes[name]:
            signature=method["type"]["qualType"]
            result=signature[:signature.index("(")].strip()
            args=[o for o in method.get("inner",[]) if o.get("kind")=="ParmVarDecl"]
            declarations=[declare(o["type"]["qualType"], f"arg{i}") for i,o in enumerate(args)]
            stubs.append(f'{result} {method["name"]}({", ".join(declarations)}) override {{' + ('return;' if result=="void" else 'return {};') + '}')
        stubs.append('};')
    pe.append('void* Wrap(const char* version,uint32_t object) { if(!object||!version)return nullptr;version=CanonicalVersion(version); auto key=std::make_pair(std::string(version),object);auto found=interfaces.find(key);if(found!=interfaces.end())return found->second; void* result=nullptr;')
    for name, version in versions.items():
        pe.append(f'if(!strcmp(version,"{version}"))result=MakeRemote<Remote{name}>(version,object,{len(virtual_members[name])});')
    pe.append('if(result)interfaces.emplace(key,result);return result; }')
    (output / "mock_interfaces.inc").write_text("\n".join(stubs) + "\n")
    (output / "pe_methods.inc").write_text("\n".join(pe) + "\n")
    (output / "server_methods.inc").write_text("\n".join(server) + "\n")
    (output / "root_versions.inc").write_text("\n".join(f'"{v}",' for n,v in versions.items() if re.sub(r"\d+$", "", n) == "ISteamClient") + "\n")
    (output / "versions.inc").write_text("\n".join(f'"{v}",' for v in versions.values()) + "\n")
    (output / "capabilities.cpp").write_text('#include "sdk.h"\n#include "wire.h"\n#include <cstdio>\nusing namespace hmosbridge;\nint main(){\n' + "\n".join(capabilities) + "\n}\n")
    manifest = {"schemaVersion": 1, "protonCommit": PIN, "sdk": "1.50", "legacyClientSdk": "1.42", "legacyClientSdks": lock["legacyClientSdks"], "legacyInterfaces": legacy_interfaces, "methods": catalog, "interfaceLayouts":interface_layouts,
                "codecs": codecs, "interfaceAliases": aliases, "sdkSources": lock["sdkSources"]}
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
