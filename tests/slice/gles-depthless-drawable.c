/* A drawable framebuffer the guest gave no depth or stencil attachment draws into gh.fbo_color (the drawable's color
 * alone), so its depth and stencil tests pass as on the device; one with either attachment, framebuffer 0, and one
 * with no record keep gh.fbo and its private depth/stencil. An attachment change on the bound drawable moves the host
 * binding along.
 *
 * Toy Shot (GitHub issue 21) attaches only a color renderbuffer and leaves GL_DEPTH_TEST on under cocos2d's 3D
 * projection. Drawn into gh.fbo, a translucent backdrop quad nearer the camera hid every block and toy behind it.
 * Field & Stream Fishing attaches color and then depth, and must end on gh.fbo.
 *
 * SLICE hw/arm/gles-host.c range /* A framebuffer's attachments as the guest made them | static bool gles_is_drawable(
 * SLICE hw/arm/gles-host.c fn gles_is_drawable gles_drawable_depthless gles_host_fbo gles_note_attach
 * SLICE hw/arm/gles-host.c fn gles_note_attach_rebind
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned int GLuint;
#define GL_FRAMEBUFFER_EXT 0x8D40
#define GL_COLOR_ATTACHMENT0_EXT 0x8CE0
#define GL_DEPTH_ATTACHMENT_EXT 0x8D00
#define GL_STENCIL_ATTACHMENT_EXT 0x8D20
#define GL_RENDERBUFFER_EXT 0x8D41
static struct {
    GHashTable *fbo_drawable, *fb_attach;
    uint32_t bound_framebuffer, read_framebuffer;
    GLuint fbo, fbo_color;
} gh;
static GLuint host_binding;
static unsigned binds;
static void glBindFramebufferEXT(unsigned target, GLuint fb)
{
    assert(target == GL_FRAMEBUFFER_EXT);
    host_binding = fb;
    binds++;
}
#include "slice.h"

enum { FBO = 0x40000000, FBO_COLOR = 0x40000001, SCREEN = 1, OFFSCREEN = 2 };

/* The guest's glFramebufferRenderbuffer on the bound framebuffer, as the handler records it. */
static void attach(uint32_t point, uint32_t rb)
{
    gles_note_attach_rebind(point, GL_RENDERBUFFER_EXT, rb, 0, 0);
}

int main(void)
{
    gh.fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    gh.fbo = FBO;
    gh.fbo_color = FBO_COLOR;
    g_hash_table_insert(gh.fbo_drawable, GUINT_TO_POINTER(SCREEN), GUINT_TO_POINTER(1));

    /* No record yet, and framebuffer 0: the private depth/stencil, as before. */
    assert(gles_host_fbo(SCREEN) == FBO && gles_host_fbo(0) == FBO);
    /* An offscreen framebuffer is its own host name, whatever it has attached. */
    assert(gles_host_fbo(OFFSCREEN) == OFFSCREEN);

    /* Toy Shot: color only. */
    gh.bound_framebuffer = gh.read_framebuffer = SCREEN;
    host_binding = FBO;
    attach(GL_COLOR_ATTACHMENT0_EXT, 1);
    assert(gles_host_fbo(SCREEN) == FBO_COLOR && host_binding == FBO_COLOR);

    /* Fishing: then depth, back to gh.fbo; and a detach of it, color-only again. */
    attach(GL_DEPTH_ATTACHMENT_EXT, 2);
    assert(gles_host_fbo(SCREEN) == FBO && host_binding == FBO);
    attach(GL_DEPTH_ATTACHMENT_EXT, 0);
    assert(gles_host_fbo(SCREEN) == FBO_COLOR && host_binding == FBO_COLOR);

    /* Stencil alone needs the private storage too. */
    attach(GL_STENCIL_ATTACHMENT_EXT, 3);
    assert(gles_host_fbo(SCREEN) == FBO && host_binding == FBO);

    /* An attachment that changes nothing rebinds nothing. */
    binds = 0;
    attach(GL_DEPTH_ATTACHMENT_EXT, 2);
    assert(binds == 0 && host_binding == FBO);

    /* An offscreen framebuffer's attachments never move the binding to a drawable's. */
    gh.bound_framebuffer = gh.read_framebuffer = OFFSCREEN;
    host_binding = OFFSCREEN;
    attach(GL_COLOR_ATTACHMENT0_EXT, 4);
    assert(host_binding == OFFSCREEN && gles_host_fbo(OFFSCREEN) == OFFSCREEN);

    /* Switched off (LT_GLES_DEPTHLESS_DRAWABLE=off) or refused by the host: no gh.fbo_color, gh.fbo for all. */
    gh.fbo_color = 0;
    gh.bound_framebuffer = gh.read_framebuffer = SCREEN;
    host_binding = gles_host_fbo(SCREEN);       /* the guest's glBindFramebuffer */
    attach(GL_STENCIL_ATTACHMENT_EXT, 0);
    attach(GL_DEPTH_ATTACHMENT_EXT, 0);
    assert(gles_host_fbo(SCREEN) == FBO && host_binding == FBO);

    puts("PASS: a color-only drawable draws without depth/stencil; one with either, fb 0 and unrecorded keep them");
    return 0;
}
