/* SPDX-License-Identifier: GPL-2.0-or-later
 * Offscreen EGL lifetime adapter. Guest GL remains in gles-host.c. This leaf
 * deliberately uses only libc/EGL so the production context can be exercised
 * independently of QEMU, including on Android hardware.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(__ANDROID__) && defined(GLES_HOST_EGL_ES)
#include "gles-host-driver.c.inc"
#endif

typedef struct GLESEGLTexture {
    unsigned name, native_name, width, height, target, refs;
    bool deleted, hidden;
    /* EXT_texture_filter_anisotropic's value where the host has no such state (gles_native_anisotropy_kept); 0: 1. */
    GLfloat max_anisotropy;
    struct GLESEGLTexture *next;
} GLESEGLTexture;
typedef struct GLESEGLTextures {
    unsigned refs;
    GLESEGLTexture *head;
    /* Recent lookups by name & 255, checked on use; emptied when any is freed. */
    GLESEGLTexture *recent[256];
    unsigned next_name;
    struct GLESEGLContext *contexts;
} GLESEGLTextures;

typedef struct GLESEGLTextureUnit {
    unsigned texture_2d, rectangle;
    GLESEGLTexture *object_2d, *object_rectangle, *object_cube;
    bool enabled_2d, enabled_rectangle;
    /* The unit's native 2D binding holds its rectangle texture, not object_2d. */
    bool host_rectangle;
    /* EXT_texture_lod_bias's value where the host has no such state (gles_native_lod_bias_kept). */
    GLfloat lod_bias;
} GLESEGLTextureUnit;

typedef struct GLESEGLContext {
    EGLContext context;
    EGLSurface surface;
    unsigned refs, api;
    unsigned unpack_alignment, unpack_row_length, errors, fixed_texture_units;
    bool fbo_checked, fbo_supported;
    GLESEGLTextures *textures;
    GLESEGLTexture default_texture, default_cube;
    GLESEGLTexture *default_rectangle;
    GLESEGLTextureUnit texture_units[32];
    struct GLESEGLContext *next_context;
    void (*texture_cleanup)(struct GLESEGLContext *, bool);
    void *texture_attachments;
    unsigned rectangle_zero;
    bool drawing_window_quad;
    bool release_pending; /* final owner released, but still current here */
} *CGLContextObj;
static EGLDisplay gles_egl_display = EGL_NO_DISPLAY;
static EGLConfig gles_egl_config;
static unsigned gles_egl_rectangle_zero=0x60000000u;
static __thread CGLContextObj gles_egl_current;

static void CGLReleaseContext(CGLContextObj c);

/*
 * Only this function binds a context on the host (EGL bindings are per thread), so gles_egl_current is the
 * thread's binding. Asking EGL as well cost ~0.8 us per guest GL request on the Poco's Adreno driver (2.7% of
 * Jetpack's vCPU); LT_EGL_CURRENT_CHECK=on asks again.
 */
static bool gles_egl_current_check(void)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("LT_EGL_CURRENT_CHECK");
        on = v && (!strcmp(v, "on") || !strcmp(v, "1"));
    }
    return on;
}

static int CGLSetCurrentContext(CGLContextObj c)
{
    if (gles_egl_current == c && (!gles_egl_current_check() || eglGetCurrentContext() ==
        (c ? c->context : EGL_NO_CONTEXT))) return 0;
    if (!eglMakeCurrent(gles_egl_display, c ? c->surface : EGL_NO_SURFACE,
                        c ? c->surface : EGL_NO_SURFACE,
                        c ? c->context : EGL_NO_CONTEXT)) {
        fprintf(stderr, "[gles-egl] make-current failed: 0x%x\n", eglGetError());
        return -1;
    }
    CGLContextObj previous = gles_egl_current;
    gles_egl_current = c;
    /* A failed final-release unbind retains one safety reference. Reclaim it
     * only after EGL actually replaces the binding. This bounds retention to
     * the calling thread's still-current wrapper, rather than one orphan per
     * failed release. Do not rebind the retired context to clean it again. */
    if (previous && previous != c && previous->release_pending) {
        previous->release_pending = false;
        CGLReleaseContext(previous);
    }
    return 0;
}
static CGLContextObj CGLRetainContext(CGLContextObj c) { c->refs++; return c; }
static void CGLReleaseContext(CGLContextObj c)
{
    if (!c || --c->refs) return;
    c->release_pending = false;
    CGLContextObj previous=gles_egl_current;
    /* Acquiring c retires a deferred previous binding. Never restore that
     * final-released wrapper after the successful switch has freed it. */
    if (previous && previous->release_pending) previous = NULL;
    if (c->texture_cleanup) {
        bool current=CGLSetCurrentContext(c)==0;
        void (*cleanup)(struct GLESEGLContext *, bool) = c->texture_cleanup;
        c->texture_cleanup = NULL; /* a retained release must not clean twice */
        cleanup(c,current);
    }
    if (gles_egl_current==c &&
        CGLSetCurrentContext(previous==c ? NULL : previous) != 0) {
        /* EGL and TLS still name c. Keep its wrapper and texture-list ref
         * alive; a later successful switch/unbind releases this safety ref,
         * or an explicit final-release retry can complete it. */
        c->refs = 1;
        c->release_pending = true;
        return;
    }
    struct GLESEGLContext **slot=&c->textures->contexts;
    while (*slot && *slot!=c) slot=&(*slot)->next_context;
    if (*slot) *slot=c->next_context;
    eglDestroyContext(gles_egl_display, c->context);
    eglDestroySurface(gles_egl_display, c->surface);
    if (!--c->textures->refs) {
        GLESEGLTexture *texture=c->textures->head;
        while (texture) { GLESEGLTexture *next=texture->next; free(texture); texture=next; }
        free(c->textures);
    }
    free(c);
}
#ifndef __ANDROID__
/* The desktop's surfaceless display is an optimization, not a requirement.
 * Publish only a fully initialized/configured candidate. No contexts exist on
 * a failed candidate, so terminating it cannot invalidate a shared context. */
static void gles_egl_discard_display(EGLDisplay display, const char *source)
{
    (void)eglGetError();
    if (!eglTerminate(display)) {
        fprintf(stderr, "[gles-egl] display=%s terminate failed: 0x%x\n",
                source, eglGetError());
    }
}

static bool gles_egl_configure_display(EGLDisplay display, const char *source,
                                       EGLint renderable)
{
    if (display == EGL_NO_DISPLAY) {
        fprintf(stderr, "[gles-egl] display=%s acquire failed: 0x%x\n",
                source, eglGetError());
        return false;
    }
    EGLint major = 0, minor = 0;
    (void)eglGetError();
    if (!eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "[gles-egl] display=%s initialize failed: 0x%x\n",
                source, eglGetError());
        gles_egl_discard_display(display, source);
        return false;
    }
    /* Diagnostic queries are not guest GL calls and must not leave an EGL
     * query error to be mistaken for a later failed selection operation. */
    const char *vendor = eglQueryString(display, EGL_VENDOR);
    const char *version = eglQueryString(display, EGL_VERSION);
    const char *apis = eglQueryString(display, EGL_CLIENT_APIS);
    fprintf(stderr, "[gles-egl] display=%s EGL=%d.%d vendor=%s version=%s apis=%s\n",
            source, major, minor, vendor ? vendor : "unavailable",
            version ? version : "unavailable", apis ? apis : "unavailable");
    EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = NULL;
    EGLint count = 0;
    (void)eglGetError();
    if (!eglChooseConfig(display, attributes, &config, 1, &count)) {
        fprintf(stderr, "[gles-egl] display=%s choose-config failed: 0x%x renderable=0x%x\n",
                source, eglGetError(), renderable);
        gles_egl_discard_display(display, source);
        return false;
    }
    if (!count) {
        fprintf(stderr, "[gles-egl] display=%s choose-config returned zero matches "
                "(success) renderable=0x%x pbuffer=1 rgba=8/8/8/8\n", source, renderable);
        gles_egl_discard_display(display, source);
        return false;
    }
    gles_egl_config = config;
    gles_egl_display = display;
    fprintf(stderr, "[gles-egl] selected display=%s renderable=0x%x\n", source, renderable);
    return true;
}

static bool gles_egl_desktop_display(EGLint renderable)
{
    (void)eglGetError();
    PFNEGLGETPLATFORMDISPLAYEXTPROC get = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get) {
        (void)eglGetError();
        EGLDisplay display = get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (gles_egl_configure_display(display, "surfaceless-mesa", renderable)) return true;
        fprintf(stderr, "[gles-egl] surfaceless candidate failed; trying default display\n");
    } else {
        fprintf(stderr, "[gles-egl] surfaceless entry point unavailable: 0x%x; trying default display\n",
                eglGetError());
    }
    (void)eglGetError();
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return gles_egl_configure_display(display, "default", renderable);
}
#endif

static bool gles_egl_create(CGLContextObj share, unsigned api, CGLContextObj *out)
{
#if defined(__ANDROID__) && defined(GLES_HOST_EGL_ES)
    if (!gles_driver_prepare()) return false;
#endif
#ifdef GLES_HOST_EGL_ES
    EGLenum client = EGL_OPENGL_ES_API;
    EGLint renderable = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
#else
    EGLenum client = EGL_OPENGL_API;
    EGLint renderable = EGL_OPENGL_BIT;
#endif
#ifndef __ANDROID__
    /* Bind the intended desktop client before config selection or pbuffer
     * creation. EGL's initial per-thread client is OpenGL ES, not OpenGL. */
    (void)eglGetError();
    if (!eglBindAPI(client)) {
        fprintf(stderr, "[gles-egl] bind-api=0x%x failed: 0x%x\n", client, eglGetError());
        return false;
    }
    if (gles_egl_display == EGL_NO_DISPLAY && !gles_egl_desktop_display(renderable)) return false;
#else
    /* Keep Android's selected provider and original API/config ordering. In
     * particular, never call unremapped desktop query/termination entry points
     * on an ANGLE display or fall back to a different Android provider. */
    if (gles_egl_display == EGL_NO_DISPLAY) {
#ifdef GLES_HOST_EGL_ES
        gles_egl_display = gles_driver_display();
#else
        gles_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
        EGLint config[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, renderable, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count = 0;
        if (gles_egl_display == EGL_NO_DISPLAY ||
            !eglInitialize(gles_egl_display, NULL, NULL) ||
            !eglChooseConfig(gles_egl_display, config, &gles_egl_config, 1, &count) || !count) {
            fprintf(stderr, "[gles-egl] initialize/config failed: 0x%x\n", eglGetError());
            gles_egl_display = EGL_NO_DISPLAY;
            return false;
        }
    }
    if (!eglBindAPI(client)) return false;
#endif
#ifdef GLES_HOST_EGL_ES
    EGLint context[] = {EGL_CONTEXT_CLIENT_VERSION, api == 2 ? 2 : 1, EGL_NONE};
#else
    EGLint context[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
#endif
    EGLint surface[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    CGLContextObj c = calloc(1, sizeof(*c));
    if (!c) return false;
    c->refs = 1;
    c->rectangle_zero=++gles_egl_rectangle_zero;
    c->unpack_alignment = 4;
    c->textures=share ? share->textures : calloc(1,sizeof(*c->textures));
    if (!c->textures) { free(c); return false; }
    c->textures->refs++;
    c->api = api == 2 ? 2 : 1;
#ifndef __ANDROID__
    (void)eglGetError();
    c->context = eglCreateContext(gles_egl_display, gles_egl_config,
                                 share ? share->context : EGL_NO_CONTEXT, context);
    if (c->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "[gles-egl] create-context api=%u client=0x%x failed: 0x%x\n",
                c->api, client, eglGetError());
        goto create_failed;
    }
    (void)eglGetError();
    c->surface = eglCreatePbufferSurface(gles_egl_display, gles_egl_config, surface);
    if (c->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[gles-egl] create-pbuffer 1x1 failed: 0x%x\n", eglGetError());
        goto create_failed;
    }
    (void)eglGetError();
    if (CGLSetCurrentContext(c)) goto create_failed;
#else
    c->context = eglCreateContext(gles_egl_display, gles_egl_config,
                                 share ? share->context : EGL_NO_CONTEXT, context);
    c->surface = eglCreatePbufferSurface(gles_egl_display, gles_egl_config, surface);
    if (c->context == EGL_NO_CONTEXT || c->surface == EGL_NO_SURFACE || CGLSetCurrentContext(c)) {
        fprintf(stderr, "[gles-egl] create api=%u failed: 0x%x\n", c->api, eglGetError());
        goto create_failed;
    }
#endif
    c->next_context=c->textures->contexts;
    c->textures->contexts=c;
    *out = c;
    return true;
create_failed:
    if (c->context != EGL_NO_CONTEXT) eglDestroyContext(gles_egl_display, c->context);
    if (c->surface != EGL_NO_SURFACE) eglDestroySurface(gles_egl_display, c->surface);
    if (!--c->textures->refs) free(c->textures);
    free(c);
    return false;
}
