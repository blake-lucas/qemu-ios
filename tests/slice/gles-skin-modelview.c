/* After a skinned draw the host's modelview is the guest's again, with the client-array cache on or off. gles_skin
 * draws OES_matrix_palette meshes under an identity modelview and gles_unbind_arrays loads the guest's back; the
 * cache made gles_unbind_arrays return before it did, so the modelview stayed identity until the guest next loaded
 * it.
 *
 * Field & Stream Fishing (GitHub issue 13) draws a fish's body and then its fins and jaw, each skinned, and loads
 * the second mesh's palette from the modelview with glLoadPaletteFromModelViewMatrixOES, without touching the
 * modelview in between. The fins took the identity: they were drawn at the camera, a big fin floating mid-screen,
 * and the fish lost its tail and lower jaw.
 *
 * SLICE hw/arm/gles-host.c define GLES_MAX_TEXUNITS|GLES_TEXCOORD_BIT|GLES_SKIN_BIT|GLES_MAX_PALETTE|GLES_FIXED
 * SLICE hw/arm/gles-host.c fn gles_elem gles_skin gles_unbind_arrays
 */
/* No GL header: the fake driver below is the GL these functions see, on any host the gate runs on. */
typedef unsigned int GLenum;
typedef int GLint;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef int GLsizei;
#define GL_BYTE 0x1400
#define GL_SHORT 0x1402
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_LIGHTING 0x0B50
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_MODELVIEW 0x1700
#define GL_TEXTURE 0x1702
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glGetFloatv(GLenum pname, GLfloat *v);
void glGetIntegerv(GLenum pname, GLint *v);
GLboolean glIsEnabled(GLenum cap);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p);
void glNormalPointer(GLenum type, GLsizei stride, const void *p);
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef void CPUState;
typedef struct {
    uint32_t enabled, size, type;
    bool host_enabled, host_pointer_valid;
    const uint8_t *data;        /* the test's: what gles_array_bytes hands back */
    uint32_t stride;
} GLESArray;
typedef struct {
    GLESArray vertex, color, normal, weight, matrix_index, texcoord[8];
    bool palette_on;
    float palette[11][16];
    float *skin;
    size_t skin_size;
    GLfloat skin_modelview[16];
    unsigned client_active_unit;
    uint64_t t_arrays;
} GLESHost;
static GLESHost gh;
static bool gles_array_cache;
static uint64_t gles_t(void) { return 0; }
static const uint8_t *gles_array_bytes(CPUState *cpu, GLESArray *a, uint32_t first, uint32_t count, uint32_t *stride)
{
    *stride = a->stride;
    return a->enabled ? a->data + (size_t)a->stride * first : NULL;
}
static void *gles_scratch(void *buf, size_t *size, size_t n, const char *who)
{
    void **p = buf;
    if (n > *size) {
        *p = realloc(*p, n);
        *size = n;
    }
    return *p;
}
static void gles_array_enable(GLESArray *a, bool enabled) { a->host_enabled = enabled; }
static void gles_client_active(unsigned unit) {}
#include "slice.h"

/* ---- the fake driver: a modelview and one other matrix, the mode, the last vertex pointer */
static GLfloat mv[16], other[16];
static GLenum mode = GL_MODELVIEW;
static const float *vertices;
static GLfloat *live(void) { return mode == GL_MODELVIEW ? mv : other; }
void glMatrixMode(GLenum m) { mode = m; }
void glLoadMatrixf(const GLfloat *m) { memcpy(live(), m, sizeof(mv)); }
void glLoadIdentity(void)
{
    memset(live(), 0, sizeof(mv));
    for (int i = 0; i < 4; i++) live()[i * 5] = 1;
}
void glGetFloatv(GLenum pname, GLfloat *v) { assert(pname == GL_MODELVIEW_MATRIX); memcpy(v, mv, sizeof(mv)); }
void glGetIntegerv(GLenum pname, GLint *v) { assert(pname == GL_MATRIX_MODE); *v = mode; }
GLboolean glIsEnabled(GLenum cap) { return 0; }
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p) { assert(size == 4); vertices = p; }
void glNormalPointer(GLenum type, GLsizei stride, const void *p) { abort(); }

/* Two bones, one vertex each fully on one of them, as the game's buffers hold them (position, index, weight). */
typedef struct { float pos[3]; uint8_t idx[2], pad[2]; float w[2]; } Vertex;
static const Vertex mesh[2] = { { { 1, 0, 0 }, { 0, 1 }, { 0 }, { 1, 0 } }, { { 0, 1, 0 }, { 1, 0 }, { 0 }, { 1, 0 } } };

static void draw(const float *guest_mv)
{
    float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t bound;

    mode = GL_MODELVIEW;
    glLoadMatrixf(guest_mv);
    mode = GL_TEXTURE;                      /* the guest's mode, which neither call may change */
    bound = gles_skin(NULL, 0, 2);
    assert(bound & 1u << GLES_SKIN_BIT);
    assert(!memcmp(mv, identity, sizeof(mv)) && mode == GL_TEXTURE);   /* drawn under identity */
    assert(vertices[0] == 1 + 10 && vertices[4 + 1] == 1 + 20);          /* each vertex on its bone */
    gles_unbind_arrays(bound);
    assert(!memcmp(mv, guest_mv, sizeof(mv)) && mode == GL_TEXTURE);    /* the guest's again */
}

int main(void)
{
    float body_mv[16] = { 0.3f, 0, 0, 0, 0, 0.3f, 0, 0, 0, 0, 0.3f, 0, 0.4f, 0.1f, -1, 1 };

    gh.vertex = (GLESArray){ .enabled = 1, .size = 3, .type = GL_FLOAT, .data = (const uint8_t *)mesh[0].pos,
                             .stride = sizeof(Vertex) };
    gh.matrix_index = (GLESArray){ .enabled = 1, .size = 2, .type = GL_UNSIGNED_BYTE,
                                   .data = (const uint8_t *)mesh[0].idx, .stride = sizeof(Vertex) };
    gh.weight = (GLESArray){ .enabled = 1, .size = 2, .type = GL_FLOAT, .data = (const uint8_t *)mesh[0].w,
                             .stride = sizeof(Vertex) };
    gh.palette_on = true;
    for (int m = 0; m < 2; m++) {           /* bone m moves x by 10, y by 20 */
        memset(gh.palette[m], 0, sizeof(gh.palette[m]));
        for (int i = 0; i < 4; i++) gh.palette[m][i * 5] = 1;
        gh.palette[m][12 + m] = 10.0f * (m + 1);
    }
    for (int cache = 0; cache < 2; cache++) {
        gles_array_cache = cache;
        draw(body_mv);
        /* The game's next mesh: its palette loaded from the modelview it left as it was. */
        float loaded[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, loaded);
        assert(!memcmp(loaded, body_mv, sizeof(loaded)));
    }
    printf("gles-skin-modelview: the modelview is the guest's after a skinned draw, cache on and off\n");
    return 0;
}
