/* The host's snapshot field codec preserves explicit APIs; old snapshots remain unknown.
 *
 * A snapshot file is input like any other: a restored context takes no index, size, layout or count the live
 * bridge would not have accepted (a drawable past 2048 squared, an array layout gl*Pointer refuses, a client
 * texture unit or a pixel-store alignment out of range, a matrix stack or unit count past what the capture
 * bounds), and the load fails instead.
 *
 * SLICE:gles-prelude hw/arm/gles-host.c range #include <TargetConditionals.h> | /* Old guest engines retain
 * SLICE:gles-refusals hw/arm/gles-host.c range /* ---------------------------------------------------------------- refusals | /* Only expose formats our decoder accepts
 * SLICE hw/arm/gles-host-snapshot.c.inc range typedef struct { GByteArray | /* ---- per-context GL state
 * SLICE hw/arm/gles-host.c fn gles_type_size gles_array_layout_ok
 * SLICE hw/arm/gles-host-snapshot.c.inc range /* ---- per-context GL state | /* Every matrix of the stack
 * SLICE hw/arm/gles-host-snapshot.c.inc fn snap_gl_ok
 * SLICE hw/arm/gles-host-snapshot.c.inc fn snap_put_map snap_get_map snap_buffer snap_put_array snap_get_array snap_alignment_ok snap_put_host snap_get_host
 * CFLAGS -I$ROOT/include -Wno-pointer-to-int-cast -Wno-pointer-bool-conversion -framework OpenGL -framework VideoToolbox -framework CoreVideo -framework CoreMedia -framework CoreFoundation
 * PKG glib-2.0
 */
#include "ipod-gles.h"
#include "slice.h"
static void check(uint32_t api) {
    GLESHost original={.api_version=api,.inited=true,.fbo=17,.tex=31,.drawable_width=1024,.drawable_height=768,.program=91};
    original.vertex.ptr=0x123400;original.unpack_alignment=4;
    SnapW w={g_byte_array_new()};snap_put_host(&w,&original);
    GLESHost restored={0};SnapR r={w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,4);
    assert(!r.bad && r.off==r.n && restored.api_version==api);
    assert(restored.fbo==17 && restored.program==91 && restored.vertex.ptr==0x123400 && restored.drawable_width==1024);
    g_hash_table_destroy(restored.fbo_drawable);
    /* Version 3 fields begin at inited, without the API word. */
    memset(&restored,0,sizeof restored);r=(SnapR){w.b->data+4,w.b->len-4,0,false};
    snap_get_host(&r,&restored,3);
    assert(!r.bad && r.off==r.n && restored.api_version==0 && restored.program==91 && restored.fbo==17);
    g_hash_table_destroy(restored.fbo_drawable);
    uint32_t invalid=3;memcpy(w.b->data,&invalid,4);
    memset(&restored,0,sizeof restored);r=(SnapR){w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,4);assert(r.bad);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(w.b,true);
}
/* One field of a good saved context changed: the load fails. */
static bool restores(void (*change)(GLESHost *)) {
    GLESHost h={.inited=true,.drawable_width=1024,.drawable_height=768,.unpack_alignment=4};
    h.vertex.client_state=GL_VERTEX_ARRAY;h.vertex.size=3;h.vertex.type=GL_FLOAT;h.vertex.stride=12;
    change(&h);
    SnapW w={g_byte_array_new()};snap_put_host(&w,&h);
    GLESHost restored={0};SnapR r={w.b->data,w.b->len,0,false};
    snap_get_host(&r,&restored,4);
    g_hash_table_destroy(restored.fbo_drawable);g_byte_array_free(w.b,true);
    return !r.bad;
}
static void same(GLESHost *h) {}
static void drawable(GLESHost *h) { h->drawable_width=4096; }
static void no_drawable(GLESHost *h) { h->drawable_height=0; }
static void stride(GLESHost *h) { h->vertex.stride=0x80000000u; }
static void components(GLESHost *h) { h->attr[3].size=0x40000001; h->attr[3].type=0x140C; }
static void client_state(GLESHost *h) { h->color.client_state=GL_VERTEX_ARRAY; }
static void unit(GLESHost *h) { h->client_active_unit=GLES_MAX_TEXUNITS; }
static void alignment(GLESHost *h) { h->pack_alignment=3; }
static void check_gl(void) {
    GLESCtxGL s={.units=2,.mv_depth=1,.proj_depth=1};
    s.unit[0].mat_depth=s.unit[1].mat_depth=1;
    assert(snap_gl_ok(&s));
    s.mv_depth=SNAP_MV_DEPTH+1;assert(!snap_gl_ok(&s));s.mv_depth=1;
    s.proj_depth=0;assert(!snap_gl_ok(&s));s.proj_depth=1;
    s.unit[1].mat_depth=SNAP_AUX_DEPTH+1;assert(!snap_gl_ok(&s));s.unit[1].mat_depth=1;
    s.units=GLES_MAX_TEXUNITS+1;assert(!snap_gl_ok(&s));
}
int main(void) {
    assert(restores(same));
    assert(!restores(drawable) && !restores(no_drawable) && !restores(stride) && !restores(components));
    assert(!restores(client_state) && !restores(unit) && !restores(alignment));
    check_gl();
    check(0);check(1);check(2);puts("PASS: real snapshot field codec preserves API1/2, accepts v3 unknown, rejects corrupt API and out-of-range state"); }
