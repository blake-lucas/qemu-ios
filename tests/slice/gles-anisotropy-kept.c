/* A desktop host without EXT_texture_filter_anisotropic (Mesa 20.3's llvmpipe) refused TEXTURE_MAX_ANISOTROPY with
 * INVALID_ENUM, and the guest's glGetError read it although the front end lists the extension (iOS 10's CoreAnimation
 * sets 8; Zombie Crisis 3D took the error for a failed upload, issue 18). Such a host keeps the value in the bridge per
 * texture, shared by the sharegroup: set and get answer from it, below 1 is INVALID_VALUE, a deleted texture forgets
 * it, and other parameters and targets still reach the host. A host with the extension (or GL 4.6) is untouched.
 *
 * SLICE hw/arm/gles-host.c fn gles_anisotropy_kept_on gles_anisotropy_kept gles_anisotropy_table
 * SLICE hw/arm/gles-host.c fn gles_anisotropy_texture gles_anisotropy_set gles_anisotropy_get gles_anisotropy_forget
 * PKG glib-2.0
 * VARIANT -DHOST_WITHOUT=1
 * VARIANT -DHOST_WITH=1
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef float GLfloat;
typedef unsigned char GLubyte;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_EXTENSIONS 0x1F03
#define GL_VERSION 0x1F02
#define GL_INVALID_VALUE 0x0501
#define GL_TEXTURE_MIN_FILTER 0x2801

typedef struct { GHashTable *anisotropy; } GLESGroup;
static struct { GLESGroup *group; GHashTable *anisotropy; } gh;

#ifdef HOST_WITH
static const char *host_extensions = "GL_ARB_multitexture GL_EXT_texture_filter_anisotropic GL_EXT_framebuffer_object";
#else
static const char *host_extensions = "GL_ARB_multitexture GL_EXT_framebuffer_object";
#endif
static const char *host_version = "3.1 Mesa 20.3.5";
static GLint bound_2d = 7, bound_cube = 9;
static GLenum rejected;

static const GLubyte *glGetString(GLenum name)
{
    return (const GLubyte *)(name == GL_EXTENSIONS ? host_extensions : host_version);
}
static void glGetIntegerv(GLenum pname, GLint *v)
{
    *v = pname == GL_TEXTURE_BINDING_2D ? bound_2d : pname == GL_TEXTURE_BINDING_CUBE_MAP ? bound_cube : 0;
}
static int64_t gles_reject(GLenum error)
{
    rejected = error;
    return 0;
}

#include "slice.h"

int main(void)
{
    GLESGroup group = { 0 };
    GLfloat f = 0;
    GLuint gone = 7;

    gh.group = &group;
#ifdef HOST_WITH
    /* A host with the extension gets every call, as before. */
    assert(!gles_anisotropy_set(GL_TEXTURE_2D, 0x84FE, 8) && !gles_anisotropy_get(GL_TEXTURE_2D, 0x84FE, &f));
    assert(!group.anisotropy);
    puts("PASS: a host with anisotropic filtering is untouched");
    return 0;
#endif
    /* Other parameters and targets are the host's; a get before any set answers 1. */
    assert(!gles_anisotropy_set(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 1));
    assert(!gles_anisotropy_set(0x84F5 /* RECTANGLE */, 0x84FE, 4));
    assert(gles_anisotropy_get(GL_TEXTURE_2D, 0x84FE, &f) && f == 1);

    /* Kept per texture: 2D 7 and cube 9 apart, in the group's table. */
    assert(gles_anisotropy_set(GL_TEXTURE_2D, 0x84FE, 8) && !rejected);
    assert(gles_anisotropy_set(GL_TEXTURE_CUBE_MAP, 0x84FE, 2));
    assert(gles_anisotropy_get(GL_TEXTURE_2D, 0x84FE, &f) && f == 8);
    assert(gles_anisotropy_get(GL_TEXTURE_CUBE_MAP, 0x84FE, &f) && f == 2);
    assert(group.anisotropy && !gh.anisotropy);

    /* Below 1: INVALID_VALUE, as the extension says, and the value stays. */
    assert(gles_anisotropy_set(GL_TEXTURE_2D, 0x84FE, 0.5f) && rejected == GL_INVALID_VALUE);
    assert(gles_anisotropy_get(GL_TEXTURE_2D, 0x84FE, &f) && f == 8);

    /* A deleted texture forgets it: its name made again starts at 1. */
    gles_anisotropy_forget(&gone, 1);
    assert(gles_anisotropy_get(GL_TEXTURE_2D, 0x84FE, &f) && f == 1);
    g_hash_table_destroy(group.anisotropy);
    puts("PASS: a host without anisotropic filtering keeps TEXTURE_MAX_ANISOTROPY in the bridge");
    return 0;
}
