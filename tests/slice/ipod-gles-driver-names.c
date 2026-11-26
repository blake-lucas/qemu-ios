/* A driver that numbers new framebuffers, renderbuffers or textures past the highest name in use hands the guest
 * names in the bridge's private range once the drawable's FBO and texture sit at GLES_PRIVATE_NAME (Mali, issue 6;
 * Mesa 20.3 for textures too: every bind was refused and the screen stayed on the boot logo). gles_guest_gen_low
 * gives such names back and replaces them from a count from 1, skipping names in use; a driver's ordinary names
 * pass untouched. Framebuffers count per context; renderbuffers and textures, shared by the sharegroup, per group
 * (LT_GLES_SHARED_LOW_NAMES=off: per context, as before).
 *
 * SLICE hw/arm/gles-host.c define GLES_PRIVATE_NAME
 * SLICE hw/arm/gles-host.c typedef GLESGenKind
 * SLICE hw/arm/gles-host.c fn gles_guest_name_ok gles_shared_low_names_on gles_gen_low_next gles_guest_gen_low
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int GLuint;
typedef unsigned char GLboolean;
typedef int GLsizei;

typedef struct { GLuint rb_next, tex_next; } GLESGroup;
typedef struct { GLESGroup *group; GLuint fb_next, rb_next, tex_next; } GLESHost;
static GLESHost host, *gh_current = &host;
#define gh (*gh_current)

static GLuint in_use_fb[8], deleted_fb[8], deleted_rb[8];
static unsigned n_deleted_fb, n_deleted_rb;
static GLboolean glIsFramebufferEXT(GLuint name)
{
    for (unsigned i = 0; i < 8; i++) if (in_use_fb[i] == name) return 1;
    return 0;
}
static GLboolean glIsRenderbufferEXT(GLuint name) { return name == 1; }
static void glDeleteFramebuffersEXT(GLsizei n, const GLuint *ids) { deleted_fb[n_deleted_fb++] = ids[0]; (void)n; }
static void glDeleteRenderbuffersEXT(GLsizei n, const GLuint *ids) { deleted_rb[n_deleted_rb++] = ids[0]; (void)n; }
static GLuint in_use_tex[8], deleted_tex[8];
static unsigned n_deleted_tex;
static GLboolean glIsTexture(GLuint name)
{
    for (unsigned i = 0; i < 8; i++) if (in_use_tex[i] == name) return 1;
    return 0;
}
static void glDeleteTextures(GLsizei n, const GLuint *ids) { deleted_tex[n_deleted_tex++] = ids[0]; (void)n; }

#include "slice.h"

int main(void)
{
    /* Ordinary driver names are left alone and nothing is deleted. */
    GLuint plain[2] = { 1, 2 };
    gles_guest_gen_low(plain, 2, GLES_GEN_FRAMEBUFFER);
    assert(plain[0] == 1 && plain[1] == 2 && n_deleted_fb == 0 && gh.fb_next == 0);

    /* Past the private name: replaced from 1, skipping names in use (2 is bound already), driver's given back. */
    in_use_fb[0] = 2;
    GLuint high[3] = { GLES_PRIVATE_NAME + 1, GLES_PRIVATE_NAME + 2, GLES_PRIVATE_NAME + 3 };
    gles_guest_gen_low(high, 3, GLES_GEN_FRAMEBUFFER);
    assert(high[0] == 1 && high[1] == 3 && high[2] == 4);
    assert(n_deleted_fb == 3 && deleted_fb[0] == GLES_PRIVATE_NAME + 1 && deleted_fb[2] == GLES_PRIVATE_NAME + 3);

    /* Renderbuffers keep their own count (1 is in use, so the first is 2). */
    GLuint rb[1] = { GLES_PRIVATE_NAME + 9 };
    gles_guest_gen_low(rb, 1, GLES_GEN_RENDERBUFFER);
    assert(rb[0] == 2 && n_deleted_rb == 1 && deleted_rb[0] == GLES_PRIVATE_NAME + 9);

    /* A new context's count starts again at 1, as on the device. */
    static GLESHost other;
    gh_current = &other;
    in_use_fb[0] = 0;
    GLuint fresh[1] = { GLES_PRIVATE_NAME + 4 };
    gles_guest_gen_low(fresh, 1, GLES_GEN_FRAMEBUFFER);
    assert(fresh[0] == 1);

    /* The count wraps below the private range instead of reaching it. */
    other.fb_next = GLES_PRIVATE_NAME - 1;
    GLuint wrap[1] = { GLES_PRIVATE_NAME + 5 };
    gles_guest_gen_low(wrap, 1, GLES_GEN_FRAMEBUFFER);
    assert(wrap[0] == 1);

    /* Textures (Mesa 20.3 numbers them past the private color texture): replaced from 1, skipping bound ones. */
    gh_current = &host;
    in_use_tex[0] = 1;
    GLuint tex[2] = { GLES_PRIVATE_NAME + 1, GLES_PRIVATE_NAME + 2 };
    gles_guest_gen_low(tex, 2, GLES_GEN_TEXTURE);
    assert(tex[0] == 2 && tex[1] == 3 && n_deleted_tex == 2 && deleted_tex[1] == GLES_PRIVATE_NAME + 2);

    /* Two contexts of one sharegroup draw from the group's count: the second context's texture is not the first's,
     * though neither is bound yet (glIsTexture answers false for both). Framebuffers stay per context. */
    static GLESGroup group;
    static GLESHost a = { &group }, b = { &group };
    in_use_tex[0] = 0;
    GLuint ta[1] = { GLES_PRIVATE_NAME + 3 }, tb[1] = { GLES_PRIVATE_NAME + 4 };
    gh_current = &a; gles_guest_gen_low(ta, 1, GLES_GEN_TEXTURE);
    gh_current = &b; gles_guest_gen_low(tb, 1, GLES_GEN_TEXTURE);
    assert(ta[0] == 1 && tb[0] == 2 && group.tex_next == 2 && a.tex_next == 0);
    GLuint ra[1] = { GLES_PRIVATE_NAME + 5 }, rb2[1] = { GLES_PRIVATE_NAME + 6 };
    gh_current = &a; gles_guest_gen_low(ra, 1, GLES_GEN_RENDERBUFFER);
    gh_current = &b; gles_guest_gen_low(rb2, 1, GLES_GEN_RENDERBUFFER);
    assert(ra[0] == 2 && rb2[0] == 3);         /* 1 is in use (glIsRenderbufferEXT) */
    GLuint fa[1] = { GLES_PRIVATE_NAME + 7 }, fb[1] = { GLES_PRIVATE_NAME + 8 };
    gh_current = &a; gles_guest_gen_low(fa, 1, GLES_GEN_FRAMEBUFFER);
    gh_current = &b; gles_guest_gen_low(fb, 1, GLES_GEN_FRAMEBUFFER);
    assert(fa[0] == 1 && fb[0] == 1);
    puts("PASS: driver names in the private range are replaced by low ones");
    return 0;
}
