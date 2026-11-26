/* SPDX-License-Identifier: GPL-2.0-or-later */

/* GL names belong to state->cgl. EGL can refuse to acquire it, for example
 * while it remains current on another thread. Deleting against whichever
 * context the caller had current would damage that unrelated context. The
 * native context release below this helper still owns final EGL destruction;
 * its texture cleanup already distinguishes successful context acquisition.
 * Keep the established non-EGL/Mac path unchanged. */
static void gles_context_delete_objects(GLESHost *state)
{
#ifdef GLES_HOST_EGL
    if (CGLSetCurrentContext(state->cgl) != 0) {
        return;
    }
#else
    CGLSetCurrentContext(state->cgl);
#endif
#if !defined(GLES_HOST_EAGL) && !defined(GLES_HOST_ANGLE)
    gles_queries_free(state);
#endif
    glDeleteFramebuffersEXT(1, &state->fbo);
    if (state->fbo_color) glDeleteFramebuffersEXT(1, &state->fbo_color);
    if (state->sync_fbo) glDeleteFramebuffersEXT(1, &state->sync_fbo);
    if (state->copy_fbo) glDeleteFramebuffersEXT(1, &state->copy_fbo);
    /* The A6's renderer scratch (gles_a6): its resolve and fetch objects. */
    if (state->resolve_fbo) glDeleteFramebuffersEXT(1, &state->resolve_fbo);
    if (state->fetch_barrier_fbo) glDeleteFramebuffersEXT(1, &state->fetch_barrier_fbo);
#if !defined(GLES_HOST_EAGL) && !defined(GLES_HOST_ANGLE)
    if (state->fetch_position_buffer) glDeleteBuffers(1, &state->fetch_position_buffer);
#endif
    glDeleteTextures(1, &state->tex);
    if (state->feedback_tex) glDeleteTextures(1, &state->feedback_tex);
    glDeleteRenderbuffersEXT(1, &state->depth);
#ifdef GLES_HOST_EGL_ES
    if (state->stencil && state->stencil != state->depth)
        glDeleteRenderbuffersEXT(1, &state->stencil);
#endif
    CGLSetCurrentContext(NULL);
}
