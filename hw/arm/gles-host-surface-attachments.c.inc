/* SPDX-License-Identifier: GPL-2.0-or-later */

/* An attachment replacement ends writes to the OLD surface just as a framebuffer
 * bind does. Mark it before changing the attachment, but leave writeback at the
 * existing frame boundary: publishing here would expose an unfinished CA pass. */
    case GLES_SLOT_FB_RENDERBUFFER: {    /* target, attach, rbtarget, rb */
        uint32_t attach = a[1], rb = a[3];

        if (!gles_guest_name_ok(rb)) {
            gles_refuse("name:private:%u", slot);
            return gles_reject(GL_INVALID_OPERATION);
        }
        if (attach == GL_COLOR_ATTACHMENT0_EXT) {
            if (gles_sync_surface(cpu)) return -1;
            /* A color renderbuffer that never got GL storage is the CA
             * drawable, so from now on this framebuffer MEANS gh.fbo. Any
             * other color attachment takes that meaning away again. */
            gles_set_drawable(gh.bound_framebuffer,
                              rb && !g_hash_table_contains(
                                  gh.rb_sized, GUINT_TO_POINTER(rb)));
        }
        gles_trace_attach("glFramebufferRenderbuffer", attach, rb);
        gh.fb_dirty = true;
        if (gles_is_drawable(gh.bound_framebuffer)) {
            /* gh.fbo already carries the color target we present out of and a
             * matching depth buffer; re-attaching over either breaks the
             * present path. */
            return 0;
        }
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, attach,
                                     GL_RENDERBUFFER_EXT, rb);
        return 0;
    }

    case GLES_SLOT_FB_TEXTURE_2D:        /* target, attach, textarget, tex, lvl */
        /* Attaching a texture as color makes this a real offscreen target,
         * even if the very same framebuffer was the drawable a moment ago.
         * Engines reuse ONE framebuffer object and swap its attachments --
         * save the binding, attach a texture, render, attach the drawable
         * renderbuffer back -- and Labyrinth does exactly that. Treating
         * "drawable" as a permanent property of the NAME rather than of the
         * current color attachment sent its render-to-texture pass to the
         * screen and left the texture empty, which composited as a white
         * level while the menus (UIKit) looked perfect. */
        if (!gles_guest_name_ok(a[3])) {
            gles_refuse("name:private:%u", slot);
            return gles_reject(GL_INVALID_OPERATION);
        }
        if (a[1] == GL_COLOR_ATTACHMENT0_EXT) {
            if (gles_sync_surface(cpu)) return -1;
            gles_set_drawable(gh.bound_framebuffer, false);
        }
        gles_trace_attach("glFramebufferTexture2D", a[1], a[3]);
        gh.fb_dirty = true;
        if (gles_is_drawable(gh.bound_framebuffer)) {
            return 0;
        }
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, a[1], a[2], a[3], a[4]);
        return 0;

