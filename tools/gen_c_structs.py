#!/usr/bin/env python3
"""
Generate C struct mirrors of the C++ parsed syntax structs.

Reads every struct definition from syntax/*.hpp and emits:

  * capi/bs_structs.h        -- plain C mirror structs (BsHevc*/BsAvc*/
                               BsVvc*/BsAv1*/BsVp9*/BsVp8*)
  * capi/bs_structs_conv.hpp -- C++ conversion functions bs_conv(src, dst)

Full field-by-field mirror.  Mapping:

  std::uintN_t / std::intN_t  -> uintN_t / intN_t
  bool                        -> uint8_t
  int / unsigned              -> unchanged
  std::size_t                 -> uint64_t
  float / double              -> unchanged
  std::array<T,N>             -> T name[..]   (N may be a constexpr constant)
  std::vector<T>              -> uint32_t name_count; T* name;
  std::optional<T>            -> uint8_t name_has; T name;
  enum (class)                -> int
  nested struct               -> struct Bs<Codec><Name>  (value member)
  std::span / string_view     -> const uint8_t*
  std::string                 -> const char*

Run:  python3 tools/gen_c_structs.py
"""

import os
import re
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYNTAX = os.path.join(ROOT, "syntax")

PRIMITIVE = {
    "std::uint8_t": "uint8_t", "std::uint16_t": "uint16_t",
    "std::uint32_t": "uint32_t", "std::uint64_t": "uint64_t",
    "std::int8_t": "int8_t", "std::int16_t": "int16_t",
    "std::int32_t": "int32_t", "std::int64_t": "int64_t",
    "bool": "uint8_t", "int": "int", "unsigned": "unsigned",
    "unsigned int": "unsigned int", "float": "float",
    "double": "double", "char": "char", "std::size_t": "uint64_t",
    "std::string": "const char*",
}

EXCLUDE = re.compile(r"(NalUnit|NalPayloadView|NalUnitHeader|RbspView|View|Obu)$")


def wanted(name):
    return not EXCLUDE.search(name)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def clean_body(body):
    """Remove nested struct/class/enum/union definitions so their members
    do not leak into the enclosing struct."""
    out = []
    i = 0
    n = len(body)
    depth = 0
    while i < n:
        if depth == 0:
            m = re.match(r"\b(?:struct|class|enum|union)\b[^{}]*\{", body[i:])
            if m:
                d = 0
                k = i + m.end() - 1
                while k < n:
                    if body[k] == "{":
                        d += 1
                    elif body[k] == "}":
                        d -= 1
                        if d == 0:
                            break
                    k += 1
                # skip the nested type and any trailing declarator up to ';'
                i = k + 1
                while i < n and body[i] not in ";}" and body[i] != "\n":
                    i += 1
                if i < n and body[i] == ";":
                    i += 1
                continue
        c = body[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth < 0:
                break
        out.append(c)
        i += 1
    return "".join(out)


def find_structs(text):
    """Return list of (path, namespace_list, body) where path is the list of
    struct names from outermost to this one (so nested structs are captured)."""
    out = []

    def scan(text, ns, path_prefix, brace, ns_depth, ns_stack):
        i = 0
        n = len(text)
        while i < n:
            m = re.match(r"\bnamespace\s+(?:([A-Za-z_]\w*)\s*)?\{", text[i:])
            if m:
                nm = m.group(1) if m.group(1) else ""
                ns.append(nm)
                brace += 1
                ns_depth.append((nm, brace))
                i += m.end()
                continue
            m = re.match(r"\b(?:struct|class)\s+([A-Za-z_]\w*)\s*(?::[^{]*?)?\{",
                         text[i:])
            if m and not re.search(r"\benum\s+$", text[max(0, i - 8):i]):
                name = m.group(1)
                path = path_prefix + [name]
                d = 0
                j = i + m.end() - 1
                while j < n:
                    if text[j] == "{":
                        d += 1
                    elif text[j] == "}":
                        d -= 1
                        if d == 0:
                            break
                    j += 1
                raw = text[i + m.end():j]
                out.append((path, [x for x in ns if x], raw))
                # recurse to capture nested structs
                scan(raw, list(ns), path, 0, [], [])
                i = j + 1
                continue
            if text[i] == "{":
                brace += 1
            elif text[i] == "}":
                brace -= 1
                while ns_depth and ns_depth[-1][1] > brace:
                    ns_depth.pop()
                    if ns_stack:
                        ns_stack.pop()
                    if ns:
                        ns.pop()
            i += 1

    scan(text, [], [], 0, [], [])
    return [(path, ns, clean_body(raw)) for (path, ns, raw) in out]


def find_aliases(text):
    """Return dict alias_name -> underlying type string for `using X = ...;`."""
    out = {}
    for m in re.finditer(
            r"\busing\s+([A-Za-z_]\w*)\s*=\s*([^;]+);", text):
        out[m.group(1)] = m.group(2).strip()
    return out


def find_constants(text):
    """Return dict NAME -> literal value for constexpr constants."""
    out = {}
    for m in re.finditer(
            r"constexpr\s+[\w:<>, \*\&]+\s+(\w+)\s*=\s*([^;]+);", text):
        out[m.group(1)] = m.group(2).strip()
    return out


def split_top(s):
    toks = s.split()
    if len(toks) < 2:
        return None, None, []
    name = toks[-1]
    typepart = " ".join(toks[:-1]).strip()
    dims = []
    mm = re.match(r"^(.*?)((?:\[[^\]]*\]\s*)+)$", name)
    if mm:
        name = mm.group(1)
        dims = re.findall(r"\[([^\]]*)\]", mm.group(2))
    return typepart, name, dims


def match_angle(s):
    """Return (inner, rest) for the first balanced <...> in s, or None."""
    i = s.find("<")
    if i < 0:
        return None
    depth = 0
    j = i
    while j < len(s):
        c = s[j]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return s[i + 1:j], s[j + 1:]
        j += 1
    return None


def split_args(s):
    args = []
    depth = 0
    cur = ""
    for ch in s:
        if ch in "<([":
            depth += 1
            cur += ch
        elif ch in ">)]":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


def elem_cbase(t, codec, all_cnames, resolve):
    t = t.strip()
    ptr = ""
    if t.endswith("*"):
        ptr = "*"
        t = t[:-1].strip()
    if "span" in t or "string_view" in t or "bitset" in t:
        return "const uint8_t*" + ptr
    if t in PRIMITIVE:
        return PRIMITIVE[t] + ptr
    base = t.replace("std::", "").replace("bs::avc::", "").replace("bs::", "").strip()
    if resolve is not None:
        r = resolve(base)
        if r is not None:
            return r + ptr
    cand = "Bs" + codec + base
    if cand in all_cnames:
        return cand + ptr
    other = "Avc" if codec == "Hevc" else "Hevc"
    cand2 = "Bs" + other + base
    if cand2 in all_cnames:
        return cand2 + ptr
    if re.match(r"[A-Z]\w*$", base):
        return "int" + ptr
    return base + ptr


def parse_type(typepart, codec, all_names, resolve):
    """Return (kind, cbase, dims, orig) resolving nested std::array templates.

    `orig` is the original (un-mapped) C++ element type, used to tell a
    genuine `bool` apart from an integer that also maps to `uint8_t` in C.
    """
    t = typepart.strip()
    t = re.sub(r"\b(const|volatile|static|inline)\b", "", t).strip()
    if "span" in t or "string_view" in t:
        return ("span", "const uint8_t*", [], t)
    if "bitset" in t:
        return ("scalar", "const uint8_t*", [], t)
    ma = match_angle(t)
    if ma is not None:
        inner, rest = ma
        head = t[:t.find("<")].strip()
        if head in ("std::vector", "vector"):
            args = split_args(inner)
            et, ec, ed, eo = parse_type(args[0], codec, all_names, resolve)
            return ("vector", ec, [], eo)
        if head in ("std::optional", "optional"):
            args = split_args(inner)
            et, ec, ed, eo = parse_type(args[0], codec, all_names, resolve)
            return ("optional", ec, [], eo)
        if head in ("std::array", "array"):
            args = split_args(inner)
            et, ec, ed, eo = parse_type(args[0], codec, all_names, resolve)
            return ("array", ec, [args[1].strip()] + ed, eo)
    return ("scalar", elem_cbase(t, codec, all_names, resolve), [], t)


def classify(typepart, codec, all_names, resolve):
    k, cb, dims, orig = parse_type(typepart, codec, all_names, resolve)
    return (k, cb, dims, orig)


def parse_members(body, codec, all_names, aliases, resolve):
    stmts = []
    depth = 0
    cur = ""
    for ch in body:
        if ch == "{":
            depth += 1
            cur += ch
        elif ch == "}":
            depth -= 1
            cur += ch
        elif ch == ";" and depth == 0:
            stmts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        stmts.append(cur)

    members = []
    for s in stmts:
        s = s.strip()
        if not s:
            continue
        if re.match(r"^(public|private|protected):?$", s):
            continue
        # skip methods, static/constexpr constants, enums, aliases, friends
        if "(" in s:
            continue
        if re.match(r"^(static|constexpr|inline|enum|using|typedef|friend|virtual|explicit)\b", s):
            continue
        if "struct" in s or "enum" in s or "class" in s:
            continue
        s2 = re.split(r"(=|\{)", s, 1)[0].strip()
        if not s2:
            continue
        typepart, name, namedims = split_top(s2)
        if not typepart or not name:
            continue
        if not re.match(r"[A-Za-z_]\w*$", name):
            continue
        typepart = expand_aliases(typepart, aliases)
        kind, cbase, innerdims, orig = classify(typepart, codec, all_names, resolve)
        dims = namedims + innerdims
        members.append({"name": name, "kind": kind,
                        "cbase": cbase, "dims": dims, "orig": orig})
    # if a span member would collide with an existing <name>_size field,
    # suppress the generated size field (the struct already carries it)
    names = {m["name"] for m in members}
    for m in members:
        if m["kind"] == "span" and (m["name"] + "_size") in names:
            m["no_size"] = True
    return members


def expand_aliases(tp, aliases):
    for _ in range(3):
        changed = False
        for a, d in aliases.items():
            new = re.sub(r"\b%s\b" % a, d, tp)
            if new != tp:
                changed = True
            tp = new
        if not changed:
            break
    return tp


def main():
    files = sorted(glob.glob(os.path.join(SYNTAX, "*.hpp")))

    constants = {}
    aliases = {}
    for path in files:
        with open(path, "r", encoding="utf-8") as f:
            text = strip_comments(f.read())
            constants.update(find_constants(text))
            aliases.update(find_aliases(text))

    structs = {}
    for path in files:
        base = os.path.basename(path)
        codec = next(
            (name for prefix, name in (
                ("hevc", "Hevc"), ("avc", "Avc"), ("vvc", "Vvc"),
                ("av1", "Av1"), ("vp9", "Vp9"), ("vp8", "Vp8"),
            ) if base.startswith(prefix)),
            "Avc",
        )
        with open(path, "r", encoding="utf-8") as f:
            text = strip_comments(f.read())
        for pth, ns_list, body in find_structs(text):
            if not wanted(pth[-1]):
                continue
            cname = "Bs" + codec + "".join(pth)
            if cname in structs:
                continue
            qname = "::" + "::".join(ns_list + pth)
            structs[cname] = (codec, pth, qname, body)

    all_cnames = set(structs.keys())

    # nested + global name resolution for struct-typed members
    children = {}
    global_bare = {}
    for cname, (codec, pth, qname, body) in structs.items():
        global_bare.setdefault(pth[-1], []).append(cname)
        for cname2, (codec2, pth2, qname2, body2) in structs.items():
            if len(pth2) == len(pth) + 1 and pth2[:-1] == pth:
                children.setdefault(cname, {})[pth2[-1]] = cname2

    def _resolve(base, codec, scname):
        ch = children.get(scname)
        if ch and base in ch:
            return ch[base]
        if base in global_bare:
            for c in global_bare[base]:
                if c.startswith("Bs" + codec):
                    return c
            return global_bare[base][0]
        return None

    parsed = {}
    for cname, (codec, pth, qname, body) in structs.items():
        parsed[cname] = (codec, pth, qname,
                         parse_members(body, codec, all_cnames, aliases,
                                       lambda b: _resolve(b, codec, cname)))
    structs = parsed

    def deps(name):
        codec, pth, qname, members = structs[name]
        d = set()
        for m in members:
            if m["kind"] in ("scalar", "array") and m["cbase"] in structs:
                d.add(m["cbase"])
        return d

    order = []
    emitted = set()
    remaining = set(structs.keys())
    while remaining:
        progressed = False
        for name in sorted(remaining):
            if deps(name) <= emitted:
                order.append(name)
                emitted.add(name)
                remaining.discard(name)
                progressed = True
        if not progressed:
            for name in sorted(remaining):
                order.append(name)
                emitted.add(name)
                remaining.discard(name)

    emit_c_header(structs, order, constants)
    emit_conv(structs, order)
    emit_c_free(structs, order)
    print("generated %d structs, %d constants" % (len(structs), len(constants)))


def cdecl(m):
    """Return the C declaration for a member (no trailing semicolon)."""
    cb = m["cbase"]
    is_struct = bool(re.match(r"Bs(Hevc|Avc|Vvc|Av1|Vp9|Vp8)(\w+)$", cb))
    base = ("struct " + cb) if is_struct else cb
    dimstr = "".join("[%s]" % d for d in m["dims"])
    if m["kind"] == "vector":
        return "uint32_t %s_count;\n    %s* %s" % (m["name"], base, m["name"])
    if m["kind"] == "span":
        if m.get("no_size"):
            return "const uint8_t* %s" % m["name"]
        return "uint32_t %s_size; const uint8_t* %s" % (m["name"], m["name"])
    return base + " " + m["name"] + dimstr


def emit_c_header(structs, order, constants):
    L = []
    L.append("/* AUTO-GENERATED by tools/gen_c_structs.py -- do not edit. */")
    L.append("#ifndef BS_STRUCTS_H")
    L.append("#define BS_STRUCTS_H")
    L.append("")
    L.append("#include <stdint.h>")
    L.append("#include <stddef.h>")
    L.append("")
    # constants used as array sizes (C only; C++ has the constexpr already)
    used = set()
    for _, _, _, members in structs.values():
        for m in members:
            for d in m["dims"]:
                if not d.isdigit() and d in constants:
                    used.add(d)
    if used:
        L.append("#ifndef __cplusplus")
        for c in sorted(used):
            L.append("#define %s %s" % (c, constants[c]))
        L.append("#else")
        L.append("#include <cstddef>")
        for c in sorted(used):
            L.append("inline constexpr std::size_t %s = %s;" % (c, constants[c]))
        L.append("#endif")
        L.append("")
    L.append("#ifdef __cplusplus")
    L.append('extern "C" {')
    L.append("#endif")
    L.append("")
    for name in order:
        L.append("struct %s;" % name)
    L.append("")
    for name in order:
        codec, pth, qname, members = structs[name]
        L.append("typedef struct %s {" % name)
        for m in members:
            L.append("    " + cdecl(m) + ";")
        L.append("} %s;" % name)
        L.append("")
    L.append("#ifdef __cplusplus")
    L.append("}")
    L.append("#endif")
    L.append("")
    L.append("#endif /* BS_STRUCTS_H */")
    with open(os.path.join(ROOT, "capi", "bs_structs.h"), "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print("wrote capi/bs_structs.h")


def is_struct_cbase(cb):
    return bool(re.match(r"Bs(Hevc|Avc|Vvc|Av1|Vp9|Vp8)(\w+)$", cb.strip().rstrip("*").strip()))


def cpp_scalar(src_expr, cbase, orig):
    if is_struct_cbase(cbase):
        return "bs_conv(%s, %s)" % (src_expr, "%s")
    if orig == "bool":
        return "(%s ? 1 : 0)" % src_expr
    if cbase == "uint8_t":
        return "(uint8_t)%s" % src_expr
    if cbase in ("int", "uint16_t", "uint32_t", "uint64_t", "int8_t",
                 "int16_t", "int32_t", "int64_t", "float", "double",
                 "unsigned", "unsigned int"):
        return "(%s)%s" % (cbase, src_expr)
    return src_expr


def emit_conv(structs, order):
    L = []
    L.append("/* AUTO-GENERATED by tools/gen_c_structs.py -- do not edit. */")
    L.append("#pragma once")
    L.append("")
    L.append('#include "bs_structs.h"')
    for path in sorted(glob.glob(os.path.join(SYNTAX, "*.hpp"))):
        L.append('#include "%s"' % os.path.relpath(path, ROOT).replace("\\", "/"))
    L.append("#include <bsparser.hpp>")
    L.append("#include <cstdint>")
    L.append("#include <cstring>")
    L.append("#include <new>")
    L.append("")
    L.append("namespace bs { namespace capi {")
    L.append("")

    for name in order:
        codec, pth, qname, members = structs[name]
        L.append("inline void bs_conv(const %s& src, %s& dst);"
                 % (qname, name))
    L.append("")

    def conv_member(m, src, dst):
        name = m["name"]
        kind = m["kind"]
        cb = m["cbase"]
        is_struct = is_struct_cbase(cb)
        # any fixed/known-size array (C array, std::array, or array of struct)
        if m["dims"]:
            ndim = len(m["dims"])
            lines = []
            idx = ""
            for k in range(ndim):
                i = "_i%d" % k
                lines.append("    for (std::size_t %s = 0; %s < %s; ++%s) {"
                             % (i, i, m["dims"][k], i))
                idx += "[%s]" % i
            if is_struct:
                assign = "bs_conv(%s.%s%s, %s.%s%s);" % (src, name, idx, dst, name, idx)
            elif kind == "span":
                assign = "%s.%s%s = %s.%s%s.data();" % (dst, name, idx, src, name, idx)
            else:
                assign = "%s.%s%s = %s;" % (dst, name, idx,
                                            cpp_scalar("%s.%s%s" % (src, name, idx), cb, m.get("orig", "")))
            lines.append("    " * (ndim + 1) + assign)
            for k in range(ndim):
                lines.append("    " * (ndim - k) + "}")
            return "\n".join(lines)
        if kind == "span":
            if m.get("no_size"):
                return "    %s.%s = %s.%s.data();" % (dst, name, src, name)
            return ("    %s.%s_size = static_cast<uint32_t>(%s.%s.size());\n"
                    "    %s.%s = %s.%s.data();"
                    % (dst, name, src, name, dst, name, src, name))
        if kind == "vector":
            if is_struct:
                inner = ("            bs_conv(%s.%s[_i], %s.%s[_i]);"
                         % (src, name, dst, name))
            else:
                inner = "            %s.%s[_i] = %s.%s[_i];" % (dst, name, src, name)
            return ("    %s.%s_count = static_cast<uint32_t>(%s.%s.size());\n"
                    "    if (!%s.%s.empty()) {\n"
                    "        %s.%s = new %s[%s.%s_count];\n"
                    "        for (std::size_t _i = 0; _i < %s.%s_count; ++_i) {\n"
                    "%s\n"
                    "        }\n"
                    "    }"
                    % (dst, name, src, name, src, name,
                       dst, name, cb, dst, name, dst, name, inner))
        if kind == "optional":
            if is_struct:
                inner = "        bs_conv(%s.%s.value(), %s.%s);" % (src, name, dst, name)
            else:
                inner = "        %s.%s = %s.%s.value();" % (dst, name, src, name)
            return ("    %s.%s_has = %s.%s.has_value() ? 1 : 0;\n"
                    "    if (%s.%s_has) {\n%s\n    }"
                    % (dst, name, src, name, dst, name, inner))
        if kind == "scalar":
            if is_struct:
                return "    bs_conv(%s.%s, %s.%s);" % (src, name, dst, name)
            return "    %s.%s = %s;" % (dst, name, cpp_scalar("%s.%s" % (src, name), cb, m.get("orig", "")))
        return ""

    for name in order:
        codec, pth, qname, members = structs[name]
        L.append("inline void bs_conv(const %s& src, %s& dst) {"
                 % (qname, name))
        for m in members:
            c = conv_member(m, "src", "dst")
            for line in c.split("\n"):
                if line:
                    L.append(line)
        L.append("}")
        L.append("")

    L.append("} /* namespace bs::capi */")
    L.append("} /* namespace bs */")
    with open(os.path.join(ROOT, "capi", "bs_structs_conv.hpp"), "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print("wrote capi/bs_structs_conv.hpp")


def emit_c_free(structs, order):
    """Emit bs_free_<cname> helpers that release heap memory owned by a
    converted C struct (the `new[]` buffers backing vector members).  Span
    pointers point into the source and are never owned, so they are skipped.
    Mirrors the C API's ownership contract: the library frees a struct once
    the callback that received it returns (or when a report is destroyed)."""
    L = []
    L.append("/* AUTO-GENERATED by tools/gen_c_structs.py -- do not edit. */")
    L.append("#ifndef BS_STRUCTS_FREE_H")
    L.append("#define BS_STRUCTS_FREE_H")
    L.append("")
    L.append("#include <cstddef>")
    L.append("#include <capi/bs_structs.h>")
    L.append("")
    L.append("namespace bs {")
    L.append("namespace capi {")
    L.append("")
    for name in order:
        codec, pth, qname, members = structs[name]
        L.append("inline void bs_free_%s(%s* s) {" % (name, name))
        L.append("    if (!s)")
        L.append("        return;")
        for m in members:
            if m["kind"] == "vector":
                L.append("    delete[] s->%s;" % m["name"])
        L.append("}")
        L.append("")
    L.append("} /* namespace bs::capi */")
    L.append("} /* namespace bs */")
    L.append("")
    L.append("#endif /* BS_STRUCTS_FREE_H */")
    with open(os.path.join(ROOT, "capi", "bs_structs_free.hpp"), "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print("wrote capi/bs_structs_free.hpp")


if __name__ == "__main__":
    main()
