/* SPDX-License-Identifier: GPL-2.0-or-later
 * GLES portability: ES1 extension dispatch, guest row strides and packed pixel
 * formats. No guest GL operation is replaced by a frame or canned drawing.
 */
#include <string.h>
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
static void gles_native_clip_plane(GLenum plane, const double *d)
{
    GLfloat f[] = {d[0], d[1], d[2], d[3]};
    glClipPlanef(plane, f);
}
static void gles_native_get_clip_plane(GLenum plane, double *out)
{
    GLfloat f[4];
    glGetClipPlanef(plane,f);
    for (unsigned i=0;i<4;i++) out[i]=f[i];
}
#define glGetClipPlane gles_native_get_clip_plane
#define glClipPlane gles_native_clip_plane

/* GL error flags are per context, including our emulated pixel-store state. */
static void gles_native_error(GLenum error)
{
    if (error >= GL_INVALID_ENUM && error <= GL_INVALID_FRAMEBUFFER_OPERATION)
        gles_egl_current->errors |= 1u << (error - GL_INVALID_ENUM);
}
static void gles_native_save_errors(void)
{
    GLenum error;
    while ((error=glGetError()) != GL_NO_ERROR) gles_native_error(error);
}
static GLenum gles_native_get_error(void)
{
    if (gles_egl_current->errors) {
        for (unsigned bit=0;bit<=GL_INVALID_FRAMEBUFFER_OPERATION-GL_INVALID_ENUM;bit++)
            if (gles_egl_current->errors & (1u<<bit)) {
                gles_egl_current->errors &= ~(1u<<bit);
                return GL_INVALID_ENUM+bit;
            }
    }
    return glGetError();
}
/* Host-setup diagnostics must not consume the context's guest-visible error
 * flags. Save driver flags in the existing per-context store, then inspect it. */
static GLenum gles_native_peek_error(void)
{
    gles_native_save_errors();
    for (unsigned bit=0;bit<=GL_INVALID_FRAMEBUFFER_OPERATION-GL_INVALID_ENUM;bit++)
        if (gles_egl_current->errors & (1u<<bit)) return GL_INVALID_ENUM+bit;
    return GL_NO_ERROR;
}
static bool gles_native_extension(const char *extensions, const char *name)
{
    size_t length=strlen(name);
    if (!extensions || !length) return false;
    for (const char *p=extensions;(p=strstr(p,name));p+=length)
        if ((p==extensions || p[-1]==' ') && (!p[length] || p[length]==' ')) return true;
    return false;
}
static bool gles_native_require_fbo(void)
{
    if (gles_egl_current->api == 2) return true;
    if (!gles_egl_current->fbo_checked) {
        const char *extensions=(const char *)glGetString(GL_EXTENSIONS);
        gles_egl_current->fbo_supported=gles_native_extension(extensions,"GL_OES_framebuffer_object");
        gles_egl_current->fbo_checked=true;
        if (!gles_egl_current->fbo_supported)
            fprintf(stderr,"[gles-egl] unsupported required GLES1 capability GL_OES_framebuffer_object; renderer=%s version=%s\n",
                    glGetString(GL_RENDERER),glGetString(GL_VERSION));
    }
    return gles_egl_current->fbo_supported;
}
static void gles_native_pixel_store(GLenum pname, GLint value)
{
    if (pname == GL_UNPACK_ROW_LENGTH) {
        if (value < 0) { gles_native_error(GL_INVALID_VALUE); return; }
        gles_egl_current->unpack_row_length = value;
        return;
    }
    if (pname == GL_UNPACK_ALIGNMENT && value != 1 && value != 2 && value != 4 && value != 8) {
        gles_native_error(GL_INVALID_VALUE); return;
    }
    glPixelStorei(pname, value);
    if (pname == GL_UNPACK_ALIGNMENT) gles_egl_current->unpack_alignment = value;
}
static unsigned gles_native_bpp(GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
        type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_4_4_4_4_REV ||
        type == GL_UNSIGNED_SHORT_1_5_5_5_REV) return 2;
    /* Packed depth/stencil (OES_packed_depth_stencil): Epic Citadel's 960x640 depth texture. Counted as one
     * byte a texel, the copy was a quarter of what the driver reads (Adreno faulted past its end). */
    if (type == GL_UNSIGNED_INT_8_8_8_8 || type == GL_UNSIGNED_INT_8_8_8_8_REV || type == 0x84FA) return 4;
    if (type == 0x8DAD) return 8;   /* GL_FLOAT_32_UNSIGNED_INT_24_8_REV */
    unsigned components = format == GL_RGBA || format == GL_BGRA ? 4 :
        format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
    return components * (type == GL_FLOAT || type == GL_UNSIGNED_INT ? 4 :
                         type == GL_UNSIGNED_SHORT || type == 0x8D61 ? 2 : 1);
}
static void *gles_native_texels(GLsizei w, GLsizei h, GLenum *format, GLenum *type, const void *pixels)
{
    if (!pixels || w <= 0 || h <= 0) return NULL;
    unsigned bpp = gles_native_bpp(*format, *type);
    unsigned align = gles_egl_current->unpack_alignment;
    size_t rowbytes = (size_t)(gles_egl_current->unpack_row_length ?
        gles_egl_current->unpack_row_length : (unsigned)w) * bpp;
    size_t stride = (rowbytes + align - 1) & ~(size_t)(align - 1);
    bool bgra = *format == GL_BGRA;
    bool packed = *type == GL_UNSIGNED_INT_8_8_8_8 || *type == GL_UNSIGNED_INT_8_8_8_8_REV;
    bool reverse16 = *type == GL_UNSIGNED_SHORT_4_4_4_4_REV || *type == GL_UNSIGNED_SHORT_1_5_5_5_REV;
    unsigned output_bpp = reverse16 || packed ? 4 : bpp;
    if ((size_t)w > (64 * 1024 * 1024u) / output_bpp / (unsigned)h) return NULL;
    unsigned char *out = malloc((size_t)w * h * output_bpp);
    if (!out) return NULL;
    for (unsigned y = 0; y < (unsigned)h; y++) {
        const unsigned char *src = (const unsigned char *)pixels + y * stride;
        unsigned char *dst = out + (size_t)y * w * output_bpp;
        if (reverse16) {
            for (unsigned x = 0; x < (unsigned)w; x++) {
                unsigned v = src[x*2] | src[x*2+1] << 8;
                if (*type == GL_UNSIGNED_SHORT_4_4_4_4_REV) {
                    dst[x*4]=((v>>8)&15)*17; dst[x*4+1]=((v>>4)&15)*17;
                    dst[x*4+2]=(v&15)*17; dst[x*4+3]=(v>>12)*17;
                } else {
                    dst[x*4]=((v>>10)&31)*255/31; dst[x*4+1]=((v>>5)&31)*255/31;
                    dst[x*4+2]=(v&31)*255/31; dst[x*4+3]=(v>>15)*255;
                }
            }
        } else if (bgra && !packed && *type != GL_UNSIGNED_BYTE) {
            unsigned component=bpp/4;
            for (unsigned x=0;x<(unsigned)w;x++) for (unsigned k=0;k<4;k++)
                memcpy(dst+x*bpp+k*component,src+x*bpp+(k==0?2:k==2?0:k)*component,component);
        } else if (bgra || packed) {
            for (unsigned x = 0; x < (unsigned)w; x++) {
                const unsigned char *p = src+x*4;
                unsigned char q[4];
                for (unsigned k=0;k<4;k++) q[k]=p[packed && *type==GL_UNSIGNED_INT_8_8_8_8 ? 3-k : k];
                dst[x*4]=q[bgra?2:0]; dst[x*4+1]=q[1]; dst[x*4+2]=q[bgra?0:2]; dst[x*4+3]=q[3];
            }
        } else memcpy(dst,src,(size_t)w*bpp);
    }
    if (bgra) *format=GL_RGBA;
    if (reverse16 || packed) { *format=GL_RGBA; *type=GL_UNSIGNED_BYTE; }
    return out;
}
typedef struct GLESNativeAttachment {
    GLuint framebuffer;
    GLenum attachment;
    GLESEGLTexture *texture;
    struct GLESNativeAttachment *next;
} GLESNativeAttachment;
static void gles_native_collect_textures(bool current)
{
    if (!current) return;
    GLESEGLTexture **slot=&gles_egl_current->textures->head;
    while (*slot) {
        GLESEGLTexture *texture=*slot;
        if (!texture->deleted || texture->refs) { slot=&texture->next; continue; }
        glDeleteTextures(1,&texture->native_name);
        *slot=texture->next;
        memset(gles_egl_current->textures->recent,0,sizeof(gles_egl_current->textures->recent));
        gles_egl_texture_free(texture);
    }
}
static void gles_native_texture_cleanup(CGLContextObj context, bool current)
{
    for (unsigned unit=0;unit<32;unit++) {
        GLESEGLTextureUnit *state=&context->texture_units[unit];
        GLESEGLTexture *objects[]={state->object_2d,state->object_rectangle,state->object_cube};
        for (unsigned i=0;i<3;i++) if (objects[i] && objects[i]->refs) objects[i]->refs--;
    }
    GLESNativeAttachment *attachment=context->texture_attachments;
    while (attachment) {
        GLESNativeAttachment *next=attachment->next;
        if (attachment->texture) attachment->texture->refs--;
        free(attachment); attachment=next;
    }
    context->texture_attachments=NULL;
    if (context->default_rectangle) context->default_rectangle->refs--;
    if (current) gles_native_collect_textures(true);
}
static GLESEGLTexture *gles_native_texture(unsigned name, bool create)
{
    if (!name) return &gles_egl_current->default_texture;
    /* Every bind looks its name up; a game has hundreds of textures in this list. */
    GLESEGLTexture **recent=&gles_egl_current->textures->recent[name&255];
    if (*recent && (*recent)->name==name && !(*recent)->deleted && !(*recent)->hidden) return *recent;
    for (GLESEGLTexture *texture=gles_egl_current->textures->head;texture;texture=texture->next)
        if (texture->name==name && !texture->deleted && !texture->hidden) return *recent=texture;
    if (!create) return NULL;
    GLESEGLTexture *texture=calloc(1,sizeof(*texture));
    if (!texture) { gles_native_error(GL_OUT_OF_MEMORY); return NULL; }
    texture->name=name; texture->native_name=name;
    for (GLESEGLTexture *other=gles_egl_current->textures->head;other;other=other->next)
        if (other->native_name==name) {
            texture->native_name=0;
            glGenTextures(1,&texture->native_name);
            if (!texture->native_name) { free(texture); gles_native_error(GL_OUT_OF_MEMORY); return NULL; }
            break;
        }
    texture->next=gles_egl_current->textures->head;
    gles_egl_current->textures->head=texture;
    gles_egl_current->texture_cleanup=gles_native_texture_cleanup;
    return texture;
}
static GLESEGLTexture *gles_native_texture_from_native(GLuint native)
{
    if (!native) return &gles_egl_current->default_texture;
    for (GLESEGLTexture *texture=gles_egl_current->textures->head;texture;texture=texture->next)
        if (texture->native_name==native) return texture;
    return NULL;
}
static void gles_native_texture_reference(GLESEGLTexture **binding, GLESEGLTexture *texture)
{
    if (texture) texture->refs++;
    if (*binding && (*binding)->refs) (*binding)->refs--;
    *binding=texture;
    gles_egl_current->texture_cleanup=gles_native_texture_cleanup;
}
static void gles_native_attach_texture(GLenum attachment, GLESEGLTexture *texture)
{
    GLint framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    GLESNativeAttachment **slot=(GLESNativeAttachment **)&gles_egl_current->texture_attachments;
    while (*slot && ((*slot)->framebuffer!=(GLuint)framebuffer || (*slot)->attachment!=attachment)) slot=&(*slot)->next;
    if (!*slot && texture) {
        *slot=calloc(1,sizeof(**slot));
        if (!*slot) { gles_native_error(GL_OUT_OF_MEMORY); return; }
        (*slot)->framebuffer=framebuffer; (*slot)->attachment=attachment;
    }
    if (*slot) {
        gles_native_texture_reference(&(*slot)->texture,texture);
        if (!texture) { GLESNativeAttachment *old=*slot; *slot=old->next; free(old); }
    }
    gles_native_collect_textures(true);
}
static void gles_native_delete_texture_attachments(GLsizei n, const GLuint *names)
{
    GLESNativeAttachment **slot=(GLESNativeAttachment **)&gles_egl_current->texture_attachments;
    while (*slot) {
        bool remove=false;
        for (GLsizei i=0;i<n;i++) if (names[i]==(*slot)->framebuffer) remove=true;
        if (!remove) { slot=&(*slot)->next; continue; }
        GLESNativeAttachment *old=*slot; *slot=old->next;
        if (old->texture) old->texture->refs--;
        free(old);
    }
    gles_native_collect_textures(true);
}
/*
 * EXT_texture_lod_bias. The MBX and the SGX's ES 1.1 have it, and the guest's
 * extension list names it, but Adreno's ES1 context refuses
 * TEXTURE_FILTER_CONTROL_EXT with INVALID_ENUM. That error reached the guest's
 * next glGetError: Zombie Highway sets a bias before each PVRTC upload and took
 * the error read after level 0 for an upload failure, so every texture came
 * back nil and the game crashed (issue 17). Where the host refuses it, the bias
 * is kept per texture unit here and read back by glGetTexEnv, but not applied:
 * textures pick their mip level without it. Adreno 650 answers the get (0) and
 * MAX_TEXTURE_LOD_BIAS but refuses every set, so the probe sets the value it
 * reads back.
 */
static bool gles_native_lod_bias_kept(void)
{
    static int kept=-1;   /* one driver per worker process: its ES1 answer is the same for every context */
    if (gles_egl_current->api!=1) return false;
    if (kept<0) {
        GLfloat bias=0;
        gles_native_save_errors();
        glGetTexEnvfv(GL_TEXTURE_FILTER_CONTROL_EXT,GL_TEXTURE_LOD_BIAS_EXT,&bias);
        glTexEnvf(GL_TEXTURE_FILTER_CONTROL_EXT,GL_TEXTURE_LOD_BIAS_EXT,bias);
        kept=glGetError()!=GL_NO_ERROR;
        for (unsigned i=0;i<8 && glGetError()!=GL_NO_ERROR;i++) {}
        fprintf(stderr,"[gles-egl] GLES1 GL_EXT_texture_lod_bias: %s\n",
                kept ? "not on this host, kept by the bridge and not applied" : "host");
    }
    return kept;
}
static GLfloat *gles_native_lod_bias(void)
{
    GLint active=GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    unsigned unit=active>=GL_TEXTURE0 && active<GL_TEXTURE0+32 ? active-GL_TEXTURE0 : 0;
    return &gles_egl_current->texture_units[unit].lod_bias;
}
static bool gles_native_lod_bias_set(GLenum target, GLenum pname, GLfloat value)
{
    if (target!=GL_TEXTURE_FILTER_CONTROL_EXT || !gles_native_lod_bias_kept()) return false;
    if (pname!=GL_TEXTURE_LOD_BIAS_EXT) gles_native_error(GL_INVALID_ENUM);
    else *gles_native_lod_bias()=value;
    return true;
}
static bool gles_native_lod_bias_get(GLenum target, GLenum pname, GLfloat *value)
{
    if (target!=GL_TEXTURE_FILTER_CONTROL_EXT || !gles_native_lod_bias_kept()) return false;
    if (pname!=GL_TEXTURE_LOD_BIAS_EXT) gles_native_error(GL_INVALID_ENUM);
    else *value=*gles_native_lod_bias();
    return true;
}
static void gles_native_tex_envf(GLenum target, GLenum pname, GLfloat value)
{
    if (!gles_native_lod_bias_set(target,pname,value)) glTexEnvf(target,pname,value);
}
static void gles_native_tex_envfv(GLenum target, GLenum pname, const GLfloat *value)
{
    if (!gles_native_lod_bias_set(target,pname,value[0])) glTexEnvfv(target,pname,value);
}
static void gles_native_tex_enviv(GLenum target, GLenum pname, const GLint *value)
{
    if (!gles_native_lod_bias_set(target,pname,(GLfloat)value[0])) glTexEnviv(target,pname,value);
}
static void gles_native_get_tex_envfv(GLenum target, GLenum pname, GLfloat *value)
{
    if (!gles_native_lod_bias_get(target,pname,value)) glGetTexEnvfv(target,pname,value);
}
static void gles_native_get_tex_enviv(GLenum target, GLenum pname, GLint *value)
{
    GLfloat bias;
    if (!gles_native_lod_bias_get(target,pname,&bias)) glGetTexEnviv(target,pname,value);
    else if (pname==GL_TEXTURE_LOD_BIAS_EXT) *value=(GLint)lrintf(bias);
}
static void gles_native_level_set(GLenum native_target, GLint level, GLenum internal, GLsizei w, GLsizei h,
                                  GLenum format, GLenum type, const void *pixels);
#include "gles-host-rectangle.c.inc"
/* Whether a level can be read back through a framebuffer as RGBA bytes (save states); else the layer keeps its bytes. */
static bool gles_native_level_readable(GLenum format, GLenum type)
{
    return (format==GL_RGBA || format==GL_RGB || format==GL_BGRA_EXT) &&
           (type==GL_UNSIGNED_BYTE || type==GL_UNSIGNED_SHORT_5_6_5 || type==GL_UNSIGNED_SHORT_4_4_4_4 ||
            type==GL_UNSIGNED_SHORT_5_5_5_1);
}
static size_t gles_native_level_bpp(GLenum format, GLenum type)
{
    size_t n = format==GL_RGBA || format==GL_BGRA_EXT ? 4 : format==GL_RGB ? 3 :
               format==GL_LUMINANCE_ALPHA || format==0x8227 /* RG */ ? 2 : 1;
    if (type==GL_UNSIGNED_SHORT_5_6_5 || type==GL_UNSIGNED_SHORT_4_4_4_4 || type==GL_UNSIGNED_SHORT_5_5_5_1) return 2;
    if (type==GL_FLOAT) return n * 4;
    if (type==0x8D61 /* HALF_FLOAT_OES */ || type==0x140B /* HALF_FLOAT */) return n * 2;
    return n;
}
/* The 2D texture bound natively (as an upload leaves it), or NULL for the default one. */
static GLESEGLTexture *gles_native_level_texture(GLenum native_target, GLint level)
{
    if (native_target!=GL_TEXTURE_2D || level<0 || level>=GLES_EGL_LEVELS) return NULL;
    GLint name=0; glGetIntegerv(GL_TEXTURE_BINDING_2D,&name);
    return name ? gles_native_texture_from_native(name) : NULL;
}
/* A level (re)specified: its size and format, and its bytes where a framebuffer can't read them. */
static void gles_native_level_set(GLenum native_target, GLint level, GLenum internal, GLsizei w, GLsizei h,
                                  GLenum format, GLenum type, const void *pixels)
{
    GLESEGLTexture *texture=gles_native_level_texture(native_target,level);
    if (!texture) return;
    GLESEGLLevel *l=&texture->level[level];
    free(l->shadow);
    *l=(GLESEGLLevel){ w, h, internal, format, type, false, NULL };
    if (!gles_native_level_readable(format,type) && w>0 && h>0) {
        size_t n=(size_t)w*h*gles_native_level_bpp(format,type);
        l->shadow=pixels ? malloc(n) : calloc(1,n);
        if (l->shadow && pixels) memcpy(l->shadow,pixels,n);
    }
}
static void gles_native_level_update(GLenum native_target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                     GLenum format, GLenum type, const void *pixels)
{
    GLESEGLTexture *texture=gles_native_level_texture(native_target,level);
    GLESEGLLevel *l=texture ? &texture->level[level] : NULL;
    if (!l || !l->shadow || !pixels || format!=l->format || type!=l->type || x<0 || y<0 || w<=0 || h<=0 ||
        (unsigned)x+w>l->width || (unsigned)y+h>l->height) return;
    size_t bpp=gles_native_level_bpp(format,type);
    for (GLsizei row=0;row<h;row++)
        memcpy((uint8_t *)l->shadow+((size_t)(y+row)*l->width+x)*bpp,(const uint8_t *)pixels+(size_t)row*w*bpp,
               (size_t)w*bpp);
}
/* glGenerateMipmap made the chain below level 0. */
static void gles_native_levels_generated(GLenum native_target)
{
    GLESEGLTexture *texture=gles_native_level_texture(native_target,0);
    if (!texture || !texture->level[0].width) return;
    GLESEGLLevel base=texture->level[0];
    unsigned w=base.width, h=base.height;
    for (unsigned level=1;level<GLES_EGL_LEVELS && (w>1 || h>1);level++) {
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
        free(texture->level[level].shadow);
        texture->level[level]=(GLESEGLLevel){ w, h, base.internal, base.format, base.type, true, NULL };
    }
}
static void gles_native_tex_image(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                                  GLint border, GLenum format, GLenum type, const void *pixels)
{
    void *copy = gles_native_texels(w,h,&format,&type,pixels);
    if (pixels && !copy) { gles_native_error(w <= 0 || h <= 0 ? GL_INVALID_VALUE : GL_OUT_OF_MEMORY); return; }
    /*
     * ES takes no conversion in glTexImage2D: the internal format must be the
     * data's format, or the upload fails with GL_INVALID_OPERATION and leaves the
     * texture empty (drawn white, or grey when lit). Desktop GL converts, and the
     * bridge relies on that: opaque PVRTC is decoded to RGBA and uploaded as
     * GL_RGB (every opaque PVRTC texture in Real Racing, Jetpack Joyride and
     * Battle Bears was missing on Android), and surfaces upload 565, alpha and
     * luminance data as GL_RGBA. Do the desktop's conversion here: RGB keeps
     * opaque alpha; alpha and luminance data expand to RGBA as (0,0,0,a) and
     * (l,l,l,a) (copy is a fresh tightly packed buffer).
     */
    if (format == GL_RGBA && (internal == GL_BGRA || internal == GL_RGBA)) internal=GL_RGBA;
    else if (internal == GL_RGB && format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        if (copy) for (size_t i = 0; i < (size_t)w * h; i++) ((unsigned char *)copy)[i * 4 + 3] = 255;
        internal = GL_RGBA;
    } else if (internal == GL_RGBA && format == GL_RGB) internal = GL_RGB;
    else if (internal == GL_RGBA && type == GL_UNSIGNED_BYTE &&
             (format == GL_ALPHA || format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA)) {
        unsigned char *rgba = copy ? malloc((size_t)w * h * 4) : NULL;
        if (copy && !rgba) { free(copy); gles_native_error(GL_OUT_OF_MEMORY); return; }
        if (copy) {
            const unsigned char *src = copy;
            unsigned n = format == GL_LUMINANCE_ALPHA ? 2 : 1;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                unsigned char l = format == GL_ALPHA ? 0 : src[i * n];
                unsigned char a = format == GL_LUMINANCE ? 255 : src[i * n + n - 1];
                rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = l; rgba[i*4+3] = a;
            }
            free(copy); copy = rgba;
        }
        format = GL_RGBA;
    }
    GLuint old=gles_native_texture_start(target);
    GLenum native_target=gles_native_target(target);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    gles_native_save_errors();
    glTexImage2D(native_target,level,internal,w,h,border,format,type,copy);
    GLenum error=glGetError();
    gles_native_error(error);
    if (error==GL_NO_ERROR && native_target==GL_TEXTURE_2D && !level) {
        GLint name; glGetIntegerv(GL_TEXTURE_BINDING_2D,&name);
        GLESEGLTexture *texture=gles_native_texture_from_native(name);
        if (texture) { texture->width=w; texture->height=h; }
    }
    if (error==GL_NO_ERROR) gles_native_level_set(native_target,level,internal,w,h,format,type,copy);
    glPixelStorei(GL_UNPACK_ALIGNMENT,gles_egl_current->unpack_alignment);
    gles_native_texture_finish(target,old);
    free(copy);
}
static void gles_native_tex_sub_image(GLenum target, GLint level, GLint x, GLint y,
                                      GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)
{
    void *copy=gles_native_texels(w,h,&format,&type,pixels);
    if (pixels && !copy) { gles_native_error(w <= 0 || h <= 0 ? GL_INVALID_VALUE : GL_OUT_OF_MEMORY); return; }
    GLuint old=gles_native_texture_start(target);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexSubImage2D(gles_native_target(target),level,x,y,w,h,format,type,copy);
    gles_native_level_update(gles_native_target(target),level,x,y,w,h,format,type,copy);
    glPixelStorei(GL_UNPACK_ALIGNMENT,gles_egl_current->unpack_alignment);
    gles_native_texture_finish(target,old);
    free(copy);
}
static void gles_native_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h,
                                    GLenum format, GLenum type, void *pixels)
{
    bool rgba=format==GL_RGBA || format==GL_BGRA;
    bool packed32=type==GL_UNSIGNED_INT_8_8_8_8 || type==GL_UNSIGNED_INT_8_8_8_8_REV;
    bool packed16=type==GL_UNSIGNED_SHORT_4_4_4_4 || type==GL_UNSIGNED_SHORT_5_5_5_1 ||
                  type==GL_UNSIGNED_SHORT_4_4_4_4_REV || type==GL_UNSIGNED_SHORT_1_5_5_5_REV;
    bool rgb565=format==GL_RGB && type==GL_UNSIGNED_SHORT_5_6_5;
    if (format==GL_RGBA && type==GL_UNSIGNED_BYTE) {
        glReadPixels(x,y,w,h,format,type,pixels); return;
    }
    if (!(rgba && (type==GL_UNSIGNED_BYTE || packed16 || packed32)) &&
        !(format==GL_RGB && type==GL_UNSIGNED_BYTE) && !rgb565) {
        glReadPixels(x,y,w,h,format,type,pixels); return;
    }
    if (w <= 0 || h <= 0 || (size_t)w > 64*1024*1024u/4/(unsigned)h) {
        glReadPixels(x,y,w,h,format,type,pixels); return;
    }
    /*
     * The present reads every frame back as B,G,R,A bytes (GL_BGRA with
     * UNSIGNED_BYTE or 8_8_8_8_REV). Where the driver has
     * GL_EXT_read_format_bgra it does that itself; otherwise the RGBA read is
     * swapped a word at a time. Either way, not the per-byte loop below into a
     * fresh 600 KB allocation, which was ~2% of the vCPU in a game on the Poco.
     * LT_GLES_BGRA_READ=0 keeps the old path, for A/B.
     */
    static int bgra_read=-1;
    if (bgra_read < 0) {
        const char *e=getenv("LT_GLES_BGRA_READ");
        bgra_read=e && !atoi(e) ? 0 :
            gles_native_extension((const char *)glGetString(GL_EXTENSIONS),"GL_EXT_read_format_bgra") ? 2 : 1;
    }
    bool bgra32=bgra_read && format==GL_BGRA && (type==GL_UNSIGNED_BYTE || type==GL_UNSIGNED_INT_8_8_8_8_REV);
    if (bgra32 && bgra_read==2) {
        gles_native_save_errors();
        glReadPixels(x,y,w,h,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
        GLenum error=glGetError();
        if (error==GL_NO_ERROR) return;
        bgra_read=1;  /* not for this surface after all: swap by hand from now on */
    }
    static unsigned char *scratch; static size_t scratch_size;
    unsigned char *copy;
    if (bgra32) {
        if (scratch_size < (size_t)w*h*4) {
            free(scratch); scratch_size=0;
            if (!(scratch=malloc((size_t)w*h*4))) { gles_native_error(GL_OUT_OF_MEMORY); return; }
            scratch_size=(size_t)w*h*4;
        }
        copy=scratch;
    } else if (!(copy=malloc((size_t)w*h*4))) { gles_native_error(GL_OUT_OF_MEMORY); return; }
    gles_native_save_errors();
    GLint alignment; glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,copy);
    GLenum error=glGetError();
    glPixelStorei(GL_PACK_ALIGNMENT,alignment);
    if (error != GL_NO_ERROR) { gles_native_error(error); if (copy!=scratch) free(copy); return; }
    unsigned bpp=packed16 || rgb565 ? 2 : format==GL_RGB ? 3 : 4;
    size_t stride=((size_t)w*bpp+alignment-1)&~(size_t)(alignment-1);
    if (bgra32) {
        for (unsigned row=0;row<(unsigned)h;row++) {
            const uint32_t *src=(const uint32_t *)(copy+(size_t)row*w*4);
            unsigned char *dst=(unsigned char *)pixels+row*stride;
            for (unsigned col=0;col<(unsigned)w;col++) {
                uint32_t v=src[col];
                v=(v&0xff00ff00u) | (v>>16&0xffu) | (v&0xffu)<<16;
                memcpy(dst+col*4,&v,4);
            }
        }
        return;
    }
    for (unsigned row=0;row<(unsigned)h;row++) for (unsigned col=0;col<(unsigned)w;col++) {
        unsigned char *src=copy+((size_t)row*w+col)*4, *dst=(unsigned char *)pixels+row*stride+col*bpp;
        unsigned char q[4]={src[format==GL_BGRA?2:0],src[1],src[format==GL_BGRA?0:2],src[3]};
        if (packed32) {
            for (unsigned k=0;k<4;k++) dst[k]=q[type==GL_UNSIGNED_INT_8_8_8_8?3-k:k];
        } else if (packed16 || rgb565) {
            unsigned v;
            if (rgb565) v=(q[0]>>3)<<11 | (q[1]>>2)<<5 | q[2]>>3;
            else if (type==GL_UNSIGNED_SHORT_4_4_4_4) v=(q[0]>>4)<<12 | (q[1]>>4)<<8 | (q[2]>>4)<<4 | q[3]>>4;
            else if (type==GL_UNSIGNED_SHORT_5_5_5_1) v=(q[0]>>3)<<11 | (q[1]>>3)<<6 | (q[2]>>3)<<1 | q[3]>>7;
            else if (type==GL_UNSIGNED_SHORT_4_4_4_4_REV) v=(q[3]>>4)<<12 | (q[2]>>4)<<8 | (q[1]>>4)<<4 | q[0]>>4;
            else v=(q[3]>>7)<<15 | (q[2]>>3)<<10 | (q[1]>>3)<<5 | q[0]>>3;
            dst[0]=v;dst[1]=v>>8;
        } else memcpy(dst,q,bpp);
    }
    free(copy);
}
static GLenum gles_native_driver_error(void) { return glGetError(); }
#undef glGetError
#define glGetError gles_native_get_error
#undef glPixelStorei
#define glPixelStorei gles_native_pixel_store
#undef glTexImage2D
#define glTexImage2D gles_native_tex_image
#undef glTexSubImage2D
#define glTexSubImage2D gles_native_tex_sub_image
#undef glReadPixels
#define glReadPixels gles_native_read_pixels
#undef glCopyTexImage2D
#define glCopyTexImage2D gles_native_copy_tex_image
#undef glCopyTexSubImage2D
#define glCopyTexSubImage2D gles_native_copy_tex_sub_image

/* ES1 FBO extensions are loaded via EGL, ES2 uses core dispatch. */
static GLboolean gles_native_IsFramebuffer(GLuint name)
{
    typedef GLboolean (*Proc)(GLuint name);
    static Proc es1;
    if (gles_egl_current->api == 2) { return glIsFramebuffer(name); }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return 0;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glIsFramebufferOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glIsFramebufferOES\n"); gles_native_error(GL_INVALID_OPERATION); return 0; }
    return es1(name);
}
static GLboolean gles_native_IsRenderbuffer(GLuint name)
{
    typedef GLboolean (*Proc)(GLuint name);
    static Proc es1;
    if (gles_egl_current->api == 2) { return glIsRenderbuffer(name); }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return 0;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glIsRenderbufferOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glIsRenderbufferOES\n"); gles_native_error(GL_INVALID_OPERATION); return 0; }
    return es1(name);
}
static void gles_native_GenerateMipmap(GLenum target)
{
    typedef void (*Proc)(GLenum target);
    static Proc es1;
    if (target==GL_TEXTURE_RECTANGLE_ARB) { gles_native_error(GL_INVALID_ENUM); return; }
    if (gles_egl_current->api==1) {
        if (!gles_native_require_fbo()) { gles_native_error(GL_INVALID_OPERATION); return; }
        if (!es1) es1=(Proc)eglGetProcAddress("glGenerateMipmapOES");
        if (!es1) { gles_native_error(GL_INVALID_OPERATION); return; }
    }
    GLuint old=gles_native_texture_start(target);
    gles_native_save_errors();
    if (gles_egl_current->api==2) glGenerateMipmap(target); else es1(target);
    GLenum error=gles_native_driver_error(); gles_native_error(error);
    if (!error) gles_native_levels_generated(gles_native_target(target));
    gles_native_texture_finish(target,old);
}
/*
 * ES1 blending extensions. iOS 3's ES 1.1 has OES_blend_subtract,
 * OES_blend_equation_separate and OES_blend_func_separate, and a guest reaches
 * them as glBlendEquationOES and friends. The ES2 core names must not serve an
 * ES1 context here: since Android 10, libGLESv2's dispatch table for an ES1
 * context holds NULL for every entry outside the ES1 list, so glBlendEquation
 * in one jumps to address 0 (Critical Wave on an iPod touch 2G, issue 10). The
 * OES entry is used only when the ES1 context lists the extension; without it
 * the closest ES1 state is set and the difference is reported once.
 */
typedef enum { GLES_NATIVE_BLEND_SUBTRACT, GLES_NATIVE_BLEND_EQUATION_SEPARATE,
               GLES_NATIVE_BLEND_FUNC_SEPARATE, GLES_NATIVE_BLEND_COUNT } GLESNativeBlend;
static void *gles_native_blend_entry(GLESNativeBlend which)
{
    static const char *const extension[GLES_NATIVE_BLEND_COUNT]={
        "GL_OES_blend_subtract","GL_OES_blend_equation_separate","GL_OES_blend_func_separate"};
    static const char *const entry[GLES_NATIVE_BLEND_COUNT]={
        "glBlendEquationOES","glBlendEquationSeparateOES","glBlendFuncSeparateOES"};
    /* One driver per worker process, so its ES1 answer is the same for every context. */
    static int checked[GLES_NATIVE_BLEND_COUNT];
    static void *proc[GLES_NATIVE_BLEND_COUNT];
    if (!checked[which]) {
        const char *extensions=(const char *)glGetString(GL_EXTENSIONS);
        if (gles_native_extension(extensions,extension[which]))
            proc[which]=(void *)eglGetProcAddress(entry[which]);
        checked[which]=1;
        fprintf(stderr,"[gles-egl] GLES1 %s: %s\n",extension[which],
                proc[which] ? "host entry" : "not on this host, approximated");
    }
    return proc[which];
}
static void gles_native_blend_approximated(GLESNativeBlend which, const char *what)
{
    static bool said[GLES_NATIVE_BLEND_COUNT];
    if (said[which]) return;
    said[which]=true;
    fprintf(stderr,"[gles-egl] %s has no host ES1 equivalent; drawn as the nearest ES1 blend\n",what);
}
static void gles_native_BlendEquation(GLenum mode)
{
    typedef void (*Proc)(GLenum mode);
    if (gles_egl_current->api==2) { glBlendEquation(mode); return; }
    Proc es1=(Proc)gles_native_blend_entry(GLES_NATIVE_BLEND_SUBTRACT);
    if (es1) { es1(mode); return; }
    /* Without the extension the equation is always FUNC_ADD. */
    if (mode!=GL_FUNC_ADD) {
        gles_native_blend_approximated(GLES_NATIVE_BLEND_SUBTRACT,"a subtract blend equation");
        gles_native_error(GL_INVALID_ENUM);
    }
}
static void gles_native_BlendEquationSeparate(GLenum rgb, GLenum alpha)
{
    typedef void (*Proc)(GLenum rgb, GLenum alpha);
    if (gles_egl_current->api==2) { glBlendEquationSeparate(rgb,alpha); return; }
    Proc es1=(Proc)gles_native_blend_entry(GLES_NATIVE_BLEND_EQUATION_SEPARATE);
    if (es1) { es1(rgb,alpha); return; }
    if (rgb!=alpha)
        gles_native_blend_approximated(GLES_NATIVE_BLEND_EQUATION_SEPARATE,"a separate alpha blend equation");
    gles_native_BlendEquation(rgb);
}
static void gles_native_BlendFuncSeparate(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha, GLenum dst_alpha)
{
    typedef void (*Proc)(GLenum, GLenum, GLenum, GLenum);
    if (gles_egl_current->api==2) { glBlendFuncSeparate(src_rgb,dst_rgb,src_alpha,dst_alpha); return; }
    Proc es1=(Proc)gles_native_blend_entry(GLES_NATIVE_BLEND_FUNC_SEPARATE);
    if (es1) { es1(src_rgb,dst_rgb,src_alpha,dst_alpha); return; }
    if (src_rgb!=src_alpha || dst_rgb!=dst_alpha)
        gles_native_blend_approximated(GLES_NATIVE_BLEND_FUNC_SEPARATE,"a separate alpha blend function");
    glBlendFunc(src_rgb,dst_rgb);
}
static void gles_native_GenFramebuffers(GLsizei n, GLuint *names)
{
    typedef void (*Proc)(GLsizei n, GLuint *names);
    static Proc es1;
    if (gles_egl_current->api == 2) { glGenFramebuffers(n,names); return; }
    if (!gles_native_require_fbo()) {
        if (n > 0 && names) memset(names,0,(size_t)n*sizeof(*names));
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glGenFramebuffersOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glGenFramebuffersOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(n,names);
}
static void gles_native_BindFramebuffer(GLenum target, GLuint name)
{
    typedef void (*Proc)(GLenum target, GLuint name);
    static Proc es1;
    if (gles_egl_current->api == 2) { glBindFramebuffer(target,name); return; }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glBindFramebufferOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glBindFramebufferOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(target,name);
}
static void gles_native_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint name, GLint level)
{
    typedef void (*Proc)(GLenum,GLenum,GLenum,GLuint,GLint);
    static Proc es1;
    GLESEGLTexture *texture=name ? gles_native_texture(name,false) : NULL;
    if (name && !texture) { gles_native_error(GL_INVALID_OPERATION); return; }
    GLenum object_target=textarget>=GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? GL_TEXTURE_CUBE_MAP : textarget;
    if (texture && texture->target!=object_target) { gles_native_error(GL_INVALID_OPERATION); return; }
    GLuint native=texture ? texture->native_name : 0;
    gles_native_save_errors();
    if (gles_egl_current->api==2) glFramebufferTexture2D(target,attachment,gles_native_target(textarget),native,level);
    else {
        if (!gles_native_require_fbo()) { gles_native_error(GL_INVALID_OPERATION); return; }
        if (!es1) es1=(Proc)eglGetProcAddress("glFramebufferTexture2DOES");
        if (!es1) { gles_native_error(GL_INVALID_OPERATION); return; }
        es1(target,attachment,gles_native_target(textarget),native,level);
    }
    GLenum error=gles_native_driver_error(); gles_native_error(error);
    if (!error) gles_native_attach_texture(attachment,texture);
}

/* Deletion detaches only the currently bound FBO. Other contexts and unbound
 * FBOs retain the old object even after its guest name is reused. */
static void gles_native_delete_bound_attachments(GLESEGLTexture *texture)
{
    GLint framebuffer; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    GLESNativeAttachment **slot=(GLESNativeAttachment **)&gles_egl_current->texture_attachments;
    while (*slot) {
        GLESNativeAttachment *record=*slot;
        if (record->texture!=texture || record->framebuffer!=(GLuint)framebuffer) { slot=&record->next; continue; }
        if (gles_egl_current->api==2) glFramebufferTexture2D(GL_FRAMEBUFFER,record->attachment,GL_TEXTURE_2D,0,0);
        else {
            typedef void (*Proc)(GLenum,GLenum,GLenum,GLuint,GLint);
            Proc detach=(Proc)eglGetProcAddress("glFramebufferTexture2DOES");
            if (detach) detach(GL_FRAMEBUFFER,record->attachment,GL_TEXTURE_2D,0,0);
        }
        *slot=record->next; texture->refs--; free(record);
    }
}

static void gles_native_GenRenderbuffers(GLsizei n, GLuint *names)
{
    typedef void (*Proc)(GLsizei n, GLuint *names);
    static Proc es1;
    if (gles_egl_current->api == 2) { glGenRenderbuffers(n,names); return; }
    if (!gles_native_require_fbo()) {
        if (n > 0 && names) memset(names,0,(size_t)n*sizeof(*names));
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glGenRenderbuffersOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glGenRenderbuffersOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(n,names);
}
static void gles_native_BindRenderbuffer(GLenum target, GLuint name)
{
    typedef void (*Proc)(GLenum target, GLuint name);
    static Proc es1;
    if (gles_egl_current->api == 2) { glBindRenderbuffer(target,name); return; }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glBindRenderbufferOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glBindRenderbufferOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(target,name);
}
static void gles_native_RenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h)
{
    typedef void (*Proc)(GLenum target, GLenum format, GLsizei w, GLsizei h);
    static Proc es1;
    if (gles_egl_current->api == 2) { glRenderbufferStorage(target,format,w,h); return; }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glRenderbufferStorageOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glRenderbufferStorageOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(target,format,w,h);
}
static void gles_native_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint name)
{
    typedef void (*Proc)(GLenum,GLenum,GLenum,GLuint);
    static Proc es1;
    gles_native_save_errors();
    if (gles_egl_current->api==2) glFramebufferRenderbuffer(target,attachment,rbtarget,name);
    else {
        if (!gles_native_require_fbo()) { gles_native_error(GL_INVALID_OPERATION); return; }
        if (!es1) es1=(Proc)eglGetProcAddress("glFramebufferRenderbufferOES");
        if (!es1) { gles_native_error(GL_INVALID_OPERATION); return; }
        es1(target,attachment,rbtarget,name);
    }
    GLenum error=gles_native_driver_error(); gles_native_error(error);
    if (!error) gles_native_attach_texture(attachment,NULL);
}

static GLenum gles_native_CheckFramebufferStatus(GLenum target)
{
    typedef GLenum (*Proc)(GLenum target);
    static Proc es1;
    if (gles_egl_current->api == 2) { return glCheckFramebufferStatus(target); }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return 0;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glCheckFramebufferStatusOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glCheckFramebufferStatusOES\n"); gles_native_error(GL_INVALID_OPERATION); return 0; }
    return es1(target);
}
static void gles_native_DeleteFramebuffers(GLsizei n, const GLuint *names)
{
    typedef void (*Proc)(GLsizei,const GLuint *);
    static Proc es1;
    gles_native_save_errors();
    if (gles_egl_current->api==2) glDeleteFramebuffers(n,names);
    else {
        if (!gles_native_require_fbo()) { gles_native_error(GL_INVALID_OPERATION); return; }
        if (!es1) es1=(Proc)eglGetProcAddress("glDeleteFramebuffersOES");
        if (!es1) { gles_native_error(GL_INVALID_OPERATION); return; }
        es1(n,names);
    }
    GLenum error=gles_native_driver_error(); gles_native_error(error);
    if (!error) gles_native_delete_texture_attachments(n,names);
}

static void gles_native_DeleteRenderbuffers(GLsizei n, const GLuint *names)
{
    typedef void (*Proc)(GLsizei n, const GLuint *names);
    static Proc es1;
    if (gles_egl_current->api == 2) { glDeleteRenderbuffers(n,names); return; }
    if (!gles_native_require_fbo()) {
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glDeleteRenderbuffersOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glDeleteRenderbuffersOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(n,names);
}
static void gles_native_GetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *value)
{
    typedef void (*Proc)(GLenum target, GLenum pname, GLint *value);
    static Proc es1;
    if (gles_egl_current->api == 2) { glGetRenderbufferParameteriv(target,pname,value); return; }
    if (!gles_native_require_fbo()) {
        if (value) *value=0;
        gles_native_error(GL_INVALID_OPERATION); return;
    }
    if (!es1) es1=(Proc)eglGetProcAddress("glGetRenderbufferParameterivOES");
    if (!es1) { fprintf(stderr,"[gles-egl] missing required glGetRenderbufferParameterivOES\n"); gles_native_error(GL_INVALID_OPERATION); return; }
    es1(target,pname,value);
}
static void gles_native_GetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *value)
{
    typedef void (*Proc)(GLenum,GLenum,GLenum,GLint *);
    static Proc es1;
    if (gles_egl_current->api==2) glGetFramebufferAttachmentParameteriv(target,attachment,pname,value);
    else {
        if (!gles_native_require_fbo()) { if(value)*value=0;gles_native_error(GL_INVALID_OPERATION);return; }
        if (!es1) es1=(Proc)eglGetProcAddress("glGetFramebufferAttachmentParameterivOES");
        if (!es1) { gles_native_error(GL_INVALID_OPERATION);return; }
        es1(target,attachment,pname,value);
    }
    if (pname==GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
        GLint type=0;
        if (gles_egl_current->api==2) glGetFramebufferAttachmentParameteriv(target,attachment,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&type);
        else es1(target,attachment,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&type);
        if (type==GL_TEXTURE) {
            GLESEGLTexture *texture=gles_native_texture_from_native(*value);
            if (texture) *value=texture->name;
        }
    }
}

#define glIsFramebufferOES gles_native_IsFramebuffer
#define glIsRenderbufferOES gles_native_IsRenderbuffer
#define glGenerateMipmapOES gles_native_GenerateMipmap
#define glGenFramebuffersOES gles_native_GenFramebuffers
#define glBindFramebufferOES gles_native_BindFramebuffer
#define glFramebufferTexture2DOES gles_native_FramebufferTexture2D
#define glGenRenderbuffersOES gles_native_GenRenderbuffers
#define glBindRenderbufferOES gles_native_BindRenderbuffer
#define glRenderbufferStorageOES gles_native_RenderbufferStorage
#define glFramebufferRenderbufferOES gles_native_FramebufferRenderbuffer
#define glCheckFramebufferStatusOES gles_native_CheckFramebufferStatus
#define glDeleteFramebuffersOES gles_native_DeleteFramebuffers
#define glDeleteRenderbuffersOES gles_native_DeleteRenderbuffers
#define glGetRenderbufferParameterivOES gles_native_GetRenderbufferParameteriv
#define glGetFramebufferAttachmentParameterivOES gles_native_GetFramebufferAttachmentParameteriv

#undef glIsFramebuffer
#define glIsFramebuffer gles_native_IsFramebuffer
#undef glIsRenderbuffer
#define glIsRenderbuffer gles_native_IsRenderbuffer
#undef glGenerateMipmap
#define glGenerateMipmap gles_native_GenerateMipmap
#undef glBlendEquation
#define glBlendEquation gles_native_BlendEquation
#undef glBlendEquationSeparate
#define glBlendEquationSeparate gles_native_BlendEquationSeparate
#undef glBlendFuncSeparate
#define glBlendFuncSeparate gles_native_BlendFuncSeparate
#undef glGenFramebuffers
#define glGenFramebuffers gles_native_GenFramebuffers
#undef glBindFramebuffer
#define glBindFramebuffer gles_native_BindFramebuffer
#undef glFramebufferTexture2D
#define glFramebufferTexture2D gles_native_FramebufferTexture2D
#undef glGenRenderbuffers
#define glGenRenderbuffers gles_native_GenRenderbuffers
#undef glBindRenderbuffer
#define glBindRenderbuffer gles_native_BindRenderbuffer
#undef glRenderbufferStorage
#define glRenderbufferStorage gles_native_RenderbufferStorage
#undef glFramebufferRenderbuffer
#define glFramebufferRenderbuffer gles_native_FramebufferRenderbuffer
#undef glCheckFramebufferStatus
#define glCheckFramebufferStatus gles_native_CheckFramebufferStatus
#undef glDeleteFramebuffers
#define glDeleteFramebuffers gles_native_DeleteFramebuffers
#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers gles_native_DeleteRenderbuffers
#undef glGetRenderbufferParameteriv
#define glGetRenderbufferParameteriv gles_native_GetRenderbufferParameteriv
#undef glGetFramebufferAttachmentParameteriv
#define glGetFramebufferAttachmentParameteriv gles_native_GetFramebufferAttachmentParameteriv

#undef glGenTextures
#define glGenTextures gles_native_gen_textures
#undef glIsTexture
#define glIsTexture gles_native_is_texture
#undef glBindTexture
#define glBindTexture gles_native_bind_texture
#undef glDeleteTextures
#define glDeleteTextures gles_native_delete_textures
#undef glGetIntegerv
#define glGetIntegerv gles_native_get_integer
#undef glGetFloatv
#define glGetFloatv gles_native_get_float
#undef glIsEnabled
#define glIsEnabled gles_native_is_enabled
#undef glEnable
#define glEnable gles_native_enable
#undef glDisable
#define glDisable gles_native_disable
#undef glTexParameteri
#define glTexParameteri gles_native_glTexParameteri
#undef glTexParameterf
#define glTexParameterf gles_native_glTexParameterf
#undef glTexParameteriv
#define glTexParameteriv gles_native_glTexParameteriv
#undef glTexParameterfv
#define glTexParameterfv gles_native_glTexParameterfv
#undef glGetTexParameteriv
#define glGetTexParameteriv gles_native_glGetTexParameteriv
#undef glGetTexParameterfv
#define glGetTexParameterfv gles_native_glGetTexParameterfv
#undef glTexEnvf
#define glTexEnvf gles_native_tex_envf
#undef glTexEnvfv
#define glTexEnvfv gles_native_tex_envfv
#undef glTexEnviv
#define glTexEnviv gles_native_tex_enviv
#undef glGetTexEnvfv
#define glGetTexEnvfv gles_native_get_tex_envfv
#undef glGetTexEnviv
#define glGetTexEnviv gles_native_get_tex_enviv
#undef glDrawArrays
#define glDrawArrays gles_native_draw_arrays
#undef glDrawElements
#define glDrawElements gles_native_draw_elements

/* DrawTex uses window coordinates, current color and per-unit crop rectangles.
 * Execute it with the real ES1 array pipeline rather than vendor DrawTex entry
 * points: some drivers advertise the extension but fail when drawing to FBOs.
 * Restore every state changed here, including client arrays and texture matrices.
 */
typedef struct GLESNativeArrayState {
    GLint size, type, stride, buffer;
    void *pointer;
    GLboolean enabled;
} GLESNativeArrayState;
static bool gles_native_draw_tex(GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat h,
                                 const GLint (*crop)[4], const bool *has_crop, unsigned crop_count)
{
    if (gles_egl_current->api != 1) { gles_native_error(GL_INVALID_OPERATION); return false; }
    if (w <= 0 || h <= 0) { gles_native_error(GL_INVALID_VALUE); return false; }
    enum { MAX_UNITS=8 };
    GLint viewport[4], active, client, mode, units, buffer, clips;
    GLfloat projection[16], modelview[16], matrices[MAX_UNITS][16], coords[MAX_UNITS][8];
    GLboolean cube[MAX_UNITS];
    GLESNativeArrayState vertex, texcoord[MAX_UNITS];
    const GLenum enables[]={GL_LIGHTING,GL_CULL_FACE,
        GL_CLIP_PLANE0,GL_CLIP_PLANE1,GL_CLIP_PLANE2,GL_CLIP_PLANE3,GL_CLIP_PLANE4,GL_CLIP_PLANE5};
    GLboolean enabled[sizeof(enables)/sizeof(enables[0])];
    gles_native_save_errors();
    glGetIntegerv(GL_VIEWPORT,viewport);
    if (viewport[2]<=0 || viewport[3]<=0) return true;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active); glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE,&client);
    glGetIntegerv(GL_MATRIX_MODE,&mode); glGetIntegerv(GL_MAX_TEXTURE_UNITS,&units);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&buffer); glGetIntegerv(GL_MAX_CLIP_PLANES,&clips);
    if (units>MAX_UNITS) units=MAX_UNITS;
    unsigned enable_count=2+(clips>6?6:clips);
    for (unsigned i=0;i<enable_count;i++) { enabled[i]=glIsEnabled(enables[i]); glDisable(enables[i]); }
    vertex.enabled=glIsEnabled(GL_VERTEX_ARRAY);
    glGetIntegerv(GL_VERTEX_ARRAY_SIZE,&vertex.size); glGetIntegerv(GL_VERTEX_ARRAY_TYPE,&vertex.type);
    glGetIntegerv(GL_VERTEX_ARRAY_STRIDE,&vertex.stride); glGetIntegerv(GL_VERTEX_ARRAY_BUFFER_BINDING,&vertex.buffer);
    glGetPointerv(GL_VERTEX_ARRAY_POINTER,&vertex.pointer);
    GLboolean color=glIsEnabled(GL_COLOR_ARRAY), normal=glIsEnabled(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
    glMatrixMode(GL_PROJECTION); glGetFloatv(GL_PROJECTION_MATRIX,projection); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glGetFloatv(GL_MODELVIEW_MATRIX,modelview); glLoadIdentity();
    for (unsigned unit=0;unit<(unsigned)units;unit++) {
        glActiveTexture(GL_TEXTURE0+unit); glClientActiveTexture(GL_TEXTURE0+unit);
        cube[unit]=glIsEnabled(GL_TEXTURE_CUBE_MAP); glDisable(GL_TEXTURE_CUBE_MAP);
        glMatrixMode(GL_TEXTURE); glGetFloatv(GL_TEXTURE_MATRIX,matrices[unit]); glLoadIdentity();
        GLESNativeArrayState *array=&texcoord[unit];
        array->enabled=glIsEnabled(GL_TEXTURE_COORD_ARRAY);
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE,&array->size); glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE,&array->type);
        glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE,&array->stride); glGetIntegerv(GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING,&array->buffer);
        glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER,&array->pointer);
        GLESEGLTexture *texture=gles_native_bound_texture(GL_TEXTURE_2D);
        GLfloat s0=0,s1=0,t0=0,t1=0;
        if (texture && texture->width && texture->height && unit<crop_count && has_crop[unit]) {
            s0=(GLfloat)crop[unit][0]/texture->width; s1=s0+(GLfloat)crop[unit][2]/texture->width;
            t0=(GLfloat)crop[unit][1]/texture->height; t1=t0+(GLfloat)crop[unit][3]/texture->height;
        }
        GLfloat *uv=coords[unit];
        uv[0]=s0;uv[1]=t0;uv[2]=s1;uv[3]=t0;uv[4]=s0;uv[5]=t1;uv[6]=s1;uv[7]=t1;
        glBindBuffer(GL_ARRAY_BUFFER,0); glTexCoordPointer(2,GL_FLOAT,0,uv); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    GLfloat left=2*(x-viewport[0])/viewport[2]-1, right=left+2*w/viewport[2];
    GLfloat bottom=2*(y-viewport[1])/viewport[3]-1, top=bottom+2*h/viewport[3];
    GLfloat depth=2*(z<0?0:z>1?1:z)-1;
    /* Eye distance zero preserves DrawTex fog. Projection alone sets depth,
     * which the normal DEPTH_RANGE then maps exactly as OES_draw_texture. */
    const GLfloat window_projection[]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,depth,1};
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(window_projection);
    const GLfloat vertices[]={left,bottom,0, right,bottom,0, left,top,0, right,top,0};
    glBindBuffer(GL_ARRAY_BUFFER,0); glVertexPointer(3,GL_FLOAT,0,vertices); glEnableClientState(GL_VERTEX_ARRAY);
    gles_egl_current->drawing_window_quad=true;
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    gles_egl_current->drawing_window_quad=false;
    for (unsigned unit=0;unit<(unsigned)units;unit++) {
        glActiveTexture(GL_TEXTURE0+unit); glClientActiveTexture(GL_TEXTURE0+unit);
        glMatrixMode(GL_TEXTURE); glLoadMatrixf(matrices[unit]);
        if (cube[unit]) glEnable(GL_TEXTURE_CUBE_MAP);
        GLESNativeArrayState *array=&texcoord[unit];
        glBindBuffer(GL_ARRAY_BUFFER,array->buffer); glTexCoordPointer(array->size,array->type,array->stride,array->pointer);
        if (!array->enabled) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glBindBuffer(GL_ARRAY_BUFFER,vertex.buffer); glVertexPointer(vertex.size,vertex.type,vertex.stride,vertex.pointer);
    if (!vertex.enabled) glDisableClientState(GL_VERTEX_ARRAY);
    if (color) glEnableClientState(GL_COLOR_ARRAY);
    if (normal) glEnableClientState(GL_NORMAL_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER,buffer);
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(projection); glMatrixMode(GL_MODELVIEW); glLoadMatrixf(modelview);
    glActiveTexture(active); glClientActiveTexture(client); glMatrixMode(mode);
    for (unsigned i=0;i<enable_count;i++) if (enabled[i]) glEnable(enables[i]);
    return true;
}
