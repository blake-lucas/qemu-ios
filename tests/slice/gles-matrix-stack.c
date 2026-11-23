/* The GL bridge's matrix stacks as deep as the guest's GPU has them, on a host whose stacks are shallower: a fake ES
 * 1.1 driver with Adreno's depths (modelview 16, projection 4, texture 2 per unit; the MBX's texture stack is 4).
 *
 * Field & Stream Fishing (GitHub issue 13) nests its texture matrix three deep: on Adreno the third push was refused,
 * the matching pop undid the second, and every later glyph and sprite was drawn with the wrong texture matrix. Each
 * sequence here runs through gles_matrix_push/pop against the shallow driver and, as the reference, straight against
 * a driver with deep stacks; the live matrices and the depths the guest reads must agree after every call.
 *
 * SLICE hw/arm/gles-host.c define GLES_MAX_TEXUNITS|GLES_KEPT_MATRICES|GLES_MATRIX_STACKS
 * SLICE hw/arm/gles-host.c typedef GLESMatrixStack
 * SLICE:fns hw/arm/gles-host.c range static const GLenum gles_stack_depth_pname | /* The stack of kind 0
 * SLICE:fns hw/arm/gles-host.c fn gles_reject gles_matrix_stack_index gles_matrix_mode_kind gles_host_stack_max
 * SLICE:fns hw/arm/gles-host.c fn gles_matrix_stack_get gles_matrix_stack_check gles_matrix_push gles_matrix_pop
 */
#include <GL/gl.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#include "slice.h"

typedef struct {
    GLESMatrixStack matrix_stack[GLES_MATRIX_STACKS];
    uint32_t matrices_kept;
    GLint host_stack_max[3];
    GLenum error;
} GLESHost;
static GLESHost gh;
static unsigned refusals;
static bool gles_refuse(const char *fmt, ...) { refusals++; return true; }
#include "fns.h"

/* ---- the fake driver: per-kind limits, two texture units, one error flag */
enum { UNITS = 2, DEEP = 64 };
typedef struct { GLfloat m[DEEP][16]; int depth; } Stack;   /* depth counts the live matrix: 1 when empty */
typedef struct {
    int max[3];
    Stack stack[2 + UNITS];
    GLenum mode, error;
    int unit;
} Driver;
static Driver shallow, deep, *cur;

static void driver_init(Driver *d, int mv, int proj, int tex)
{
    memset(d, 0, sizeof(*d));
    d->max[0] = mv; d->max[1] = proj; d->max[2] = tex;
    d->mode = GL_MODELVIEW;
    for (int k = 0; k < 2 + UNITS; k++) {
        d->stack[k].depth = 1;
        for (int i = 0; i < 4; i++) d->stack[k].m[0][i * 5] = 1;
    }
}
static int kind_of(GLenum mode) { return mode == GL_MODELVIEW ? 0 : mode == GL_PROJECTION ? 1 : 2; }
static Stack *top_stack(void) { int k = kind_of(cur->mode); return &cur->stack[k < 2 ? k : 2 + cur->unit]; }
static GLfloat *live(void) { Stack *s = top_stack(); return s->m[s->depth - 1]; }
static void set_error(GLenum e) { if (!cur->error) cur->error = e; }

void glMatrixMode(GLenum mode) { cur->mode = mode; }
void glActiveTexture(GLenum unit) { cur->unit = unit - GL_TEXTURE0; }
GLenum glGetError(void) { GLenum e = cur->error; cur->error = GL_NO_ERROR; return e; }
void glPushMatrix(void)
{
    Stack *s = top_stack();
    if (s->depth >= cur->max[kind_of(cur->mode)]) { set_error(GL_STACK_OVERFLOW); return; }
    memcpy(s->m[s->depth], s->m[s->depth - 1], sizeof(s->m[0]));
    s->depth++;
}
void glPopMatrix(void)
{
    Stack *s = top_stack();
    if (s->depth <= 1) { set_error(GL_STACK_UNDERFLOW); return; }
    s->depth--;
}
void glLoadMatrixf(const GLfloat *m) { memcpy(live(), m, 16 * sizeof(GLfloat)); }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat *m = live();
    for (int r = 0; r < 4; r++) m[12 + r] += m[r] * x + m[4 + r] * y + m[8 + r] * z;
}
void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat *m = live();
    for (int r = 0; r < 4; r++) { m[r] *= x; m[4 + r] *= y; m[8 + r] *= z; }
}
void glGetFloatv(GLenum pname, GLfloat *v)
{
    GLenum mode = pname == GL_MODELVIEW_MATRIX ? GL_MODELVIEW : pname == GL_PROJECTION_MATRIX ? GL_PROJECTION
                : GL_TEXTURE, saved = cur->mode;
    assert(pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX);
    cur->mode = mode;
    memcpy(v, live(), 16 * sizeof(GLfloat));
    cur->mode = saved;
}
void glGetIntegerv(GLenum pname, GLint *v)
{
    switch (pname) {
    case GL_MATRIX_MODE: *v = cur->mode; return;
    case GL_ACTIVE_TEXTURE: *v = GL_TEXTURE0 + cur->unit; return;
    case GL_MAX_MODELVIEW_STACK_DEPTH: *v = cur->max[0]; return;
    case GL_MAX_PROJECTION_STACK_DEPTH: *v = cur->max[1]; return;
    case GL_MAX_TEXTURE_STACK_DEPTH: *v = cur->max[2]; return;
    case GL_MODELVIEW_STACK_DEPTH: *v = cur->stack[0].depth; return;
    case GL_PROJECTION_STACK_DEPTH: *v = cur->stack[1].depth; return;
    case GL_TEXTURE_STACK_DEPTH: *v = cur->stack[2 + cur->unit].depth; return;
    }
    abort();
}

/* ---- one guest call, through the bridge on the shallow driver and directly on the deep one */
static unsigned checks;
static void compare(void)
{
    static const GLenum depths[3] = { GL_MODELVIEW_STACK_DEPTH, GL_PROJECTION_STACK_DEPTH, GL_TEXTURE_STACK_DEPTH };
    GLenum mode = shallow.mode;
    int unit = shallow.unit;
    assert(mode == deep.mode && unit == deep.unit);
    for (int k = 0; k < 3; k++) {
        for (int u = 0; u < (k == 2 ? UNITS : 1); u++) {
            GLint want, got;
            GLenum m = k == 0 ? GL_MODELVIEW : k == 1 ? GL_PROJECTION : GL_TEXTURE;
            cur = &deep; deep.mode = m; deep.unit = u; glGetIntegerv(depths[k], &want);
            GLfloat w[16]; memcpy(w, live(), sizeof(w));
            cur = &shallow; shallow.mode = m; shallow.unit = u; got = gles_matrix_stack_get(depths[k]);
            if (got != want || memcmp(w, live(), sizeof(w))) {
                fprintf(stderr, "stack %d unit %d: depth %d (want %d), live m[12]=%g (want %g)\n",
                        k, u, got, want, live()[12], w[12]);
                abort();
            }
            checks++;
        }
    }
    shallow.mode = deep.mode = mode;
    shallow.unit = deep.unit = unit;
}
static GLenum errors(void)
{
    GLenum host, want;
    cur = &deep; want = glGetError();
    cur = &shallow; host = glGetError();
    GLenum got = gh.error ? gh.error : host;
    gh.error = 0;
    assert(got == want);
    return got;
}
#define BOTH(call) do { cur = &shallow; call; cur = &deep; call; } while (0)
static void mode(GLenum m) { BOTH(glMatrixMode(m)); }
static void unit(int u) { BOTH(glActiveTexture(GL_TEXTURE0 + u)); }
static void translate(float x) { BOTH(glTranslatef(x, x / 2, 0)); }
static void scale(float x) { BOTH(glScalef(x, x, 1)); }
static void push(void) { cur = &shallow; gles_matrix_push(); cur = &deep; glPushMatrix(); errors(); compare(); }
static void pop(void) { cur = &shallow; gles_matrix_pop(); cur = &deep; glPopMatrix(); errors(); compare(); }

int main(void)
{
    driver_init(&shallow, 16, 4, 2);       /* Adreno 650/618's ES 1.1 */
    driver_init(&deep, 16, 16, 16);        /* the reference: as deep as the guest's GPU */

    /* The game's text: a texture matrix three deep, per glyph, many times over. */
    mode(GL_TEXTURE);
    for (int glyph = 0; glyph < 40; glyph++) {
        push(); scale(1.0f / 512); push(); translate(glyph * 3.0f); push(); scale(0.5f);
        translate(1);
        pop(); pop(); pop();
    }
    assert(shallow.stack[2].depth == 1 && gh.matrices_kept == 0);

    /* Kept entries on both units at once, the active unit switched between pushes. */
    unit(0); push(); push(); translate(5); push(); translate(7);
    unit(1); push(); scale(2); push(); push(); translate(3);
    unit(0); push(); scale(3);
    unit(1); pop(); pop();
    unit(0); pop(); pop(); pop(); pop();
    unit(1); pop();
    unit(0);
    assert(gh.matrices_kept == 0 && refusals == 0);

    /* Projection past the host's 4 and modelview within its 16, interleaved with texture. */
    mode(GL_PROJECTION);
    for (int i = 0; i < 6; i++) { push(); translate(i); }
    mode(GL_MODELVIEW); push(); translate(9); push();
    mode(GL_TEXTURE); push(); push(); push(); translate(2);
    mode(GL_PROJECTION); for (int i = 0; i < 6; i++) pop();
    mode(GL_TEXTURE); pop(); pop(); pop();
    mode(GL_MODELVIEW); pop(); pop();
    assert(gh.matrices_kept == 0 && refusals == 0);

    /* The maximum depths read at least the guest's (the front end lowers them to the GPU's). */
    cur = &shallow;
    assert(gles_matrix_stack_get(GL_MAX_TEXTURE_STACK_DEPTH) == GLES_KEPT_MATRICES);
    assert(gles_matrix_stack_get(GL_MAX_PROJECTION_STACK_DEPTH) == GLES_KEPT_MATRICES);
    assert(gles_matrix_stack_get(GL_MAX_MODELVIEW_STACK_DEPTH) == 16);

    /* Overflow at GLES_KEPT_MATRICES and underflow at the bottom, as a deep driver raises them. */
    mode(GL_TEXTURE);
    for (int i = 1; i < GLES_KEPT_MATRICES; i++) { push(); translate(1); }
    cur = &shallow; gles_matrix_push(); cur = &deep; glPushMatrix();
    assert(errors() == GL_STACK_OVERFLOW);
    compare();
    for (int i = 1; i < GLES_KEPT_MATRICES; i++) pop();
    cur = &shallow; gles_matrix_pop(); cur = &deep; glPopMatrix();
    assert(errors() == GL_STACK_UNDERFLOW);
    assert(refusals == 1);                  /* the host's underflow is reported as before */
    compare();

    printf("gles-matrix-stack: %u comparisons\n", checks);
    return 0;
}
