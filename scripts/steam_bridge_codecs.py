"""Generate field-wise codecs from pinned SDK AST, never native struct bytes.

Pointer-bearing records, unions, bitfields and private layouts stay unsupported.
Callback IDs and fields are derived from Valve headers; no success is invented.
"""
import json
import re
import subprocess

PRIMITIVES = {"bool", "char", "signed char", "unsigned char", "short", "unsigned short", "int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "float", "double", "CSteamID", "CGameID"}
MANUAL_RECORDS = {'SteamIPAddress_t', 'SteamNetworkingIdentity', 'SteamNetworkingIPAddr', 'SteamControllerState_t'}

def objects(text):
    decoder = json.JSONDecoder()
    at = 0
    while at < len(text):
        while at < len(text) and text[at].isspace(): at += 1
        if at == len(text): break
        value, at = decoder.raw_decode(text, at)
        yield value

def walk(obj):
    yield obj
    for item in obj.get("inner", []): yield from walk(item)

def generate(compiler, sdk, output, source):
    ast = output / "codec-ast.private.json"
    with ast.open("w") as stream:
        subprocess.run([str(compiler), "-std=c++17", "-fms-extensions", "-isystem", str(sdk), "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=_t", str(source)], stdout=stream, check=True)
    entries = list(objects(ast.read_text()))
    records = {o["name"]:o for o in entries if o.get("kind") == "CXXRecordDecl" and o.get("completeDefinition") and o.get("tagUsed") == "struct" and o.get("name", "").endswith("_t")}
    enums = {o["name"] for entry in entries for o in walk(entry) if o.get("kind") == "EnumDecl" and o.get("name")}
    # SDK enums in fields are typedef-free. The field AST also exposes enum
    # declarations through type spelling; check them against pinned headers.
    for header in sdk.glob("*.h"):
        enums.update(re.findall(r"enum\s+(\w+)\s*\{", header.read_text(errors="replace")))
    aliases = {}
    for header in sdk.glob("*.h"):
        for target,name in re.findall(r"typedef\s+([\w ]+?)\s+(\w+)\s*;", header.read_text(errors="replace")):
            aliases[name] = " ".join(target.split())
    allowed = set(PRIMITIVES) | enums | MANUAL_RECORDS
    for _ in range(len(aliases)):
        before=len(allowed)
        allowed.update(name for name,target in aliases.items() if target in allowed)
        if len(allowed)==before:break
    chosen = []
    pending = {name:obj for name,obj in records.items() if name not in MANUAL_RECORDS}
    while pending:
        progress = False
        for name, obj in list(pending.items()):
            fields = [o for o in obj.get("inner", []) if o.get("kind") == "FieldDecl"]
            # Public data-only structs. Constructors/operators are allowed:
            # CSteamID members give several otherwise plain SDK structs one.
            if obj.get("bases") or any(o.get("kind") == "AccessSpecDecl" and o.get("access") != "public" for o in obj.get("inner", [])): continue
            if any(not f.get("name") or f.get("isBitfield") or any(c in f["type"]["qualType"] for c in "*&()") for f in fields): continue
            bases = [re.sub(r"\[\d+\]", "", f["type"].get("desugaredQualType", f["type"]["qualType"])).strip() for f in fields]
            if any(b not in allowed for b in bases): continue
            callback = next((o for o in walk(obj) if o.get("kind") == "EnumConstantDecl" and o.get("name") == "k_iCallback"), None)
            code = next((int(o["value"]) for o in walk(callback) if o.get("kind") == "ConstantExpr" and "value" in o), None) if callback else None
            chosen.append({"name":name, "fields":[f["name"] for f in fields], "callback":code})
            allowed.add(name)
            del pending[name]
            progress = True
        if not progress: break
    codecs = []
    for item in chosen:
        body = "".join(f"ValueField(w, enc, v.{field});" for field in item["fields"])
        codecs.append(f'template<>struct Record<{item["name"]}>{{static constexpr bool supported=true;static void Transfer(Buffer& w,bool enc,{item["name"]}& v){{(void)w;(void)enc;(void)v;{body}}}}};')
    (output / "record_codecs.inc").write_text("\n".join(codecs) + "\n")
    callbacks = [o for o in chosen if o["callback"] is not None and o["name"] != "PS3TrophiesInstalled_t"]
    if len({o["callback"] for o in callbacks}) != len(callbacks): raise ValueError("Duplicate SDK callback ID: " + str([(o["name"],o["callback"]) for o in callbacks if sum(c["callback"] == o["callback"] for c in callbacks)>1]))
    (output / "callback_sizes.inc").write_text("\n".join(f'case {o["callback"]}:return sizeof({o["name"]});' for o in callbacks) + "\n")
    (output / "callback_codecs.inc").write_text("\n".join(f'case {o["callback"]}:return CallbackType<{o["name"]}>(raw,length,wire,encode,decoded,[](Buffer& w,bool enc,{o["name"]}& v){{Record<{o["name"]}>::Transfer(w,enc,v);}});' for o in callbacks) + "\n")
    ast.unlink()
    return {"records":chosen, "callbacks":len(callbacks), "excludedRecords":sorted(pending)}
