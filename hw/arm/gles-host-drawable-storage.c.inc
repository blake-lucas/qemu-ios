/* SPDX-License-Identifier: GPL-2.0-or-later */

static int64_t gles_drawable_storage(uint32_t width, uint32_t height)
{
    GLint texture, renderbuffer, framebuffer;
    GLuint color = 0, depth = 0, stencil = 0, fbo = 0;
    GLenum error, status;
    /* Desktop snapshots predate this field; preserve their packed recipe. */
    GLenum depth_format = GL_DEPTH24_STENCIL8_EXT;
#ifdef GLES_HOST_EGL_ES
    depth_format = gh.drawable_depth_format;
#endif
    uint8_t *readback;
    bool mapped = false;
#ifdef GLES_HOST_AHB
    GLESAhb ahb = { .image = EGL_NO_IMAGE_KHR };
#endif
    uint64_t charged;

    if (!width || !height || width > 2048 || height > 2048) {
        gles_refuse("drawable:size:%ux%u", width, height);
        return gles_reject(GL_INVALID_VALUE);
    }
    gh.drawable_announced = true;
    if (width == gh.drawable_width && height == gh.drawable_height) return 0;
    if (!gles_charge_drawable(width, height, &charged)) {
        return gles_reject(GL_OUT_OF_MEMORY);
    }
    readback = g_try_malloc0_n((size_t)width * height, 4);
    if (!readback) {
        gles_charge(gles_charge_table(true), gles_charge_key(GLES_CHARGE_DRAWABLE, 0, 0, 0), charged, NULL);
        gles_refuse("drawable:memory");
        return gles_reject(GL_OUT_OF_MEMORY);
    }

    gles_texture_begin();
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &renderbuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
    color = gles_private_name(glIsTexture);
    glBindTexture(GL_TEXTURE_2D, color);
#ifdef GLES_HOST_AHB
    /* CPU-mapped storage, so the present can skip glReadPixels (see gh.ahb). */
    mapped = !gles_no_iosurface && gles_ahb_attach(&ahb, width, height);
#endif
    if (!mapped) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gles_drawable_depth_allocate(depth_format, width, height,
                                &depth, &stencil);
    fbo = gles_private_name(glIsFramebufferEXT);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, color, 0);
    gles_drawable_depth_attach(depth, stencil);
    status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
#ifdef GLES_HOST_AHB
    if (mapped && status != GL_FRAMEBUFFER_COMPLETE_EXT) {
        /* Accepted but not renderable: ordinary storage, read back as before. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gles_ahb_free(&ahb);
        mapped = false;
        status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    }
#endif
    error = glGetError();
    if (error == GL_NO_ERROR && status != GL_FRAMEBUFFER_COMPLETE_EXT)
        error = GL_OUT_OF_MEMORY;
    if (error != GL_NO_ERROR) gles_refuse("drawable:storage:0x%x", error);
    if (error == GL_NO_ERROR) {
        if ((GLuint)texture == gh.tex) texture = color;
        renderbuffer = gles_drawable_depth_remap(renderbuffer, gh.depth, gh.stencil,
                                                 depth, stencil);
        if ((GLuint)framebuffer == gh.fbo) framebuffer = fbo;
        glDeleteTextures(1, &gh.tex);
        gles_drawable_depth_delete(gh.depth, gh.stencil);
        glDeleteFramebuffersEXT(1, &gh.fbo);
        g_free(gh.readback);
        gh.tex = color; gh.depth = depth; gh.stencil = stencil; gh.fbo = fbo;
        gh.readback = readback;
        gh.readback_size = (size_t)width * height * 4;
        gh.drawable_width = width; gh.drawable_height = height;
        /* The portable readback path handles resized layers on both hosts.
         * The platform's original IOSurface remains owned by its context;
         * Android's AHardwareBuffer is per drawable, so it follows the resize. */
        gh.iosurface = mapped;
#ifdef GLES_HOST_AHB
        gles_ahb_free(&gh.ahb);
        gh.ahb = ahb;
#endif
        gh.fb_dirty = true;
        gh.depth_cleared_this_frame = false;
    } else {
#ifdef GLES_HOST_AHB
        gles_ahb_free(&ahb);
#endif
        glDeleteTextures(1, &color);
        gles_drawable_depth_delete(depth, stencil);
        glDeleteFramebuffersEXT(1, &fbo);
        g_free(readback);
        gles_charge(gles_charge_table(true), gles_charge_key(GLES_CHARGE_DRAWABLE, 0, 0, 0), charged, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, renderbuffer);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    return error == GL_NO_ERROR ? 0 : gles_reject(error);
}
