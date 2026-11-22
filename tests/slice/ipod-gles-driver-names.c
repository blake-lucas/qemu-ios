/* A driver that numbers new framebuffers and renderbuffers past the highest name in use hands the guest names
 * in the bridge's private range once the drawable's FBO sits at GLES_PRIVATE_NAME (Mali, issue 6: every bind
 * was refused and the screen stayed on the boot logo). gles_guest_gen_low gives such names back and replaces
 * them from the context's own count from 1, skipping names in use; a driver's ordinary names pass untouched.
 *
 * SLICE hw/arm/gles-host.c define GLES_PRIVATE_NAME
 * SLICE hw/arm/gles-host.c fn gles_guest_name_ok gles_guest_gen_low
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int GLuint;
typedef unsigned char GLboolean;
typedef int GLsizei;

static struct { GLuint fb_next, rb_next; } host, *gh_current = &host;
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

#include "slice.h"

int main(void)
{
    /* Ordinary driver names are left alone and nothing is deleted. */
    GLuint plain[2] = { 1, 2 };
    gles_guest_gen_low(plain, 2, false);
    assert(plain[0] == 1 && plain[1] == 2 && n_deleted_fb == 0 && gh.fb_next == 0);

    /* Past the private name: replaced from 1, skipping names in use (2 is bound already), driver's given back. */
    in_use_fb[0] = 2;
    GLuint high[3] = { GLES_PRIVATE_NAME + 1, GLES_PRIVATE_NAME + 2, GLES_PRIVATE_NAME + 3 };
    gles_guest_gen_low(high, 3, false);
    assert(high[0] == 1 && high[1] == 3 && high[2] == 4);
    assert(n_deleted_fb == 3 && deleted_fb[0] == GLES_PRIVATE_NAME + 1 && deleted_fb[2] == GLES_PRIVATE_NAME + 3);

    /* Renderbuffers keep their own count (1 is in use, so the first is 2). */
    GLuint rb[1] = { GLES_PRIVATE_NAME + 9 };
    gles_guest_gen_low(rb, 1, true);
    assert(rb[0] == 2 && n_deleted_rb == 1 && deleted_rb[0] == GLES_PRIVATE_NAME + 9);

    /* A new context's count starts again at 1, as on the device. */
    static struct { GLuint fb_next, rb_next; } other;
    gh_current = (void *)&other;
    in_use_fb[0] = 0;
    GLuint fresh[1] = { GLES_PRIVATE_NAME + 4 };
    gles_guest_gen_low(fresh, 1, false);
    assert(fresh[0] == 1);

    /* The count wraps below the private range instead of reaching it. */
    other.fb_next = GLES_PRIVATE_NAME - 1;
    GLuint wrap[1] = { GLES_PRIVATE_NAME + 5 };
    gles_guest_gen_low(wrap, 1, false);
    assert(wrap[0] == 1);
    puts("PASS: driver names in the private range are replaced by low ones");
    return 0;
}
