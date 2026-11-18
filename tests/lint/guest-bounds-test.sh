#!/bin/bash
# Guest-written values reach no host index, length or allocation unchecked (README.md, "Guest values").
#
# First the lint itself: a sample device with one unchecked use of each kind must give exactly those findings, and
# the same uses guarded must give none. Then every file in tests/lint/guest-bounds.py's SCOPE must be clean.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LINT="$ROOT/tests/lint/guest-bounds.py"
TMP="$(mktemp -d /tmp/guest-bounds.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/bad.c" <<'EOF'
static void dev_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DevState *s = opaque;
    switch (addr) {
    case 0:
        s->index = val;
        break;
    case 4:
        if (val < 16) {
            s->ok = val;
        }
        break;
    case 8:
        s->buf[s->index] = 1;
        s->buf[s->ok] = 1;
        memcpy(s->buf, s->src, val);
        s->data = g_malloc(val);
        break;
    }
}

static void dev_dma(DevState *s)
{
    uint32_t desc[4];
    dma_memory_read(&address_space_memory, s->base, desc, sizeof(desc), MEMTXATTRS_UNSPECIFIED);
    uint32_t off = desc[1], n = desc[2];
    s->table[off] = 0;
    dev_write(s, off, 0, 4);
    dma_memory_read(&address_space_memory, s->base, s->buf, n, MEMTXATTRS_UNSPECIFIED);
    s->table[IT_IDX("dev", off, 16)] = 0;
    if (n > sizeof(s->buf)) {
        return;
    }
    memcpy(s->buf, s->src, n);
    s->table[off & 15] = 0;
    s->table[off] = 0;    /* bounded: the sample's annotated exception */
}
EOF
want="bad.c:14: index
bad.c:16: memcpy length
bad.c:17: g_malloc size
bad.c:27: index
bad.c:28: MMIO handler offset
bad.c:29: dma_memory_read length"
got="$(python3 "$LINT" "$TMP/bad.c" 2>/dev/null | sed -E 's#^.*/(bad\.c:[0-9]+: [^`]*) `.*#\1#; s/ $//')"
if [ "$got" != "$want" ]; then
    printf 'FAIL the sample device\n-- want\n%s\n-- got\n%s\n' "$want" "$got"
    exit 1
fi
echo "PASS the lint flags the sample's six unchecked uses and none of its checked ones"

if python3 "$LINT"; then
    echo "PASS every file in scope"
else
    echo "FAIL the findings above: bound each value (IT_IDX/IT_SIZE/IT_RANGE or a test) before its use"
    exit 1
fi
