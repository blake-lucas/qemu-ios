/* SPDX-License-Identifier: GPL-2.0-or-later
 * ES1 rectangle textures retain their own bindings/enables and immutable
 * object targets. Native storage is 2D; D^-1 times the guest texture matrix
 * converts pixel coordinates after the guest transform, including q division.
 */
#ifndef GL_TEXTURE_RECTANGLE_ARB
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#define GL_TEXTURE_BINDING_RECTANGLE_ARB 0x84F6
#define GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB 0x84F8
#endif
static GLenum gles_native_target(GLenum target)
{ return target == GL_TEXTURE_RECTANGLE_ARB && gles_egl_current->api==1 ? GL_TEXTURE_2D : target; }
/* Some GLES1 translators report the backing GL limit rather than their own
 * fixed-function limit. Verify usable units once, retaining prior error flags
 * and the caller's active unit. Never iterate unsupported units during a draw. */
static GLint gles_native_fixed_units(void)
{
    if (gles_egl_current->fixed_texture_units) return gles_egl_current->fixed_texture_units;
    GLint reported=0,active=GL_TEXTURE0;
    gles_native_save_errors();
    glGetIntegerv(GL_MAX_TEXTURE_UNITS,&reported);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    GLint limit=reported>8 ? 8 : reported, supported=0;
    for (;supported<limit;supported++) {
        glActiveTexture(GL_TEXTURE0+supported);
        GLenum error=glGetError();
        if (!error) continue;
        if (error!=GL_INVALID_ENUM) gles_native_error(error);
        break;
    }
    glActiveTexture(active);
    gles_egl_current->fixed_texture_units=supported;
    if (supported!=reported)
        fprintf(stderr,"[gles-egl] GLES1 fixed texture units: reported=%d usable=%d\n",reported,supported);
    return supported;
}
static int gles_native_unit(void)
{
    GLint active;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    return active>=GL_TEXTURE0 && active<GL_TEXTURE0+32 ? active-GL_TEXTURE0 : -1;
}
static GLESEGLTexture *gles_native_bound_texture(GLenum target)
{
    int unit=gles_native_unit();
    if (unit<0) return NULL;
    GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
    if (target==GL_TEXTURE_2D) return state->object_2d ? state->object_2d : &gles_egl_current->default_texture;
    if (target==GL_TEXTURE_CUBE_MAP) return state->object_cube ? state->object_cube : &gles_egl_current->default_cube;
    if (target==GL_TEXTURE_RECTANGLE_ARB) return state->object_rectangle ? state->object_rectangle : gles_egl_current->default_rectangle;
    return NULL;
}
static GLESEGLTexture *gles_native_default_rectangle(void)
{
    if (!gles_egl_current->default_rectangle) {
        GLESEGLTexture *texture=calloc(1,sizeof(*texture));
        if (!texture) { gles_native_error(GL_OUT_OF_MEMORY); return NULL; }
        glGenTextures(1,&texture->native_name);
        if (!texture->native_name) { free(texture); gles_native_error(GL_OUT_OF_MEMORY); return NULL; }
        GLint old; glGetIntegerv(GL_TEXTURE_BINDING_2D,&old);
        glBindTexture(GL_TEXTURE_2D,texture->native_name);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D,old);
        texture->target=GL_TEXTURE_RECTANGLE_ARB;
        texture->hidden=texture->deleted=true; texture->refs=1;
        texture->next=gles_egl_current->textures->head;
        gles_egl_current->textures->head=texture;
        gles_egl_current->default_rectangle=texture;
        gles_egl_current->texture_cleanup=gles_native_texture_cleanup;
    }
    return gles_egl_current->default_rectangle;
}
static GLuint gles_native_texture_start(GLenum target)
{
    GLint old=0;
    if (target==GL_TEXTURE_2D || (target==GL_TEXTURE_RECTANGLE_ARB && gles_egl_current->api==1)) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&old);
        GLESEGLTexture *texture=gles_native_bound_texture(target);
        if (!texture && target==GL_TEXTURE_RECTANGLE_ARB) texture=gles_native_default_rectangle();
        if (texture) glBindTexture(GL_TEXTURE_2D,texture->native_name);
    }
    return old;
}
static void gles_native_texture_finish(GLenum target, GLuint old)
{
    if (target==GL_TEXTURE_2D || (target==GL_TEXTURE_RECTANGLE_ARB && gles_egl_current->api==1))
        glBindTexture(GL_TEXTURE_2D,old);
}
static void gles_native_bind_texture(GLenum target, GLuint name)
{
    bool rectangle=target==GL_TEXTURE_RECTANGLE_ARB;
    if (rectangle && gles_egl_current->api!=1) { gles_native_error(GL_INVALID_ENUM); return; }
    int unit=gles_native_unit();
    if (unit<0 || (target!=GL_TEXTURE_2D && target!=GL_TEXTURE_CUBE_MAP && !rectangle)) { glBindTexture(target,name); return; }
    GLESEGLTexture *texture=name ? gles_native_texture(name,true) :
        rectangle ? gles_native_default_rectangle() : target==GL_TEXTURE_2D ? &gles_egl_current->default_texture : &gles_egl_current->default_cube;
    if (!texture) return;
    if (texture->target && texture->target!=target) { gles_native_error(GL_INVALID_OPERATION); return; }
    gles_native_save_errors();
    glBindTexture(gles_native_target(target),texture->native_name);
    GLenum error=glGetError(); gles_native_error(error);
    if (error) return;
    if (!texture->target && rectangle) {
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    texture->target=target;
    GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
    GLESEGLTexture **binding=rectangle ? &state->object_rectangle : target==GL_TEXTURE_2D ? &state->object_2d : &state->object_cube;
    gles_native_texture_reference(binding,texture);
    if (rectangle) state->rectangle=name;
    else if (target==GL_TEXTURE_2D) state->texture_2d=name;
    if (rectangle || target==GL_TEXTURE_2D) state->host_rectangle=rectangle;
}
static void gles_native_gen_textures(GLsizei n, GLuint *names)
{
    if (n<0) { gles_native_error(GL_INVALID_VALUE); return; }
    for (GLsizei i=0;i<n;i++) {
        GLESEGLTexture *texture=calloc(1,sizeof(*texture));
        if (!texture) { gles_native_error(GL_OUT_OF_MEMORY); return; }
        glGenTextures(1,&texture->native_name);
        if (!texture->native_name) { free(texture); gles_native_error(GL_OUT_OF_MEMORY); return; }
        do {
            gles_egl_current->textures->next_name++;
        } while (!gles_egl_current->textures->next_name || gles_native_texture(gles_egl_current->textures->next_name,false));
        names[i]=texture->name=gles_egl_current->textures->next_name;
        texture->next=gles_egl_current->textures->head;
        gles_egl_current->textures->head=texture;
        gles_egl_current->texture_cleanup=gles_native_texture_cleanup;
    }
}
static GLboolean gles_native_is_texture(GLuint name)
{
    GLESEGLTexture *texture=name ? gles_native_texture(name,false) : NULL;
    return texture && texture->target ? glIsTexture(texture->native_name) : GL_FALSE;
}
static void gles_native_get_integer(GLenum pname, GLint *value)
{
    if (pname==GL_MAX_TEXTURE_UNITS && gles_egl_current->api==1) { *value=gles_native_fixed_units(); return; }
    /* The SGX's ES 1.1 limit; the guest's front end lowers it to the MBX's. */
    if (pname==GL_MAX_TEXTURE_LOD_BIAS_EXT && gles_native_lod_bias_kept()) { *value=4; return; }
    int unit=gles_native_unit();
    if (unit>=0 && pname==GL_TEXTURE_BINDING_RECTANGLE_ARB) { *value=gles_egl_current->texture_units[unit].rectangle; return; }
    if (unit>=0 && pname==GL_TEXTURE_BINDING_2D) { *value=gles_egl_current->texture_units[unit].texture_2d; return; }
    if (unit>=0 && pname==GL_TEXTURE_BINDING_CUBE_MAP) {
        GLESEGLTexture *texture=gles_native_bound_texture(GL_TEXTURE_CUBE_MAP);
        *value=texture ? texture->name : 0; return;
    }
    glGetIntegerv(pname==GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB ? GL_MAX_TEXTURE_SIZE : pname,value);
}
static void gles_native_get_float(GLenum pname, GLfloat *value)
{
    if (pname==GL_TEXTURE_BINDING_2D || pname==GL_TEXTURE_BINDING_CUBE_MAP ||
        pname==GL_TEXTURE_BINDING_RECTANGLE_ARB || pname==GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB ||
        (pname==GL_MAX_TEXTURE_UNITS && gles_egl_current->api==1) ||
        (pname==GL_MAX_TEXTURE_LOD_BIAS_EXT && gles_native_lod_bias_kept())) {
        GLint integer; gles_native_get_integer(pname,&integer); *value=integer; return;
    }
    glGetFloatv(pname,value);
}
static GLboolean gles_native_is_enabled(GLenum cap)
{
    int unit=gles_native_unit();
    if (unit>=0 && cap==GL_TEXTURE_RECTANGLE_ARB) return gles_egl_current->texture_units[unit].enabled_rectangle;
    if (unit>=0 && cap==GL_TEXTURE_2D && gles_egl_current->api==1) return gles_egl_current->texture_units[unit].enabled_2d;
    return glIsEnabled(cap);
}
static void gles_native_texture_enable(GLenum cap, bool enable)
{
    int unit=gles_native_unit();
    if (gles_egl_current->api==1 && unit>=0 && (cap==GL_TEXTURE_2D || cap==GL_TEXTURE_RECTANGLE_ARB)) {
        GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
        if (cap==GL_TEXTURE_2D) state->enabled_2d=enable;
        else state->enabled_rectangle=enable;
        if (state->enabled_2d || state->enabled_rectangle) glEnable(GL_TEXTURE_2D);
        else glDisable(GL_TEXTURE_2D);
        return;
    }
    if (enable) glEnable(cap); else glDisable(cap);
}
static void gles_native_enable(GLenum cap) { gles_native_texture_enable(cap,true); }
static void gles_native_disable(GLenum cap) { gles_native_texture_enable(cap,false); }

#define NATIVE_TEXTURE_PARAMETER(name, value_type) \
static void gles_native_##name(GLenum target, GLenum pname, value_type value) \
{ \
    GLuint old=gles_native_texture_start(target); \
    name(gles_native_target(target),pname,value); \
    gles_native_texture_finish(target,old); \
}
NATIVE_TEXTURE_PARAMETER(glTexParameteri, GLint)
NATIVE_TEXTURE_PARAMETER(glTexParameterf, GLfloat)
NATIVE_TEXTURE_PARAMETER(glTexParameteriv, const GLint *)
NATIVE_TEXTURE_PARAMETER(glTexParameterfv, const GLfloat *)
NATIVE_TEXTURE_PARAMETER(glGetTexParameteriv, GLint *)
NATIVE_TEXTURE_PARAMETER(glGetTexParameterfv, GLfloat *)
#undef NATIVE_TEXTURE_PARAMETER

/* Copy calls use the same logical target selection as uploads. */
static void gles_native_copy_tex_image(GLenum target, GLint level, GLenum internal,
                                       GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
{
    GLuint old=gles_native_texture_start(target);
    gles_native_save_errors();
    glCopyTexImage2D(gles_native_target(target),level,internal,x,y,w,h,border);
    GLenum error=glGetError(); gles_native_error(error);
    if (!error && !level) {
        GLESEGLTexture *texture=gles_native_bound_texture(target);
        if (texture) { texture->width=w; texture->height=h; }
    }
    gles_native_texture_finish(target,old);
}
static void gles_native_copy_tex_sub_image(GLenum target, GLint level, GLint xoff, GLint yoff,
                                           GLint x, GLint y, GLsizei w, GLsizei h)
{
    GLuint old=gles_native_texture_start(target);
    glCopyTexSubImage2D(gles_native_target(target),level,xoff,yoff,x,y,w,h);
    gles_native_texture_finish(target,old);
}
static void gles_native_delete_bound_attachments(GLESEGLTexture *texture);
static void gles_native_delete_textures(GLsizei n, const GLuint *names)
{
    if (n<0) { gles_native_error(GL_INVALID_VALUE); return; }
    GLint active; glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    for (GLsizei i=0;i<n;i++) {
        GLESEGLTexture *texture=names[i] ? gles_native_texture(names[i],false) : NULL;
        if (!texture) continue;
        texture->deleted=true;
        for (unsigned unit=0;unit<32;unit++) {
            GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
            if (state->object_2d!=texture && state->object_rectangle!=texture && state->object_cube!=texture) continue;
            glActiveTexture(GL_TEXTURE0+unit);
            if (state->object_2d==texture) gles_native_bind_texture(GL_TEXTURE_2D,0);
            if (state->object_rectangle==texture) gles_native_bind_texture(GL_TEXTURE_RECTANGLE_ARB,0);
            if (state->object_cube==texture) gles_native_bind_texture(GL_TEXTURE_CUBE_MAP,0);
        }
        gles_native_delete_bound_attachments(texture);
    }
    glActiveTexture(active);
    gles_native_collect_textures(true);
}
static unsigned gles_native_rectangle_begin(GLfloat matrices[8][16], GLint bindings[8],
                                            GLint *saved_active, GLint *saved_mode)
{
    if (gles_egl_current->api!=1) return 0;
    GLint units=gles_native_fixed_units();
    /*
     * With no rectangle texture enabled or bound on any unit, every unit's
     * native binding and GL_TEXTURE_2D enable already are what this would set
     * (object_2d, enabled_2d), so there is nothing to set or restore. Doing it
     * anyway cost ~10 driver calls per unit per draw and made the driver
     * revalidate its texture state on every draw.
     */
    unsigned unit=0;
    for (;unit<8 && unit<(unsigned)units;unit++) {
        GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
        if (state->enabled_rectangle || state->host_rectangle) break;
    }
    if (unit==8 || unit>=(unsigned)units) return 0;   /* *saved_active stays 0: nothing to end */
    glGetIntegerv(GL_ACTIVE_TEXTURE,saved_active); glGetIntegerv(GL_MATRIX_MODE,saved_mode);
    unsigned mask=0;
    for (unit=0;unit<8 && unit<(unsigned)units;unit++) {
        glActiveTexture(GL_TEXTURE0+unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&bindings[unit]);
        GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
        bool rectangle=state->enabled_rectangle && !glIsEnabled(GL_TEXTURE_CUBE_MAP) && !gles_egl_current->drawing_window_quad;
        GLESEGLTexture *texture=rectangle ? state->object_rectangle : state->object_2d;
        if (!texture) texture=rectangle ? gles_native_default_rectangle() : &gles_egl_current->default_texture;
        glBindTexture(GL_TEXTURE_2D,texture ? texture->native_name : 0);
        if (rectangle || state->enabled_2d) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
        if (rectangle) {
            if (texture && texture->width && texture->height) {
                glMatrixMode(GL_TEXTURE); glGetFloatv(GL_TEXTURE_MATRIX,matrices[unit]);
                glLoadIdentity(); glScalef(1.0f/texture->width,1.0f/texture->height,1);
                glMultMatrixf(matrices[unit]); mask|=1u<<unit;
            }
        }
    }
    glActiveTexture(*saved_active); glMatrixMode(*saved_mode);
    return mask;
}
static void gles_native_rectangle_end(unsigned mask, GLfloat matrices[8][16],
                                      GLint bindings[8], GLint active, GLint mode)
{
    if (gles_egl_current->api!=1 || !active) return;
    GLint units=gles_native_fixed_units();
    for (unsigned unit=0;unit<8 && unit<(unsigned)units;unit++) {
        glActiveTexture(GL_TEXTURE0+unit);
        if (mask&(1u<<unit)) { glMatrixMode(GL_TEXTURE); glLoadMatrixf(matrices[unit]); }
        glBindTexture(GL_TEXTURE_2D,bindings[unit]);
        GLESEGLTextureUnit *state=&gles_egl_current->texture_units[unit];
        if (state->enabled_2d || state->enabled_rectangle) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(active); glMatrixMode(mode);
}
static void gles_native_draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    GLfloat matrices[8][16]; GLint bindings[8],active=0,matrix=0;
    unsigned mask=gles_native_rectangle_begin(matrices,bindings,&active,&matrix);
    glDrawArrays(mode,first,count);
    gles_native_rectangle_end(mask,matrices,bindings,active,matrix);
}
static void gles_native_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    GLfloat matrices[8][16]; GLint bindings[8],active=0,matrix=0;
    unsigned mask=gles_native_rectangle_begin(matrices,bindings,&active,&matrix);
    glDrawElements(mode,count,type,indices);
    gles_native_rectangle_end(mask,matrices,bindings,active,matrix);
}
