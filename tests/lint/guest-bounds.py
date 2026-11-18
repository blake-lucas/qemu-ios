#!/usr/bin/env python3
"""Flag guest-written values that reach a host index, length or allocation unchecked.

The rule (README.md, "Guest values"): every index, length, offset and count the guest
wrote (an MMIO or bus write, a descriptor or buffer read from guest memory) goes through
IT_IDX, IT_SIZE or IT_RANGE (include/hw/arm/ipod_touch_guard.h), or an explicit bounds
test, before its first use as an array subscript, a memcpy/memset/allocation length, a
guest-memory read's length into a host buffer, or an MMIO handler's offset.

This is a lint, not a proof: it follows names through one file. Sources are the value
parameter of MMIO write handlers and bus callbacks (I2C send, SSI transfer), buffers
filled by a guest-memory read, values loaded from guest memory, and anything assigned
from those, including struct fields and accessor macros (a field stored from a guest
value is guest-controlled in every function of the file). A value counts as checked when
it passes through IT_IDX/IT_SIZE/IT_RANGE, MIN, `%` or a constant `&` mask, or when an
if/while/for/assert/ternary earlier in the same branch of the same function compares it.
A use the lint can't see is safe takes a `/* bounded: why */` comment on its line or the
line before.

  tests/lint/guest-bounds.py [FILE...]     default: the files in SCOPE; exit 1 on findings
"""
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Our device models and guest-facing host code. The GL bridge (gles-host*.c, guest-gles.c)
# validates through its own fetch helpers and is checked by its own tests.
SCOPE = [
    "hw/arm/ipod_touch_*.c", "hw/arm/s5l89*.c", "hw/arm/ipad1.c", "hw/arm/ipod-agent.c",
    "hw/arm/guest-services.c", "hw/arm/guest-pasteboard.c", "hw/arm/guest-package.c",
    "hw/arm/mrvl8686.c", "hw/arm/nand-image.c", "hw/arm/it_iboot.c", "hw/arm/tsl2561.c",
    "hw/arm/ipod_video.c", "hw/misc/ios_*.c", "hw/i2c/ipod_touch_i2c.c", "hw/intc/pl192.c",
    "net/slirp-dns.c",
]

GUARD = re.compile(r"\b(IT_IDX|IT_SIZE|IT_RANGE|IT_FIELD|MIN|extract32|extract64)\s*\(|%"
                   r"|&\s*\(?\s*(0x[0-9a-fA-F]+|\d+|[A-Z_][A-Z0-9_]*)\b|>>\s*(2[4-9]|3[01])\b")
CMP = re.compile(r"<=|>=|[^-<>]<[^<=]|[^->]>[^>=]|==|!=")
TEST = re.compile(r"\b(if|while|for|assert|g_assert)\b|\?")
BRANCH = re.compile(r"\bcase\b[^;]*:|\bdefault\s*:|\}\s*else\b")
# function: (buffer arg, length arg, direction arg that must be 0/false for a read, or None)
READ_ARGS = {"dma_memory_read": (2, 3, None), "address_space_read": (3, 4, None),
             "cpu_physical_memory_read": (1, 2, None), "cpu_memory_rw_debug": (2, 3, 4),
             "pci_dma_read": (2, 3, None)}
READS = re.compile(r"\b(" + "|".join(READ_ARGS) + r")\s*\(")
LOADS = re.compile(r"\b(ld[ublwq]_(le_|be_)?(phys|p|dma)|ldl_phys|address_space_ld\w*|"
                   r"strto[u]?l+|atoi|atol|g_ascii_strto[u]?ll)\s*\(")
SCANF = re.compile(r"\bsscanf\s*\(")
INT_PARAM = re.compile(r"\b(?:uint32_t|uint64_t|uint16_t|hwaddr|unsigned|int|size_t|int32_t)\s+(\w+)\s*(?=[,)])")
ALLOC = re.compile(r"\b(g_malloc0?|g_try_malloc0?|g_realloc|g_try_realloc|malloc|calloc|realloc|"
                   r"g_new0?|g_try_new0?|g_renew|alloca)\s*\(")
COPY = re.compile(r"\b(memcpy|memmove|memset)\s*\(")
HANDLER = re.compile(r"\(\s*void\s*\*\s*\w+\s*,\s*hwaddr\s+\w+\s*,\s*uint64_t\s+(\w+)\s*,"
                     r"\s*unsigned\s+\w+\s*\)")
BUS_CB = re.compile(r"\(\s*(I2CSlave|SSIPeripheral)\s*\*\s*\w+\s*,\s*(uint8_t|uint32_t)\s+(\w+)\s*\)")
ASSIGN = re.compile(r"^\s*(?:(?:const\s+)?[A-Za-z_]\w*[\s\*]+)?"
                    r"(?P<lhs>\*?\s*[A-Za-z_]\w*(?:\s*\([^;=]*?\))?(?:\s*(?:->|\.)\s*[A-Za-z_]\w*|\s*\[[^\]]*\])*)"
                    r"\s*(?:[+\-|^]|<<)?=(?!=)\s*(?P<rhs>.*);\s*(?:/\*bounded:\*/)?\s*$", re.S)
IDENT = re.compile(r"[A-Za-z_]\w*")
NOT_NAMES = {"if", "while", "for", "switch", "return", "sizeof", "case", "else", "do", "goto",
             "unsigned", "int", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", "hwaddr",
             "int32_t", "int64_t", "const", "struct", "static", "bool", "char", "void", "s"}


def strip(src):
    """Blank comments and literals, keeping lines; a `bounded:` comment becomes a marker."""
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("/*", i) or src.startswith("//", i):
            j = src.find("*/", i + 2) + 2 if src[i + 1] == "*" else src.find("\n", i)
            j = n if j < 2 else j
            body = src[i:j]
            mark = "/*bounded:*/" if "bounded:" in body else ""
            out.append(mark + "\n" * body.count("\n"))
            i = j
        elif src[i] in "\"'":
            q, j = src[i], i + 1
            while j < n and src[j] != q and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(q + q)
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def functions(text):
    """Yield (header, first line number, body text) for each top-level function."""
    depth, line, last_top, start = 0, 0, 0, None
    for i, c in enumerate(text):
        if c == "\n":
            line += 1
        elif c == "{":
            if depth == 0:
                start = (text[last_top:i], line, i)
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start:
                head, sline, si = start
                if head.rstrip().endswith(")"):
                    yield head, sline, text[si:i + 1]
                start, last_top = None, i + 1
        elif c == ";" and depth == 0:
            last_top = i + 1


def statements(body, first_line):
    """Split a body into (line, text) statements at ; { } (string-free text)."""
    out, cur, line, cur_line, depth = [], "", first_line, first_line, 0
    for c in body:
        if not cur:
            cur_line = line
        cur += c
        depth += (c == "(") - (c == ")")
        if c == "\n":
            line += 1
        if c in "{}" or (c == ";" and depth <= 0):      # a for (;;) header stays whole
            out.append((cur_line, cur))
            cur = ""
    if cur.strip():
        out.append((cur_line, cur))
    # a trailing `bounded:` marker belongs to the statement it ends
    for k in range(1, len(out)):
        if out[k][1].lstrip().startswith("/*bounded:*/") and "\n" not in out[k][1].split("/*bounded:*/")[0]:
            out[k - 1] = (out[k - 1][0], out[k - 1][1] + "/*bounded:*/")
    return out


def args_of(text, at):
    """The argument strings of the call whose '(' is at index `at`."""
    depth, cur, args = 0, "", []
    for c in text[at:]:
        if c in "([":
            depth += 1
            if depth == 1:
                continue
        elif c in ")]":
            depth -= 1
            if depth == 0:
                return args + [cur]
        elif c == "," and depth == 1:
            args.append(cur)
            cur = ""
            continue
        cur += c
    return args + [cur]


def subscripts(st):
    """Yield (position, expression) for each [...] subscript in a statement."""
    for m in re.finditer(r"(?<=[\w\)\]])\s*\[", st):
        a = args_of(st, m.end() - 1)
        if a:
            yield m.end() - 1, a[0]


class Taint:
    def __init__(self):
        self.fields, self.accessors = set(), set()

    def names(self, expr, local):
        expr = re.sub(r"\bsizeof\s*\([^()]*(\([^()]*\))?[^()]*\)", " ", expr)
        found = {f for f in re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)", expr) if f in self.fields}
        found |= {a for a in re.findall(r"\b([A-Za-z_]\w*)\s*\(", expr) if a in self.accessors}
        plain = re.sub(r"(?:->|\.)\s*[A-Za-z_]\w*", " ", expr)
        found |= set(IDENT.findall(plain)) & local
        if LOADS.search(expr):
            found.add("guest memory")
        return found


def checked(names, before):
    """A comparison of one of `names` in the statements since the last branch label."""
    for _, st in reversed(before):
        st = re.sub(r"\bsizeof\s*\([^()]*(\([^()]*\))?[^()]*\)", " ", st)
        if TEST.search(st) and any(CMP.search(c) and any(re.search(r"\b" + re.escape(n) + r"\b", c)
                                                        for n in names)
                                   for c in re.split(r"&&|\|\||\?|;", st)):
            return True
        if any(re.search(r"\b" + re.escape(n) + r"\s*=(?!=)[^;]*", st) and GUARD.search(st)
               for n in names):
            return True                 # reassigned through a bound: n = MIN(n, N)
        if BRANCH.search(st):
            return False
    return False


def split_top(st):
    """Split a declaration `T a = x, b = y;` at its top-level commas."""
    parts, depth, cur = [], 0, ""
    for c in st:
        depth += c in "([{"
        depth -= c in ")]}"
        if c == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
    return parts + [cur]


def sources(head, stmts, local, taint):
    """Add the names a function fills from guest values to `local` and `taint`."""
    for rx, g in ((HANDLER, 1), (BUS_CB, 3)):
        m = rx.search(head)
        if m:
            local.add(m.group(g))
    decl = head[head.rfind("}") + 1:]
    if not re.search(r"\bstatic\b", decl):
        # an exported device API takes values another model got from the guest
        local |= set(INT_PARAM.findall(decl[decl.find("("):]))
    for k, (line, st) in enumerate(stmts):
        for m in READS.finditer(st):
            a = args_of(st, m.end() - 1)
            bi, li, wi = READ_ARGS[m.group(1)]
            if len(a) > li and (wi is None or len(a) <= wi or a[wi].strip() in ("0", "false")):
                # the buffer: `buf`, `&x`, `buf + off` (the first name), `s->field` (the last field)
                dst = re.sub(r"\(\s*[\w\s\*]+\)", " ", a[bi]).split("+")[0]
                fields = re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)", dst)
                names = [w for w in IDENT.findall(dst) if w not in NOT_NAMES]
                if fields:
                    taint.fields.add(fields[-1])
                elif names:
                    local.add(names[0])
        for m in SCANF.finditer(st):
            for a in args_of(st, m.end() - 1)[2:]:
                w = re.match(r"\s*&\s*([A-Za-z_]\w*)\s*$", a)
                if w:
                    local.add(w.group(1))
        body = re.sub(r"^(\s*(case\s[^:;]*|default)\s*:)+", "", st.replace("\n", " ").split("{")[-1])
        for part in split_top(body):
            am = ASSIGN.match(part if part.rstrip().endswith(";") else part + ";")
            if not am:
                continue
            lhs, rhs = am.group("lhs"), am.group("rhs")
            t = taint.names(rhs, local)
            if not t or GUARD.search(rhs) or checked(t, stmts[:k]):
                continue
            acc = re.match(r"\*?\s*([A-Za-z_]\w*)\s*\(", lhs)
            if acc:
                taint.accessors.add(acc.group(1))
            elif re.search(r"->|\.", lhs):
                taint.fields.add(re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)", lhs)[-1])
            else:
                local.add(re.match(r"\*?\s*([A-Za-z_]\w*)", lhs).group(1))


def analyze(path):
    text = strip(open(path, encoding="utf-8", errors="replace").read())
    funcs = [(h, l, statements(b, l)) for h, l, b in functions(text)]
    handlers = set()
    for h, _, _ in funcs:
        m = re.search(r"(\w+)\s*\([^()]*\)\s*$", h.strip(), re.S)
        if m and HANDLER.search(h):
            handlers.add(m.group(1))
    taint, findings = Taint(), []
    for _ in range(4):                       # fields feed functions earlier in the file
        before = (set(taint.fields), set(taint.accessors))
        locals_ = []
        for head, _, stmts in funcs:
            local = set()
            for _ in range(3):
                sources(head, stmts, local, taint)
            locals_.append(local)
        if (taint.fields, taint.accessors) == before:
            break

    for (head, _, stmts), local in zip(funcs, locals_):
        for k, (line, st) in enumerate(stmts):
            if "/*bounded:*/" in st or (k and stmts[k - 1][1].rstrip().endswith("/*bounded:*/")
                                        and stmts[k - 1][0] < line):
                continue

            # (kind, expression, where the sink starts in the statement)
            sinks = [("index", e, pos) for pos, e in subscripts(st)]
            for m in COPY.finditer(st):
                a = args_of(st, m.end() - 1)
                if len(a) == 3:
                    sinks.append((m.group(1) + " length", a[2], m.start()))
            for m in ALLOC.finditer(st):
                skip = 2 if m.group(1) == "g_renew" else 1 if re.search(r"realloc|new", m.group(1)) else 0
                sinks += [(m.group(1) + " size", a, m.start()) for a in args_of(st, m.end() - 1)[skip:]]
            for m in READS.finditer(st):
                a = args_of(st, m.end() - 1)
                li = READ_ARGS[m.group(1)][1]
                if len(a) > li:
                    sinks.append((m.group(1) + " length", a[li], m.start()))
            for h in handlers:
                for m in re.finditer(r"(?<![\w.>])" + h + r"\s*\(", st):
                    a = args_of(st, m.end() - 1)
                    if len(a) == 4 and "hwaddr" not in a[1]:
                        sinks.append(("MMIO handler offset", a[1], m.start()))
            for kind, expr, pos in sinks:
                if GUARD.search(expr):
                    continue
                t = taint.names(expr, local)
                # a test earlier in the branch, or earlier in this statement (a && chain, a ternary)
                if t and not checked(t, stmts[:k] + [(line, st[:pos])]):
                    findings.append((line + 1 + st[:pos].count("\n"), kind, " ".join(expr.split()),
                                     ", ".join(sorted(t))))
    return sorted(set(findings))


def main(argv):
    files = argv or sorted({f for p in SCOPE for f in glob.glob(os.path.join(ROOT, p))})
    total = 0
    for f in files:
        for line, kind, expr, names in analyze(f):
            total += 1
            print(f"{os.path.relpath(f, ROOT)}:{line}: {kind} `{expr}` uses guest-written {names} unchecked")
    print(f"guest-bounds: {total} finding(s) in {len(files)} file(s)", file=sys.stderr)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
