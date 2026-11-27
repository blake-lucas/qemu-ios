/*
 * Saving and restoring live GL state (#included by gles-host.c: the desktop GL backends, and Android's native ES).
 *
 * The state lives in host GL contexts, which QEMU cannot serialize, so it is
 * read back through GL at save time and rebuilt through GL at load time:
 *
 *   - share groups and contexts keep their handles (the guest holds them);
 *   - per namespace (a share group, or a context with none): emulated buffer
 *     objects, textures with their pixels, renderbuffers (storage only: their
 *     contents come back cleared), framebuffer attachments, shaders and
 *     programs with attribute bindings and uniform values;
 *   - per context: the GLESHost fields that are guest state (client and
 *     attribute arrays, bindings, pixel store, drawable size, ...) and the host
 *     GL context's own state, fixed function included (see GLESCtxGL).
 *
 * Texture, buffer, framebuffer and renderbuffer names are bound back
 * directly, which legacy GL allows; shader and program names are the guest ids
 * of gles_glsl_* and get new host ids. The host-private drawable objects
 * (gh.fbo/tex/depth) are ordinary GL objects in the namespace and travel the
 * same way.
 *
 * Everything is one versioned blob in the "gles-host" section.
 */

#define SNAP_MAGIC   0x534c4731u     /* "1GLS" */
#define SNAP_VERSION 8                /* 8: the iPhone 5's GL state (buffer usage and maps, drawable renderbuffers, shader clip planes, queries, texture storage levels); 7: vertex array objects; 6: OES_matrix_palette; 5: surfaces' page lists; 4: the guest's GLES API; 3 to 5 load without it */
#define SNAP_SCAN_GAP 256            /* names probed past the last live one */

static uint32_t gles_group_handle(GLESGroup *group);

/*
 * The native ES backend (GLES_HOST_EGL_ES) runs each guest context in a host context of its own API: an ES1 one has
 * no programs, an ES2 one no fixed function, and on Android a call outside the context's API jumps to address 0
 * (gles_native_BlendEquation). snap_fixed and snap_shaders say which state the current context has; a desktop
 * compatibility context has both. ES also reads no texture back: see snap_put_textures_es.
 */
#ifdef GLES_HOST_EGL_ES
static bool snap_fixed(void) { return gh.api_version != 2; }
static bool snap_shaders(void) { return gh.api_version == 2; }
#ifndef GL_SOURCE0_RGB
#define GL_SOURCE0_RGB GL_SRC0_RGB
#define GL_SOURCE0_ALPHA GL_SRC0_ALPHA
#endif
#ifndef GL_POINT_SPRITE
#define GL_POINT_SPRITE GL_POINT_SPRITE_OES
#define GL_COORD_REPLACE GL_COORD_REPLACE_OES
#endif
#ifndef GL_RENDERBUFFER_INTERNAL_FORMAT_EXT
#define GL_RENDERBUFFER_INTERNAL_FORMAT_EXT GL_RENDERBUFFER_INTERNAL_FORMAT
#endif
#ifndef GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_EXT
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_EXT GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL
#endif
#else
static bool snap_fixed(void) { return true; }
static bool snap_shaders(void) { return true; }
#endif

typedef struct { GByteArray *b; } SnapW;
typedef struct { const uint8_t *p; size_t n, off; bool bad; } SnapR;

static void sw32(SnapW *w, uint32_t v) { g_byte_array_append(w->b, (uint8_t *)&v, 4); }
static void swbytes(SnapW *w, const void *p, uint32_t n)
{
    sw32(w, n);
    if (n) g_byte_array_append(w->b, p, n);
}
static uint32_t sr32(SnapR *r)
{
    uint32_t v = 0;
    if (r->off + 4 > r->n) { r->bad = true; return 0; }
    memcpy(&v, r->p + r->off, 4);
    r->off += 4;
    return v;
}
/* A length-prefixed run; NULL (and r->bad) if it overruns the blob. */
static const uint8_t *srbytes(SnapR *r, uint32_t *n)
{
    const uint8_t *p;
    *n = sr32(r);
    if (r->bad || r->off + *n > r->n) { r->bad = true; return NULL; }
    p = r->p + r->off;
    r->off += *n;
    return p;
}

/* ---- per-context GL state ------------------------------------------------ */

static const GLenum snap_enables[] = {
    GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
    GL_DITHER, GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE,
    GL_SAMPLE_COVERAGE, GL_ALPHA_TEST, GL_FOG, GL_LIGHTING, GL_LIGHT0, GL_LIGHT1,
    GL_LIGHT2, GL_LIGHT3, GL_LIGHT4, GL_LIGHT5, GL_LIGHT6, GL_LIGHT7,
    GL_COLOR_MATERIAL, GL_NORMALIZE, GL_RESCALE_NORMAL, GL_POINT_SMOOTH,
    GL_LINE_SMOOTH, GL_MULTISAMPLE, GL_COLOR_LOGIC_OP, GL_CLIP_PLANE0,
    GL_CLIP_PLANE1, GL_CLIP_PLANE2, GL_CLIP_PLANE3, GL_CLIP_PLANE4,
    GL_CLIP_PLANE5, GL_POINT_SPRITE,
};
static const GLenum snap_hints[] = {
    GL_PERSPECTIVE_CORRECTION_HINT, GL_POINT_SMOOTH_HINT, GL_LINE_SMOOTH_HINT,
    GL_FOG_HINT, GL_GENERATE_MIPMAP_HINT,
};

#define SNAP_MV_DEPTH  32
#define SNAP_AUX_DEPTH 4

typedef struct {
    GLfloat amb[4], dif[4], spec[4], pos[4], spot_dir[4];
    GLfloat spot_exp, spot_cut, att[3];
} SnapLight;

typedef struct {
    GLint mode, combine_rgb, combine_a, src_rgb[3], src_a[3], op_rgb[3], op_a[3];
    GLfloat color[4], rgb_scale, a_scale, cur_tex[4];
    GLint coord_replace, tex2d, rect, bind2d, bindrect;
    GLint mat_depth;
    GLfloat mat[SNAP_AUX_DEPTH][16];
} SnapUnit;

/* The host context's own state. Written as raw bytes: the layout is this
 * build's, and SNAP_VERSION plus sizeof() guard a mismatch. */
typedef struct {
    GLint viewport[4], scissor[4];
    GLfloat depth_range[2], clear_color[4], clear_depth;
    GLint clear_stencil;
    GLboolean color_mask[4], depth_mask, sample_inv, pad0[2];
    GLint stencil_wm, stencil_back_wm;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint blend_eq_rgb, blend_eq_a;
    GLfloat blend_color[4];
    GLint depth_func, cull_mode, front_face;
    GLint st[2][6];     /* front/back: func, ref, value mask, fail, zfail, zpass */
    GLfloat poly_factor, poly_units, line_width, point_size, alpha_ref, sample_cov;
    GLint alpha_func, shade_model, logic_op;
    GLint hints[ARRAY_SIZE(snap_hints)];
    GLboolean enables[ARRAY_SIZE(snap_enables)];
    GLint mv_depth, proj_depth;
    GLfloat mv[SNAP_MV_DEPTH][16], proj[SNAP_AUX_DEPTH][16];
    SnapLight light[8];
    GLfloat lm_amb[4];
    GLint lm_two;
    GLfloat mat_amb[4], mat_dif[4], mat_spec[4], mat_emi[4], mat_shin;
    GLint fog_mode;
    GLfloat fog_density, fog_start, fog_end, fog_color[4];
    GLfloat cur_color[4], cur_normal[4];
    GLfloat pt_min, pt_max, pt_att[3], pt_fade;
    GLfloat clip[6][4];
    GLint units;
    SnapUnit unit[GLES_MAX_TEXUNITS];
    GLint active_tex, fbo, rb, program;
    /* What the context has (snap_limits): only those caps, lights and clip planes are read and set, as a driver
     * need not check a name past its limits. */
    GLint max_lights, max_clip, point_sprite;
    GLboolean enable_ok[ARRAY_SIZE(snap_enables)];
    GLfloat vattr[GLES_MAX_ATTRIBS][4];
} GLESCtxGL;

/* Every matrix of the stack for `mode`, bottom first. Only the top of a stack
 * can be read, so it is popped down to the bottom and pushed back. */
static void snap_get_stack(GLenum mode, GLenum depth_pname, GLenum mat_pname,
                           GLint *depth, GLfloat (*m)[16], int cap)
{
    GLint d = 1;
    glMatrixMode(mode);
    glGetIntegerv(depth_pname, &d);
    d = MIN(MAX(d, 1), cap);
    *depth = d;
    for (int i = d - 1; i >= 0; i--) {
        glGetFloatv(mat_pname, m[i]);
        if (i) glPopMatrix();
    }
    for (int i = 1; i < d; i++) {
        glPushMatrix();
        glLoadMatrixf(m[i]);
    }
}

static void snap_set_stack(GLenum mode, GLenum depth_pname, GLint depth, GLfloat (*m)[16])
{
    GLint d = 1;
    glMatrixMode(mode);
    glGetIntegerv(depth_pname, &d);
    while (d-- > 1) glPopMatrix();
    glLoadMatrixf(m[0]);
    for (int i = 1; i < depth; i++) {
        glPushMatrix();
        glLoadMatrixf(m[i]);
    }
}

static void snap_capture_fixed(GLESCtxGL *s);

/*
 * The GL errors a save or load step left, dropped (they are not the guest's). LT_STATE_GL_LOG=1 logs them by step:
 * a call a context refuses here is one a stricter driver may not survive, so a clean log on Mesa, Adreno and ANGLE is
 * the check that every call fits its context.
 */
static void snap_drain(const char *where)
{
    static int log = -1;

    if (log < 0) log = getenv("LT_STATE_GL_LOG") != NULL;
    for (GLenum e; (e = glGetError()) != GL_NO_ERROR;) {
        if (log) fprintf(stderr, "[gles] snapshot: GL error 0x%x after %s (api %u)\n", e, where, gh.api_version);
    }
}

/* The context's lights, clip planes and point sprites; an ES2 context has none. */
static void snap_limits(GLESCtxGL *s)
{
    s->max_lights = s->max_clip = s->point_sprite = 0;
    if (!snap_fixed()) return;
    glGetIntegerv(GL_MAX_LIGHTS, &s->max_lights);
    glGetIntegerv(GL_MAX_CLIP_PLANES, &s->max_clip);
    s->max_lights = MIN(MAX(s->max_lights, 0), 8);
    s->max_clip = MIN(MAX(s->max_clip, 0), 6);
#ifdef GLES_HOST_EGL_ES
    s->point_sprite = gles_native_extension((const char *)glGetString(GL_EXTENSIONS), "GL_OES_point_sprite");
#else
    s->point_sprite = 1;
#endif
}

/* Whether the context has @cap: an ES2 context only the per-fragment and rasterizer ones. */
static bool snap_cap_ok(const GLESCtxGL *s, GLenum cap)
{
    if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) return cap - GL_LIGHT0 < (GLuint)s->max_lights;
    if (cap >= GL_CLIP_PLANE0 && cap <= GL_CLIP_PLANE5) return cap - GL_CLIP_PLANE0 < (GLuint)s->max_clip;
    if (cap == GL_POINT_SPRITE) return s->point_sprite;
    if (snap_fixed()) return true;
    switch (cap) {
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_STENCIL_TEST: case GL_SCISSOR_TEST:
    case GL_DITHER: case GL_POLYGON_OFFSET_FILL: case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE:
        return true;
    default:
        return false;
    }
}

static void snap_capture_gl(GLESCtxGL *s)
{
    memset(s, 0, sizeof(*s));
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
    glGetFloatv(GL_DEPTH_RANGE, s->depth_range);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clear_depth);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &s->clear_stencil);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_SAMPLE_COVERAGE_INVERT, &s->sample_inv);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s->stencil_wm);
    s->stencil_back_wm = s->stencil_wm;
    if (snap_shaders()) glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &s->stencil_back_wm);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_a);
    if (snap_shaders()) glGetFloatv(GL_BLEND_COLOR, s->blend_color);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    glGetIntegerv(GL_CULL_FACE_MODE, &s->cull_mode);
    glGetIntegerv(GL_FRONT_FACE, &s->front_face);
    {
        static const GLenum f[6] = { GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK,
                                     GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS };
        static const GLenum b[6] = { GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK,
                                     GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                                     GL_STENCIL_BACK_PASS_DEPTH_PASS };
        for (int i = 0; i < 6; i++) {
            glGetIntegerv(f[i], &s->st[0][i]);
            s->st[1][i] = s->st[0][i];
            if (snap_shaders()) glGetIntegerv(b[i], &s->st[1][i]);
        }
    }
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s->poly_factor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s->poly_units);
    glGetFloatv(GL_LINE_WIDTH, &s->line_width);
    glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, &s->sample_cov);
    snap_drain("capture: common state");
    snap_limits(s);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_enables); i++) {
        s->enable_ok[i] = snap_cap_ok(s, snap_enables[i]);
        if (s->enable_ok[i]) s->enables[i] = glIsEnabled(snap_enables[i]);
    }
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_tex);
    if (snap_fixed()) {
        snap_capture_fixed(s);
    } else {
        glGetIntegerv(GL_GENERATE_MIPMAP_HINT, &s->hints[ARRAY_SIZE(snap_hints) - 1]);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &s->units);
        s->units = MIN(s->units, (GLint)GLES_MAX_TEXUNITS);
        s->mv_depth = s->proj_depth = 1;    /* no stacks, but snap_gl_ok's bounds */
        for (int u = 0; u < s->units; u++) {
            glActiveTexture(GL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->unit[u].bind2d);
            s->unit[u].mat_depth = 1;
        }
        glActiveTexture(s->active_tex);
    }
    snap_drain("capture: enables, units and fixed function");
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &s->fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &s->rb);
    if (snap_shaders()) {
        glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
        for (unsigned i = 1; i < GLES_MAX_ATTRIBS; i++) {
            glGetVertexAttribfv(i, GL_CURRENT_VERTEX_ATTRIB, s->vattr[i]);
        }
    }
    snap_drain("capture: bindings and program");
}

/* The fixed-function state, with the texture units' environments and the matrix stacks. */
static void snap_capture_fixed(GLESCtxGL *s)
{
    GLint mode = GL_MODELVIEW;
    glGetFloatv(GL_POINT_SIZE, &s->point_size);
    glGetIntegerv(GL_ALPHA_TEST_FUNC, &s->alpha_func);
    glGetFloatv(GL_ALPHA_TEST_REF, &s->alpha_ref);
    glGetIntegerv(GL_SHADE_MODEL, &s->shade_model);
    glGetIntegerv(GL_LOGIC_OP_MODE, &s->logic_op);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_hints); i++) glGetIntegerv(snap_hints[i], &s->hints[i]);

    for (int i = 0; i < s->max_lights; i++) {
        SnapLight *l = &s->light[i];
        GLenum n = GL_LIGHT0 + i;
        glGetLightfv(n, GL_AMBIENT, l->amb);
        glGetLightfv(n, GL_DIFFUSE, l->dif);
        glGetLightfv(n, GL_SPECULAR, l->spec);
        glGetLightfv(n, GL_POSITION, l->pos);
        glGetLightfv(n, GL_SPOT_DIRECTION, l->spot_dir);
        glGetLightfv(n, GL_SPOT_EXPONENT, &l->spot_exp);
        glGetLightfv(n, GL_SPOT_CUTOFF, &l->spot_cut);
        glGetLightfv(n, GL_CONSTANT_ATTENUATION, &l->att[0]);
        glGetLightfv(n, GL_LINEAR_ATTENUATION, &l->att[1]);
        glGetLightfv(n, GL_QUADRATIC_ATTENUATION, &l->att[2]);
    }
    glGetFloatv(GL_LIGHT_MODEL_AMBIENT, s->lm_amb);
    glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &s->lm_two);
    glGetMaterialfv(GL_FRONT, GL_AMBIENT, s->mat_amb);
    glGetMaterialfv(GL_FRONT, GL_DIFFUSE, s->mat_dif);
    glGetMaterialfv(GL_FRONT, GL_SPECULAR, s->mat_spec);
    glGetMaterialfv(GL_FRONT, GL_EMISSION, s->mat_emi);
    glGetMaterialfv(GL_FRONT, GL_SHININESS, &s->mat_shin);
    glGetIntegerv(GL_FOG_MODE, &s->fog_mode);
    glGetFloatv(GL_FOG_DENSITY, &s->fog_density);
    glGetFloatv(GL_FOG_START, &s->fog_start);
    glGetFloatv(GL_FOG_END, &s->fog_end);
    glGetFloatv(GL_FOG_COLOR, s->fog_color);
    glGetFloatv(GL_CURRENT_COLOR, s->cur_color);
    glGetFloatv(GL_CURRENT_NORMAL, s->cur_normal);
    glGetFloatv(GL_POINT_SIZE_MIN, &s->pt_min);
    glGetFloatv(GL_POINT_SIZE_MAX, &s->pt_max);
    glGetFloatv(GL_POINT_DISTANCE_ATTENUATION, s->pt_att);
    glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, &s->pt_fade);
    for (int i = 0; i < s->max_clip; i++) {
        double eq[4] = { 0 };
        glGetClipPlane(GL_CLIP_PLANE0 + i, eq);
        for (int k = 0; k < 4; k++) s->clip[i][k] = eq[k];
    }

    glGetIntegerv(GL_MATRIX_MODE, &mode);
    snap_get_stack(GL_MODELVIEW, GL_MODELVIEW_STACK_DEPTH, GL_MODELVIEW_MATRIX,
                   &s->mv_depth, s->mv, SNAP_MV_DEPTH);
    snap_get_stack(GL_PROJECTION, GL_PROJECTION_STACK_DEPTH, GL_PROJECTION_MATRIX,
                   &s->proj_depth, s->proj, SNAP_AUX_DEPTH);

    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &s->units);
    s->units = MIN(s->units, (GLint)GLES_MAX_TEXUNITS);
    for (int u = 0; u < s->units; u++) {
        SnapUnit *t = &s->unit[u];
        glActiveTexture(GL_TEXTURE0 + u);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &t->mode);
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, t->color);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &t->combine_rgb);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, &t->combine_a);
        for (int k = 0; k < 3; k++) {
            glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE0_RGB + k, &t->src_rgb[k]);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + k, &t->src_a[k]);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_RGB + k, &t->op_rgb[k]);
            glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + k, &t->op_a[k]);
        }
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, &t->rgb_scale);
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, &t->a_scale);
        if (s->point_sprite) glGetTexEnviv(GL_POINT_SPRITE, GL_COORD_REPLACE, &t->coord_replace);
        glGetFloatv(GL_CURRENT_TEXTURE_COORDS, t->cur_tex);
        t->tex2d = glIsEnabled(GL_TEXTURE_2D);
        t->rect = glIsEnabled(GL_TEXTURE_RECTANGLE_ARB);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &t->bind2d);
        glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &t->bindrect);
        snap_get_stack(GL_TEXTURE, GL_TEXTURE_STACK_DEPTH, GL_TEXTURE_MATRIX,
                       &t->mat_depth, t->mat, SNAP_AUX_DEPTH);
    }
    glActiveTexture(s->active_tex);
    glMatrixMode(mode);
}

/* The counts snap_apply_gl indexes its arrays with, as snap_capture_gl bounds them. */
static bool snap_gl_ok(const GLESCtxGL *s)
{
    if (s->units < 0 || s->units > (GLint)GLES_MAX_TEXUNITS || s->max_lights < 0 || s->max_lights > 8 ||
        s->max_clip < 0 || s->max_clip > 6 ||
        s->mv_depth < 1 || s->mv_depth > SNAP_MV_DEPTH ||
        s->proj_depth < 1 || s->proj_depth > SNAP_AUX_DEPTH) {
        return false;
    }
    for (int u = 0; u < s->units; u++) {
        if (s->unit[u].mat_depth < 1 || s->unit[u].mat_depth > SNAP_AUX_DEPTH) {
            return false;
        }
    }
    return true;
}

static void snap_set_enable(GLenum cap, GLboolean on)
{
    if (on) glEnable(cap); else glDisable(cap);
}

static void snap_apply_fixed(const GLESCtxGL *s);

/* loading: the saved program name is the old host one, which may name another program or a shader now; the
 * program is bound by its guest name once every context is back (snap_use_program). */
static void snap_apply_gl(const GLESCtxGL *s, bool loading)
{
    /*
     * The context's own framebuffer first: the object walk left the last framebuffer it visited bound, and state
     * set over a framebuffer the driver didn't expect can be fatal. PowerVR's ES1 glDisable(GL_DEPTH_TEST) over a
     * color-only one dereferenced null on the Pixel 10.
     */
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, s->fbo);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, s->rb);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
    glDepthRange(s->depth_range[0], s->depth_range[1]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    glClearDepth(s->clear_depth);
    glClearStencil(s->clear_stencil);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glDepthMask(s->depth_mask);
    glBlendFuncSeparate(s->blend_src_rgb, s->blend_dst_rgb, s->blend_src_a, s->blend_dst_a);
    glBlendEquationSeparate(s->blend_eq_rgb, s->blend_eq_a);
    if (snap_shaders()) {
        glStencilMaskSeparate(GL_FRONT, s->stencil_wm);
        glStencilMaskSeparate(GL_BACK, s->stencil_back_wm);
        glBlendColor(s->blend_color[0], s->blend_color[1], s->blend_color[2], s->blend_color[3]);
        for (int f = 0; f < 2; f++) {
            GLenum face = f ? GL_BACK : GL_FRONT;
            glStencilFuncSeparate(face, s->st[f][0], s->st[f][1], s->st[f][2]);
            glStencilOpSeparate(face, s->st[f][3], s->st[f][4], s->st[f][5]);
        }
    } else {
        glStencilMask(s->stencil_wm);
        glStencilFunc(s->st[0][0], s->st[0][1], s->st[0][2]);
        glStencilOp(s->st[0][3], s->st[0][4], s->st[0][5]);
    }
    glDepthFunc(s->depth_func);
    glCullFace(s->cull_mode);
    glFrontFace(s->front_face);
    glPolygonOffset(s->poly_factor, s->poly_units);
    glLineWidth(s->line_width);
    glSampleCoverage(s->sample_cov, s->sample_inv);
    snap_drain("apply: common state");
    for (unsigned i = 0; i < ARRAY_SIZE(snap_enables); i++) {
        if (s->enable_ok[i]) snap_set_enable(snap_enables[i], s->enables[i]);
    }
    snap_drain("apply: enables");
    if (snap_fixed()) {
        snap_apply_fixed(s);
    } else {
        glHint(GL_GENERATE_MIPMAP_HINT, s->hints[ARRAY_SIZE(snap_hints) - 1]);
        for (int u = 0; u < s->units; u++) {
            glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, s->unit[u].bind2d);
        }
    }
    glActiveTexture(s->active_tex ? s->active_tex : GL_TEXTURE0);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, s->fbo);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, s->rb);
    if (snap_shaders()) {
        if (!loading) glUseProgram(s->program);
        for (unsigned i = 1; i < GLES_MAX_ATTRIBS; i++) glVertexAttrib4fv(i, s->vattr[i]);
    }
    snap_drain("apply: units, bindings and program");
}

static void snap_apply_fixed(const GLESCtxGL *s)
{
    glPointSize(s->point_size);
    glAlphaFunc(s->alpha_func, s->alpha_ref);
    glShadeModel(s->shade_model);
    glLogicOp(s->logic_op);
    for (unsigned i = 0; i < ARRAY_SIZE(snap_hints); i++) glHint(snap_hints[i], s->hints[i]);

    /* Positions, spot directions and clip planes were read in eye space; set
     * them under an identity modelview so they come back unchanged. */
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    for (int i = 0; i < s->max_lights; i++) {
        const SnapLight *l = &s->light[i];
        GLenum n = GL_LIGHT0 + i;
        glLightfv(n, GL_AMBIENT, l->amb);
        glLightfv(n, GL_DIFFUSE, l->dif);
        glLightfv(n, GL_SPECULAR, l->spec);
        glLightfv(n, GL_POSITION, l->pos);
        glLightfv(n, GL_SPOT_DIRECTION, l->spot_dir);
        glLightf(n, GL_SPOT_EXPONENT, l->spot_exp);
        glLightf(n, GL_SPOT_CUTOFF, l->spot_cut);
        glLightf(n, GL_CONSTANT_ATTENUATION, l->att[0]);
        glLightf(n, GL_LINEAR_ATTENUATION, l->att[1]);
        glLightf(n, GL_QUADRATIC_ATTENUATION, l->att[2]);
    }
    for (int i = 0; i < s->max_clip; i++) {
        double eq[4] = { s->clip[i][0], s->clip[i][1], s->clip[i][2], s->clip[i][3] };
        glClipPlane(GL_CLIP_PLANE0 + i, eq);
    }
    glPopMatrix();
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, s->lm_amb);
    glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, (GLfloat)s->lm_two);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, s->mat_amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, s->mat_dif);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, s->mat_spec);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, s->mat_emi);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, s->mat_shin);
    glFogf(GL_FOG_MODE, (GLfloat)s->fog_mode);
    glFogf(GL_FOG_DENSITY, s->fog_density);
    glFogf(GL_FOG_START, s->fog_start);
    glFogf(GL_FOG_END, s->fog_end);
    glFogfv(GL_FOG_COLOR, s->fog_color);
    glColor4f(s->cur_color[0], s->cur_color[1], s->cur_color[2], s->cur_color[3]);
    glNormal3f(s->cur_normal[0], s->cur_normal[1], s->cur_normal[2]);
    glPointParameterf(GL_POINT_SIZE_MIN, s->pt_min);
    glPointParameterf(GL_POINT_SIZE_MAX, s->pt_max);
    glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, s->pt_att);
    glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, s->pt_fade);

    for (int u = 0; u < s->units; u++) {
        const SnapUnit *t = &s->unit[u];
        glActiveTexture(GL_TEXTURE0 + u);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, t->mode);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, t->color);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, t->combine_rgb);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, t->combine_a);
        for (int k = 0; k < 3; k++) {
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB + k, t->src_rgb[k]);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA + k, t->src_a[k]);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB + k, t->op_rgb[k]);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + k, t->op_a[k]);
        }
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, t->rgb_scale);
        glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, t->a_scale);
        if (s->point_sprite) glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, t->coord_replace);
        glMultiTexCoord4f(GL_TEXTURE0 + u, t->cur_tex[0], t->cur_tex[1], t->cur_tex[2], t->cur_tex[3]);
        snap_set_enable(GL_TEXTURE_2D, t->tex2d);
        snap_set_enable(GL_TEXTURE_RECTANGLE_ARB, t->rect);
        glBindTexture(GL_TEXTURE_2D, t->bind2d);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, t->bindrect);
        snap_set_stack(GL_TEXTURE, GL_TEXTURE_STACK_DEPTH, t->mat_depth, (GLfloat (*)[16])t->mat);
    }
    snap_set_stack(GL_PROJECTION, GL_PROJECTION_STACK_DEPTH, s->proj_depth, (GLfloat (*)[16])s->proj);
    snap_set_stack(GL_MODELVIEW, GL_MODELVIEW_STACK_DEPTH, s->mv_depth, (GLfloat (*)[16])s->mv);
}

/* ---- per-namespace objects ------------------------------------------------ */

static void snap_put_map(SnapW *w, GHashTable *t, size_t value_size)
{
    GHashTableIter it;
    gpointer k, v;
    sw32(w, t ? g_hash_table_size(t) : 0);
    if (!t) return;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        sw32(w, GPOINTER_TO_UINT(k));
        if (value_size) swbytes(w, v, value_size); else sw32(w, GPOINTER_TO_UINT(v));
    }
}

/* value_size 0: the value is the pointer-sized integer itself. A value is
 * allocated as `alloc` bytes (>= value_size), the rest zeroed. */
static GHashTable *snap_get_map(SnapR *r, GHashTable *t, size_t value_size, size_t alloc)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t k = sr32(r), len;
        if (value_size) {
            const uint8_t *p = srbytes(r, &len);
            if (p && len == value_size) {
                void *v = g_malloc0(MAX(alloc, len));
                memcpy(v, p, len);
                g_hash_table_replace(t, GUINT_TO_POINTER(k), v);
            }
        } else {
            g_hash_table_replace(t, GUINT_TO_POINTER(k), GUINT_TO_POINTER(sr32(r)));
        }
    }
    return t;
}

static void snap_put_buffers(SnapW *w, GHashTable *t)
{
    GHashTableIter it;
    gpointer k, v;
    sw32(w, t ? g_hash_table_size(t) : 0);
    if (!t) return;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESBuffer *b = v;
        sw32(w, GPOINTER_TO_UINT(k));
        swbytes(w, b->data, b->size);
        sw32(w, b->usage); sw32(w, b->map_va); sw32(w, b->map_size);
        sw32(w, b->map_access); sw32(w, b->map_offset); sw32(w, b->map_range);
    }
}

static void snap_get_buffers(SnapR *r, GHashTable *t, uint32_t version)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t name = sr32(r), len;
        const uint8_t *p = srbytes(r, &len);
        GLESBuffer *b;
        if (!p || len > GLES_MAX_BUFFER_BYTES || !gles_budget_ok(0, len)) {
            r->bad = true;
            break;
        }
        b = g_new0(GLESBuffer, 1);
        b->name = name;
        b->refs = 1;
        b->size = len;
        b->data = len ? g_memdup2(p, len) : NULL;
        if (version >= 8) {
            b->usage = sr32(r); b->map_va = sr32(r); b->map_size = sr32(r);
            b->map_access = sr32(r); b->map_offset = sr32(r); b->map_range = sr32(r) != 0;
            if (b->map_offset > len || b->map_size > len - b->map_offset ||
                (b->map_va && (uint64_t)b->map_va + b->map_size > UINT32_MAX)) {
                g_free(b->data); g_free(b); r->bad = true; break;
            }
        }
        gles_buffer_bytes += len;
        g_hash_table_replace(t, GUINT_TO_POINTER(name), b);
    }
}

static GLESBuffer *snap_buffer(GHashTable *t, uint32_t name)
{
    GLESBuffer *b = name && t ? g_hash_table_lookup(t, GUINT_TO_POINTER(name)) : NULL;
    if (b) b->refs++;
    return b;
}

/* Probe the dense guest names from 1, then the host-private range. */
static bool snap_scan(GLuint *name, GLuint *gap)
{
    if (*gap >= SNAP_SCAN_GAP && *name < GLES_PRIVATE_NAME) {
        *name = GLES_PRIVATE_NAME;
        *gap = 0;
    }
    return *gap < SNAP_SCAN_GAP;
}

#ifdef GLES_HOST_EGL_ES
/*
 * ES has no glGetTexImage and no level queries, so the native layer keeps each level's size and format
 * (GLESEGLLevel). An RGB or RGBA level is read through a framebuffer and comes back as RGBA bytes; the layer kept
 * the bytes of the others; a level glGenerateMipmap made is made again, as is one a framebuffer can't read (ES 2.0
 * attaches level 0 only without OES_fbo_render_mipmap). Cube maps aren't kept (nor on the desktop).
 */
static bool snap_es_read_level(GLenum target, GLuint name, GLint level, GLsizei w, GLsizei h, uint8_t *px)
{
    GLuint fb = 0;
    GLint old = 0, pack = 4;
    GLenum status;

    while (glGetError() != GL_NO_ERROR) {
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &old);
    glGenFramebuffersEXT(1, &fb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, target, name, level);
    status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (status == GL_FRAMEBUFFER_COMPLETE_EXT) {
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        if (glGetError() != GL_NO_ERROR) status = 0;
    }
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, target, 0, 0);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, old);
    glDeleteFramebuffersEXT(1, &fb);
    snap_drain("texture readback");
    return status == GL_FRAMEBUFFER_COMPLETE_EXT;
}

/* A drawable's color texture held in an AHardwareBuffer had no upload to record: its size is the drawable's. */
static bool snap_es_drawable_level(GLuint name, GLESEGLLevel *l)
{
    GHashTableIter it;
    gpointer v;
    GLESHost *h = &gh_legacy;

    if (gles_contexts) g_hash_table_iter_init(&it, gles_contexts);
    for (;;) {
        if (h->inited && h->cgl && h->cgl->textures == gles_egl_current->textures && h->tex == name &&
            h->drawable_width && h->drawable_height) {
            *l = (GLESEGLLevel){ h->drawable_width, h->drawable_height, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE };
            return true;
        }
        if (!gles_contexts || !g_hash_table_iter_next(&it, NULL, &v)) return false;
        h = v;
    }
}

static void snap_put_textures(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW tw = { body };
    uint32_t count = 0;
    GArray *names = g_array_new(false, false, sizeof(GLuint));
    static bool cube_said;

    sw32(w, gles_egl_current->textures->next_name);
    /* Names first: binding one can free another the layer was keeping only for a binding. */
    for (GLESEGLTexture *t = gles_egl_current->textures->head; t; t = t->next) {
        if (t->deleted || t->hidden || !t->name) continue;
        if (t->target == GL_TEXTURE_2D || t->target == GL_TEXTURE_RECTANGLE_ARB) {
            g_array_append_val(names, t->name);
        } else if (t->target && !cube_said) {
            cube_said = true;
            fprintf(stderr, "[gles] snapshot: texture %u has an unsupported target 0x%x; dropped\n", t->name,
                    t->target);
        }
    }
    for (guint i = 0; i < names->len; i++) {
        static const GLenum p[] = { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
                                    GL_TEXTURE_WRAP_T, GL_GENERATE_MIPMAP, GL_TEXTURE_MAX_LEVEL };
        GLuint name = g_array_index(names, GLuint, i);
        GLESEGLTexture *t = gles_native_texture(name, false);
        GLenum target;

        if (!t) continue;
        target = t->target;
        glBindTexture(target, name);
        count++;
        sw32(&tw, name);
        sw32(&tw, target);
        for (unsigned k = 0; k < ARRAY_SIZE(p); k++) {
            GLint v = INT32_MIN; /* a parameter this context doesn't have stays as it is on load */
            if (k < 4 || (k == 4 && snap_fixed())) glGetTexParameteriv(target, p[k], &v);
            sw32(&tw, v);
        }
        snap_drain("texture parameters");
        for (GLint lvl = 0; lvl < GLES_EGL_LEVELS; lvl++) {
            GLESEGLLevel l;
            bool generated;
            g_autofree uint8_t *px = NULL;
            const void *bytes = NULL;
            uint32_t len = 0;

            t = gles_native_texture(name, false);
            if (!t) break;
            l = t->level[lvl];
            generated = l.generated;
            if (!l.width && !(lvl == 0 && snap_es_drawable_level(name, &l))) continue;
            if (gles_native_level_depth(l.format)) {
                /* storage only, as a renderbuffer's */
            } else if (!generated && l.shadow) {
                bytes = l.shadow;
                len = (uint32_t)((size_t)l.width * l.height * gles_native_level_bpp(l.format, l.type));
            } else if (!generated) {
                px = g_malloc((size_t)l.width * l.height * 4);
                if (snap_es_read_level(target, name, lvl, l.width, l.height, px)) {
                    bytes = px;
                    len = l.width * l.height * 4;
                    l.internal = l.format = GL_RGBA;
                    l.type = GL_UNSIGNED_BYTE;
                } else if (lvl > 0) {
                    generated = true;
                } else {
                    fprintf(stderr, "[gles] snapshot: texture %u (0x%x/0x%x) can't be read; dropped\n", name,
                            l.format, l.type);
                    continue;
                }
            }
            sw32(&tw, lvl + 1);
            sw32(&tw, l.width);
            sw32(&tw, l.height);
            sw32(&tw, l.internal);
            sw32(&tw, l.format);
            sw32(&tw, l.type);
            sw32(&tw, generated);
            swbytes(&tw, bytes, len);
        }
        sw32(&tw, 0);
    }
    g_array_free(names, true);
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

static void snap_get_textures(SnapR *r)
{
    uint32_t next = sr32(r), n = sr32(r);

    gles_egl_current->textures->next_name = MAX(gles_egl_current->textures->next_name, next);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        GLenum target = sr32(r);
        GLint p[6];
        bool generated = false;

        for (int k = 0; k < 6; k++) p[k] = sr32(r);
        if (r->bad || !name || (target != GL_TEXTURE_2D && target != GL_TEXTURE_RECTANGLE_ARB)) {
            r->bad = true;
            break;
        }
        glBindTexture(target, name);
        /* Parameters first: GENERATE_MIPMAP (ES1) remakes the chain as level 0 arrives. */
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, p[0]);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, p[1]);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, p[2]);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, p[3]);
        if (p[4] != INT32_MIN && target == GL_TEXTURE_2D) glTexParameteri(target, GL_GENERATE_MIPMAP, p[4]);
        for (uint32_t lvl; (lvl = sr32(r)) && !r->bad;) {
            GLint wd = sr32(r), ht = sr32(r);
            GLenum internal = sr32(r), format = sr32(r), type = sr32(r);
            bool made = sr32(r);
            uint32_t len;
            const uint8_t *px = srbytes(r, &len);

            if (lvl > GLES_EGL_LEVELS || wd <= 0 || ht <= 0 || wd > 4096 || ht > 4096) {
                r->bad = true;
                break;
            }
            if (made) {
                generated = true;
                continue;
            }
            if (gles_native_level_depth(format) && !len &&
                gles_charge_texture(target, lvl - 1, (uint64_t)wd * ht * gles_native_level_bpp(format, type))) {
                gles_tex_charge.table = NULL;
                glTexImage2D(target, lvl - 1, internal, wd, ht, 0, format, type, NULL);
            } else if (px && len && (uint64_t)wd * ht * gles_native_level_bpp(format, type) == len &&
                       gles_charge_texture(target, lvl - 1, len)) {
                gles_tex_charge.table = NULL;
                glTexImage2D(target, lvl - 1, internal, wd, ht, 0, format, type, px);
            }
        }
        if (generated && target == GL_TEXTURE_2D) glGenerateMipmapEXT(target);
        snap_drain("texture restore");
    }
}
#else
static bool snap_depth_ifmt(GLint ifmt);

static void snap_put_textures(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW tw = { body };
    uint32_t count = 0;
    GLint unpack = 4;

    for (GLuint name = 1, gap = 0; snap_scan(&name, &gap); name++) {
        GLenum target = GL_TEXTURE_2D;
        if (!glIsTexture(name)) { gap++; continue; }
        gap = 0;
        while (glGetError() != GL_NO_ERROR) {
        }
        glBindTexture(GL_TEXTURE_2D, name);
        if (glGetError() != GL_NO_ERROR) {
            target = GL_TEXTURE_RECTANGLE_ARB;
            glBindTexture(target, name);
            if (glGetError() != GL_NO_ERROR) {
                fprintf(stderr, "[gles] snapshot: texture %u has an unsupported target; dropped\n", name);
                continue;
            }
        }
        count++;
        sw32(&tw, name);
        sw32(&tw, target);
        {
            static const GLenum p[] = { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
                                        GL_TEXTURE_WRAP_T, GL_GENERATE_MIPMAP, GL_TEXTURE_MAX_LEVEL };
            for (unsigned i = 0; i < ARRAY_SIZE(p); i++) {
                GLint v = 0;
                glGetTexParameteriv(target, p[i], &v);
                sw32(&tw, v);
            }
        }
        for (GLint lvl = 0; lvl < 13; lvl++) {
            GLint wd = 0, ht = 0, ifmt = 0;
            glGetTexLevelParameteriv(target, lvl, GL_TEXTURE_WIDTH, &wd);
            glGetTexLevelParameteriv(target, lvl, GL_TEXTURE_HEIGHT, &ht);
            glGetTexLevelParameteriv(target, lvl, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            if (wd <= 0 || ht <= 0) break;
            sw32(&tw, lvl + 1);
            sw32(&tw, wd);
            sw32(&tw, ht);
            sw32(&tw, ifmt);
            if (snap_depth_ifmt(ifmt)) {
                swbytes(&tw, NULL, 0);  /* storage only, as a renderbuffer's (snap_get_textures) */
                continue;
            }
            g_autofree uint8_t *px = g_malloc((size_t)wd * ht * 4);
            glGetIntegerv(GL_PACK_ALIGNMENT, &unpack);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(target, lvl, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glPixelStorei(GL_PACK_ALIGNMENT, unpack);
            swbytes(&tw, px, (uint32_t)wd * ht * 4);
        }
        sw32(&tw, 0);
    }
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

/* A depth (or depth and stencil) texture: its storage comes back, its contents cleared, as a renderbuffer's. */
static bool snap_depth_ifmt(GLint ifmt)
{
    switch (ifmt) {
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
    case GL_DEPTH_STENCIL_EXT: case GL_DEPTH24_STENCIL8_EXT:
        return true;
    default:
        return false;
    }
}

/* Internal formats that can be recreated from RGBA8 bytes as they are;
 * anything else (compressed, host-private) comes back as RGBA. */
static GLint snap_texture_ifmt(GLint ifmt)
{
    switch (ifmt) {
    case GL_RGBA: case GL_RGB: case GL_ALPHA: case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA: case GL_RGBA8: case GL_RGB8: case GL_ALPHA8:
    case GL_LUMINANCE8: case GL_LUMINANCE8_ALPHA8: case GL_RGB5_A1: case GL_RGBA4:
        return ifmt;
    default:
        return GL_RGBA;
    }
}

static void snap_get_textures(SnapR *r)
{
    uint32_t n = sr32(r);
    gles_unpack_tight();
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        GLenum target = sr32(r);
        GLint p[6];
        for (int k = 0; k < 6; k++) p[k] = sr32(r);
        glBindTexture(target, name);
        for (uint32_t lvl; (lvl = sr32(r)) && !r->bad;) {
            GLint wd = sr32(r), ht = sr32(r), ifmt = sr32(r);
            uint32_t len;
            const uint8_t *px = srbytes(r, &len);
            /* The host reads wd * ht * 4 bytes: exactly the run the blob holds. */
            if (snap_depth_ifmt(ifmt) && !len && wd > 0 && ht > 0 && wd <= 4096 && ht <= 4096 &&
                gles_charge_texture(target, lvl - 1, (uint64_t)wd * ht * 4)) {
                bool stencil = ifmt == GL_DEPTH_STENCIL_EXT || ifmt == GL_DEPTH24_STENCIL8_EXT;
                gles_tex_charge.table = NULL;
                glTexImage2D(target, lvl - 1, ifmt, wd, ht, 0, stencil ? GL_DEPTH_STENCIL_EXT : GL_DEPTH_COMPONENT,
                             stencil ? GL_UNSIGNED_INT_24_8_EXT : GL_UNSIGNED_INT, NULL);
            } else if (px && wd > 0 && ht > 0 && (uint64_t)wd * ht * 4 == len &&
                gles_charge_texture(target, lvl - 1, len)) {
                gles_tex_charge.table = NULL;
                glTexImage2D(target, lvl - 1, snap_texture_ifmt(ifmt), wd, ht, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
        }
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, p[0]);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, p[1]);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, p[2]);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, p[3]);
        if (target == GL_TEXTURE_2D) {
            glTexParameteri(target, GL_GENERATE_MIPMAP, p[4]);
        }
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, p[5]);
    }
}
#endif

static void snap_put_renderbuffers(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW bw = { body };
    uint32_t count = 0;
    for (GLuint name = 1, gap = 0; snap_scan(&name, &gap); name++) {
        GLint wd = 0, ht = 0, ifmt = 0;
        if (!glIsRenderbufferEXT(name)) { gap++; continue; }
        gap = 0;
        count++;
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, name);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_WIDTH_EXT, &wd);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_HEIGHT_EXT, &ht);
        glGetRenderbufferParameterivEXT(GL_RENDERBUFFER_EXT, GL_RENDERBUFFER_INTERNAL_FORMAT_EXT, &ifmt);
        sw32(&bw, name);
        sw32(&bw, wd);
        sw32(&bw, ht);
        sw32(&bw, ifmt);
    }
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

static void snap_get_renderbuffers(SnapR *r)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        GLint wd = sr32(r), ht = sr32(r), ifmt = sr32(r);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, name);
        if (wd > 0 && ht > 0 &&
            gles_charge(gles_charge_table(false), gles_charge_key(GLES_CHARGE_RENDERBUFFER, name, 0, 0),
                        (uint64_t)wd * ht * 4, NULL)) {
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, ifmt, wd, ht);
        }
    }
}

static const GLenum snap_attachments[] = {
    GL_COLOR_ATTACHMENT0_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_STENCIL_ATTACHMENT_EXT,
};

static void snap_put_framebuffers(SnapW *w)
{
    GByteArray *body = g_byte_array_new();
    SnapW fw = { body };
    uint32_t count = 0;
    for (GLuint name = 1, gap = 0; snap_scan(&name, &gap); name++) {
        if (!glIsFramebufferEXT(name)) { gap++; continue; }
        gap = 0;
        count++;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, name);
        sw32(&fw, name);
        for (unsigned i = 0; i < ARRAY_SIZE(snap_attachments); i++) {
            GLint type = 0, obj = 0, lvl = 0, target = GL_TEXTURE_2D;
            glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, snap_attachments[i],
                GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE_EXT, &type);
            if (type == GL_TEXTURE || type == GL_RENDERBUFFER_EXT) {
                glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, snap_attachments[i],
                    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME_EXT, &obj);
            }
            if (type == GL_TEXTURE) {
                GLint rect = 0;
                glGetFramebufferAttachmentParameterivEXT(GL_FRAMEBUFFER_EXT, snap_attachments[i],
                    GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL_EXT, &lvl);
#ifdef GLES_HOST_EGL_ES
                GLESEGLTexture *t = obj ? gles_native_texture(obj, false) : NULL;
                rect = t && t->target == GL_TEXTURE_RECTANGLE_ARB;
#else
                /* The texture target is not an attachment query in GL 2.1:
                 * a rectangle texture refuses a 2D bind. */
                glBindTexture(GL_TEXTURE_RECTANGLE_ARB, obj);
                rect = glGetError() == GL_NO_ERROR;
                glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
#endif
                target = rect ? GL_TEXTURE_RECTANGLE_ARB : GL_TEXTURE_2D;
            }
            sw32(&fw, type);
            sw32(&fw, obj);
            sw32(&fw, lvl);
            sw32(&fw, target);
        }
    }
    sw32(w, count);
    g_byte_array_append(w->b, body->data, body->len);
    g_byte_array_free(body, true);
}

static void snap_get_framebuffers(SnapR *r)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        GLuint name = sr32(r);
        GLint seen[ARRAY_SIZE(snap_attachments)][2];
        GLenum status;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, name);
        for (unsigned k = 0; k < ARRAY_SIZE(snap_attachments); k++) {
            GLint type = sr32(r), obj = sr32(r), lvl = sr32(r), target = sr32(r);
            seen[k][0] = type;
            seen[k][1] = obj;
            if (type == GL_TEXTURE) {
                glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, snap_attachments[k], target, obj, lvl);
            } else if (type == GL_RENDERBUFFER_EXT) {
                glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, snap_attachments[k],
                                             GL_RENDERBUFFER_EXT, obj);
            }
        }
        status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
        if (status != GL_FRAMEBUFFER_COMPLETE_EXT && (seen[0][0] || seen[1][0] || seen[2][0])) {
            fprintf(stderr, "[gles] snapshot: framebuffer %u restored incomplete (0x%x): color 0x%x %d, depth 0x%x %d, "
                    "stencil 0x%x %d\n", name, status, seen[0][0], seen[0][1], seen[1][0], seen[1][1], seen[2][0],
                    seen[2][1]);
        }
    }
}

/* Shaders and programs: saved under their guest ids, recreated with new host
 * ids. Programs link against their shaders, so shaders go first. */
static void snap_put_glsl(SnapW *w)
{
    uint32_t *next;
    GHashTable **t = gles_glsl_table(&next);
    GHashTableIter it;
    gpointer k, v;

    sw32(w, *next);
    /* An ES1 context has no programs to ask about (and none in its table: iOS shares no objects across APIs). */
    if (!snap_shaders()) {
        sw32(w, 0);
        return;
    }
    sw32(w, *t ? g_hash_table_size(*t) : 0);
    if (!*t) return;
    for (int pass = 0; pass < 2; pass++) {
        g_hash_table_iter_init(&it, *t);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            GLuint host = GPOINTER_TO_UINT(v);
            bool prog = glIsProgram(host);
            if (prog != (pass == 1)) continue;
            sw32(w, GPOINTER_TO_UINT(k));
            sw32(w, prog);
            if (!prog) {
                GLint type = 0, len = 0;
                glGetShaderiv(host, GL_SHADER_TYPE, &type);
                glGetShaderiv(host, GL_SHADER_SOURCE_LENGTH, &len);
                g_autofree char *src = g_malloc0(len + 1);
                if (len) glGetShaderSource(host, len + 1, NULL, src);
                sw32(w, type);
                swbytes(w, src, strlen(src));
                continue;
            }
            GLuint sh[16];
            GLsizei nsh = 0;
            GLint linked = 0, nattr = 0, nuni = 0;
            /* Attached shaders travel inline: apps delete their shaders
             * right after linking, so they are often gone from the name map
             * while the program still uses them. */
            glGetAttachedShaders(host, 16, &nsh, sh);
            sw32(w, nsh);
            for (GLsizei i = 0; i < nsh; i++) {
                GLint type = 0, len = 0;
                glGetShaderiv(sh[i], GL_SHADER_TYPE, &type);
                glGetShaderiv(sh[i], GL_SHADER_SOURCE_LENGTH, &len);
                g_autofree char *src = g_malloc0(len + 1);
                if (len) glGetShaderSource(sh[i], len + 1, NULL, src);
                sw32(w, type);
                swbytes(w, src, strlen(src));
            }
            glGetProgramiv(host, GL_LINK_STATUS, &linked);
            sw32(w, linked);
            glGetProgramiv(host, GL_ACTIVE_ATTRIBUTES, &nattr);
            sw32(w, linked ? nattr : 0);
            for (GLint i = 0; linked && i < nattr; i++) {
                char name[256];
                GLint size;
                GLenum type;
                glGetActiveAttrib(host, i, sizeof(name), NULL, &size, &type, name);
                swbytes(w, name, strlen(name));
                sw32(w, glGetAttribLocation(host, name));
            }
            /* Uniform values, one record per element, with the location the
             * guest was given (it caches locations; see snap_get_glsl). */
            GByteArray *body = g_byte_array_new();
            SnapW uw = { body };
            uint32_t count = 0;
            glGetProgramiv(host, GL_ACTIVE_UNIFORMS, &nuni);
            glUseProgram(host);
            for (GLint i = 0; linked && i < nuni; i++) {
                char name[256], el[272];
                GLint size;
                GLenum type;
                glGetActiveUniform(host, i, sizeof(name), NULL, &size, &type, name);
                char *br = strchr(name, '[');
                if (br) *br = 0;
                for (GLint e = 0; e < size; e++) {
                    snprintf(el, sizeof(el), size > 1 ? "%s[%d]" : "%s", name, e);
                    GLint loc = glGetUniformLocation(host, el);
                    GLfloat val[16] = { 0 };
                    if (loc < 0) continue;
                    if (type == GL_INT || type == GL_BOOL || type == GL_SAMPLER_2D ||
                        type == GL_SAMPLER_CUBE || type == GL_INT_VEC2 || type == GL_INT_VEC3 ||
                        type == GL_INT_VEC4 || type == GL_BOOL_VEC2 || type == GL_BOOL_VEC3 ||
                        type == GL_BOOL_VEC4) {
                        glGetUniformiv(host, loc, (GLint *)val);
                    } else {
                        glGetUniformfv(host, loc, val);
                    }
                    count++;
                    swbytes(&uw, el, strlen(el));
                    sw32(&uw, type);
                    sw32(&uw, gles_uloc_guest(GPOINTER_TO_UINT(k), loc));
                    swbytes(&uw, val, sizeof(val));
                }
            }
            sw32(w, count);
            g_byte_array_append(w->b, body->data, body->len);
            g_byte_array_free(body, true);
        }
    }
}

static void snap_set_uniform(GLint loc, GLenum type, const GLfloat *v)
{
    const GLint *iv = (const GLint *)v;
    switch (type) {
    case GL_FLOAT: glUniform1fv(loc, 1, v); break;
    case GL_FLOAT_VEC2: glUniform2fv(loc, 1, v); break;
    case GL_FLOAT_VEC3: glUniform3fv(loc, 1, v); break;
    case GL_FLOAT_VEC4: glUniform4fv(loc, 1, v); break;
    case GL_FLOAT_MAT2: glUniformMatrix2fv(loc, 1, GL_FALSE, v); break;
    case GL_FLOAT_MAT3: glUniformMatrix3fv(loc, 1, GL_FALSE, v); break;
    case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, 1, GL_FALSE, v); break;
    case GL_INT: case GL_BOOL: case GL_SAMPLER_2D: case GL_SAMPLER_CUBE:
        glUniform1iv(loc, 1, iv); break;
    case GL_INT_VEC2: case GL_BOOL_VEC2: glUniform2iv(loc, 1, iv); break;
    case GL_INT_VEC3: case GL_BOOL_VEC3: glUniform3iv(loc, 1, iv); break;
    case GL_INT_VEC4: case GL_BOOL_VEC4: glUniform4iv(loc, 1, iv); break;
    }
}

static void snap_get_glsl(SnapR *r)
{
    uint32_t *next;
    GHashTable **t = gles_glsl_table(&next);
    uint32_t n;

    *next = sr32(r);
    n = sr32(r);
    if (!*t) *t = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_remove_all(*t);
    if (*gles_uloc_table()) g_hash_table_remove_all(*gles_uloc_table());
    if (*gles_glsl_source_table()) g_hash_table_remove_all(*gles_glsl_source_table());
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t guest = sr32(r), prog = sr32(r), len;
        if (!prog) {
            GLenum type = sr32(r);
            const uint8_t *src = srbytes(r, &len);
            g_autofree char *s = g_strndup((const char *)src, src ? len : 0);
            const char *p = s;
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &p, NULL);
            glCompileShader(sh);
            g_hash_table_insert(*t, GUINT_TO_POINTER(guest), GUINT_TO_POINTER(sh));
            continue;
        }
        GLuint pr = glCreateProgram(), shs[16];
        uint32_t nsh = sr32(r);
        for (uint32_t k = 0; k < nsh && !r->bad; k++) {
            GLenum type = sr32(r);
            const uint8_t *src = srbytes(r, &len);
            g_autofree char *s = g_strndup((const char *)src, src ? len : 0);
            const char *p = s;
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &p, NULL);
            glCompileShader(sh);
            glAttachShader(pr, sh);
            if (k < 16) shs[k] = sh;
        }
        uint32_t linked = sr32(r), nattr = sr32(r);
        for (uint32_t k = 0; k < nattr && !r->bad; k++) {
            const uint8_t *nm = srbytes(r, &len);
            g_autofree char *name = g_strndup((const char *)nm, nm ? len : 0);
            GLint loc = (GLint)sr32(r);
            if (loc >= 0) glBindAttribLocation(pr, loc, name);
        }
#ifdef GLES_HOST_EGL_ES
        if (linked) { glLinkProgram(pr); gles_fetch_link(pr); }
#else
        if (linked) { gles_link_program(pr); gles_fetch_link(pr); }
#endif
        for (uint32_t k = 0; k < MIN(nsh, 16); k++) glDeleteShader(shs[k]);  /* freed with the program */
        g_hash_table_insert(*t, GUINT_TO_POINTER(guest), GUINT_TO_POINTER(pr));
        uint32_t nuni = sr32(r);
        if (linked) glUseProgram(pr);
        for (uint32_t k = 0; k < nuni && !r->bad; k++) {
            const uint8_t *nm = srbytes(r, &len);
            g_autofree char *name = g_strndup((const char *)nm, nm ? len : 0);
            GLenum type = sr32(r);
            GLint old_loc = (GLint)sr32(r);
            const uint8_t *val = srbytes(r, &len);
            GLint loc = linked ? glGetUniformLocation(pr, name) : -1;
            if (loc >= 0 && val && len == 16 * sizeof(GLfloat)) {
                snap_set_uniform(loc, type, (const GLfloat *)val);
            }
            if (loc != old_loc && old_loc >= 0) {
                /* The guest keeps the location it was given. */
                g_hash_table_insert(gles_uloc_map(guest, true), GINT_TO_POINTER(old_loc + 1),
                                    GINT_TO_POINTER(loc + 1));
            }
        }
    }
}

/* All objects of the namespace current in the bound context. */
static void snap_put_objects(SnapW *w)
{
    snap_put_textures(w);
    snap_drain("textures");
    snap_put_renderbuffers(w);
    snap_drain("renderbuffers");
    snap_put_framebuffers(w);
    snap_drain("framebuffers");
    snap_put_glsl(w);
    snap_drain("shaders and programs");
}

static void snap_get_objects(SnapR *r)
{
    snap_get_textures(r);
    snap_drain("textures");
    snap_get_renderbuffers(r);
    snap_drain("renderbuffers");
    snap_get_framebuffers(r);
    snap_drain("framebuffers");
    snap_get_glsl(r);
    snap_drain("shaders and programs");
}

/* ---- GLESHost fields ------------------------------------------------------ */

static void snap_put_array(SnapW *w, const GLESArray *a)
{
    sw32(w, a->enabled);
    sw32(w, a->size);
    sw32(w, a->type);
    sw32(w, a->stride);
    sw32(w, a->ptr);
    sw32(w, a->vbo ? a->vbo->name : 0);
    sw32(w, a->client_state);
}

/* An array as the setter would have left it (gles_set_pointer: never set, or a
 * layout it accepts), under its own client state or none. */
static void snap_get_array(SnapR *r, GLESArray *a, GHashTable *buffers, GLenum client_state)
{
    a->enabled = sr32(r);
    a->size = sr32(r);
    a->type = sr32(r);
    a->stride = sr32(r);
    a->ptr = sr32(r);
    a->vbo = snap_buffer(buffers, sr32(r));
    a->client_state = sr32(r);
    if ((a->type && !gles_array_layout_ok(a->size, a->type, a->stride)) ||
        (a->client_state && a->client_state != client_state)) {
        r->bad = true;
    }
}

static bool snap_alignment_ok(uint32_t align)
{
    return !align || align == 1 || align == 2 || align == 4 || align == 8;
}

/* A VAO's saved arrays (gles_vao_swap), in snap_put_host's order; a missing one as a 0. */
static void snap_put_vao(SnapW *w, const GLESVao *v)
{
    sw32(w, v != NULL);
    if (!v) return;
    snap_put_array(w, &v->vertex);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) snap_put_array(w, &v->texcoord[i]);
    snap_put_array(w, &v->color);
    snap_put_array(w, &v->normal);
    snap_put_array(w, &v->pointsize);
    snap_put_array(w, &v->weight);
    snap_put_array(w, &v->matrix_index);
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        snap_put_array(w, &v->attr[i]);
        sw32(w, v->attr_normalized[i]);
    }
    sw32(w, v->element_buffer ? v->element_buffer->name : 0);
}

static GLESVao *snap_get_vao(SnapR *r, GHashTable *buffers)
{
    GLESVao *v;
    if (!sr32(r)) return NULL;
    v = g_new0(GLESVao, 1);
    snap_get_array(r, &v->vertex, buffers, GL_VERTEX_ARRAY);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) snap_get_array(r, &v->texcoord[i], buffers, GL_TEXTURE_COORD_ARRAY);
    snap_get_array(r, &v->color, buffers, GL_COLOR_ARRAY);
    snap_get_array(r, &v->normal, buffers, GL_NORMAL_ARRAY);
    snap_get_array(r, &v->pointsize, buffers, GL_POINT_SIZE_ARRAY_OES);
    snap_get_array(r, &v->weight, buffers, GL_WEIGHT_ARRAY_OES);
    snap_get_array(r, &v->matrix_index, buffers, GL_MATRIX_INDEX_ARRAY_OES);
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        snap_get_array(r, &v->attr[i], buffers, 0);
        v->attr_normalized[i] = sr32(r);
    }
    v->element_buffer = snap_buffer(buffers, sr32(r));
    return v;
}

static void snap_put_host(SnapW *w, GLESHost *h)
{
    sw32(w, h->api_version);
    sw32(w, h->inited);
    sw32(w, h->fbo);
    sw32(w, h->tex);
    sw32(w, h->depth);
    sw32(w, h->drawable_width);
    sw32(w, h->drawable_height);
    sw32(w, h->drawable_announced);
    sw32(w, h->drawable_renderbuffer);
    sw32(w, h->clip_enabled);
    snap_put_array(w, &h->vertex);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) snap_put_array(w, &h->texcoord[i]);
    sw32(w, h->client_active_unit);
    snap_put_array(w, &h->color);
    snap_put_array(w, &h->normal);
    snap_put_array(w, &h->pointsize);
    sw32(w, h->array_buffer ? h->array_buffer->name : 0);
    sw32(w, h->element_buffer ? h->element_buffer->name : 0);
    sw32(w, h->bound_renderbuffer);
    sw32(w, h->bound_framebuffer);
    snap_put_map(w, h->fbo_drawable, 0);
    sw32(w, h->unpack_alignment);
    sw32(w, h->unpack_row_bytes);
    sw32(w, h->pack_alignment);
    sw32(w, h->error);
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        snap_put_array(w, &h->attr[i]);
        sw32(w, h->attr_normalized[i]);
    }
    sw32(w, h->program);
    swbytes(w, &h->default_pvrtc, sizeof(h->default_pvrtc));
    snap_put_array(w, &h->weight);
    snap_put_array(w, &h->matrix_index);
    sw32(w, h->palette_on);
    sw32(w, h->palette_mode);
    sw32(w, h->palette_current);
    swbytes(w, h->palette, sizeof(h->palette));
    /* 7: the VAOs, the bound one's copy empty (gles_vao_swap) */
    sw32(w, h->vao_bound);
    sw32(w, h->vao_next);
    snap_put_vao(w, h->vao_default);
    sw32(w, h->vaos ? g_hash_table_size(h->vaos) : 0);
    if (h->vaos) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, h->vaos);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            sw32(w, GPOINTER_TO_UINT(key));
            snap_put_vao(w, value);
        }
    }
}

static void snap_get_host(SnapR *r, GLESHost *h, uint32_t version)
{
    uint32_t len;
    h->api_version = version >= 4 ? sr32(r) : 0;
    if (h->api_version > 2) r->bad = true;
    h->inited = sr32(r);
    h->fbo = sr32(r);
    h->tex = sr32(r);
    h->depth = sr32(r);
    h->drawable_width = sr32(r);
    h->drawable_height = sr32(r);
    h->drawable_announced = sr32(r);
    h->drawable_renderbuffer = version >= 8 ? sr32(r) : 0;
    h->clip_enabled = version >= 8 ? sr32(r) : 0;
    if (h->clip_enabled & ~255u) r->bad = true;
    if (h->inited && (h->drawable_width - 1 >= GLES_FB_MAX || h->drawable_height - 1 >= GLES_FB_MAX)) {
        r->bad = true;      /* 1..GLES_FB_MAX, as gles_drawable_storage takes them */
    }
    snap_get_array(r, &h->vertex, h->buffers, GL_VERTEX_ARRAY);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) {
        snap_get_array(r, &h->texcoord[i], h->buffers, GL_TEXTURE_COORD_ARRAY);
    }
    h->client_active_unit = sr32(r);
    if (h->client_active_unit >= GLES_MAX_TEXUNITS) r->bad = true;
    snap_get_array(r, &h->color, h->buffers, GL_COLOR_ARRAY);
    snap_get_array(r, &h->normal, h->buffers, GL_NORMAL_ARRAY);
    snap_get_array(r, &h->pointsize, h->buffers, GL_POINT_SIZE_ARRAY_OES);
    h->array_buffer = snap_buffer(h->buffers, sr32(r));
    h->element_buffer = snap_buffer(h->buffers, sr32(r));
    h->bound_renderbuffer = sr32(r);
    h->bound_framebuffer = sr32(r);
    if (!h->fbo_drawable) h->fbo_drawable = g_hash_table_new(g_direct_hash, g_direct_equal);
    snap_get_map(r, h->fbo_drawable, 0, 0);
    if (version < 8) {
        /* Before 8 the map was a set (each value its own key): which renderbuffer a drawable has is unknown, 0. */
        GHashTableIter it;
        g_hash_table_iter_init(&it, h->fbo_drawable);
        while (g_hash_table_iter_next(&it, NULL, NULL)) {
            g_hash_table_iter_replace(&it, NULL);
        }
    }
    h->unpack_alignment = sr32(r);
    h->unpack_row_bytes = sr32(r);
    h->pack_alignment = sr32(r);
    if (!snap_alignment_ok(h->unpack_alignment) || !snap_alignment_ok(h->pack_alignment)) r->bad = true;
    h->error = sr32(r);
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        snap_get_array(r, &h->attr[i], h->buffers, 0);
        h->attr_normalized[i] = sr32(r);
    }
    h->program = sr32(r);
    {
        const uint8_t *p = srbytes(r, &len);
        size_t expected = version >= 8 ? sizeof(h->default_pvrtc) : offsetof(GLESPVRTC, immutable_levels);
        if (p && len == expected) memcpy(&h->default_pvrtc, p, len);
        else r->bad = true;
    }
    gles_palette_reset(h);                  /* before 6: as a new context has it */
    if (version >= 6) {
        const uint8_t *p;
        snap_get_array(r, &h->weight, h->buffers, GL_WEIGHT_ARRAY_OES);
        snap_get_array(r, &h->matrix_index, h->buffers, GL_MATRIX_INDEX_ARRAY_OES);
        h->palette_on = sr32(r);
        h->palette_mode = sr32(r);
        h->palette_current = sr32(r);
        p = srbytes(r, &len);
        if (p && len == sizeof(h->palette)) memcpy(h->palette, p, len);
        else r->bad = true;
        /* as gles_set_pointer and glCurrentPaletteMatrixOES leave them */
        if (h->weight.size > GLES_MAX_VERTEX_UNITS || h->matrix_index.size > GLES_MAX_VERTEX_UNITS ||
            (h->weight.type && h->weight.type != GL_FLOAT && h->weight.type != GLES_FIXED) ||
            (h->matrix_index.type && h->matrix_index.type != GL_UNSIGNED_BYTE) ||
            h->palette_current >= GLES_MAX_PALETTE) {
            r->bad = true;
        }
    }
    if (version >= 7) {
        uint32_t count;
        h->vao_bound = sr32(r);
        h->vao_next = sr32(r);
        h->vao_default = snap_get_vao(r, h->buffers);
        count = sr32(r);
        if (count > GLES_MAX_NAMES) {
            r->bad = true;
            count = 0;
        }
        for (uint32_t i = 0; i < count && !r->bad; i++) {
            uint32_t name = sr32(r);
            GLESVao *v = snap_get_vao(r, h->buffers);
            if (!name || !v) {
                gles_vao_free(v);
                r->bad = true;
                break;
            }
            g_hash_table_replace(gles_vao_table(h), GUINT_TO_POINTER(name), v);
        }
        if (h->vao_bound && !(h->vaos && g_hash_table_contains(h->vaos, GUINT_TO_POINTER(h->vao_bound)))) {
            r->bad = true;
        }
    }
}

/* ---- sections ------------------------------------------------------------- */

/*
 * Each surface's page list, after the tables (v5). Without it a restored surface wrote its frames
 * back through the MMU of whatever process flushed them, until a bind found its pages again, and
 * CoreAnimation binds the surfaces it composites into once: the iPod touch 2G's restored screen kept
 * the frame it was saved with while SpringBoard went on drawing (LightTouchMac's sessions resume).
 * The pages are RAM offsets, which a snapshot restores on the same machine.
 */
static void snap_put_pages(SnapW *w, GHashTable *surfaces)
{
    GHashTableIter it;
    gpointer k, v;
    sw32(w, surfaces ? g_hash_table_size(surfaces) : 0);
    if (!surfaces) return;
    g_hash_table_iter_init(&it, surfaces);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESSurface *s = v;
        sw32(w, GPOINTER_TO_UINT(k));
        swbytes(w, s->pages, s->npages * sizeof(ram_addr_t));
    }
}

/* Whether [ram, ram + page) is RAM; its region is then logged as a bind logs it. */
static bool snap_page_ram(ram_addr_t ram)
{
    RAMBlock *block;

    if (ram & ~TARGET_PAGE_MASK) return false;
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(block) {
        if (ram >= block->offset && ram - block->offset + TARGET_PAGE_SIZE <= block->used_length) {
            gles_log_region(block->mr);
            return true;
        }
    }
    return false;
}

/* Gives each surface in `surfaces` its saved pages: as many as its rows span, each in RAM. */
static void snap_get_pages(SnapR *r, GHashTable *surfaces)
{
    uint32_t n = sr32(r);
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        uint32_t k = sr32(r), len;
        const uint8_t *p = srbytes(r, &len);
        GLESSurface *s = g_hash_table_lookup(surfaces, GUINT_TO_POINTER(k));
        unsigned count = len / sizeof(ram_addr_t);
        if (!p || !s || len % sizeof(ram_addr_t)) {
            r->bad = true;
            break;
        }
        if (!count) continue;
        bool nv12 = s->format == 0x34323076 || s->format == 0x34323066;
        if (count != gles_surface_npages(s->base, s->stride, s->width, s->height,
                                         gles_surface_bpp(s->format), nv12)) {
            r->bad = true;
            break;
        }
        GLESSurface *with = g_realloc(s, sizeof(GLESSurface) + len);
        memcpy(with->pages, p, len);
        with->npages = count;
        for (unsigned j = 0; j < count && !r->bad; j++) {
            if (!snap_page_ram(with->pages[j])) r->bad = true;
        }
        if (r->bad) with->npages = 0;
        g_hash_table_steal(surfaces, GUINT_TO_POINTER(k));
        g_hash_table_insert(surfaces, GUINT_TO_POINTER(k), with);
    }
}

/* The namespace-wide tables of a group, or of a context with no group. */
static void snap_put_tables(SnapW *w, GHashTable *buffers, GHashTable *surfaces,
                            GHashTable *rb_sized, GHashTable *pvrtc, uint32_t next_buffer)
{
    sw32(w, next_buffer);
    snap_put_buffers(w, buffers);
    snap_put_map(w, surfaces, GLES_SURFACE_SNAP_BYTES);
    snap_put_map(w, rb_sized, 0);
    snap_put_map(w, pvrtc, sizeof(GLESPVRTC));
    snap_put_pages(w, surfaces);
}

/* A surface gles_bind_surface would have accepted, its page list still to come (npages 0, as a
 * v3 or v4 snapshot leaves it: re-read through the MMU until the next bind). */
static bool snap_surface_ok(GLESSurface *s)
{
    bool nv12 = s->format == 0x34323076 || s->format == 0x34323066;

    s->window = s->window != 0;
    s->dirty = s->dirty != 0;
    return (s->target == GL_TEXTURE_2D || s->target == GL_TEXTURE_RECTANGLE_ARB) &&
           s->width && s->height && s->width <= 4096 && s->height <= 4096 &&
           (nv12 ? !s->window : gles_surface_bpp(s->format) != 0);
}

static uint32_t snap_get_tables(SnapR *r, GHashTable *buffers, GHashTable *surfaces,
                                GHashTable *rb_sized, GHashTable *pvrtc, uint32_t version)
{
    GHashTableIter it;
    gpointer k, v;
    uint32_t next = sr32(r);
    snap_get_buffers(r, buffers, version);
    snap_get_map(r, surfaces, GLES_SURFACE_SNAP_BYTES, sizeof(GLESSurface));
    g_hash_table_iter_init(&it, surfaces);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (!snap_surface_ok(v)) r->bad = true;
    }
    snap_get_map(r, rb_sized, 0, 0);
    snap_get_map(r, pvrtc, version >= 8 ? sizeof(GLESPVRTC) : offsetof(GLESPVRTC, immutable_levels), sizeof(GLESPVRTC));
    if (version >= 5) snap_get_pages(r, surfaces);
    return next;
}

/* Queries are context-local. Ended results are materialized; an active query
 * is split at the snapshot boundary and its partial result is accumulated. */
#ifdef GLES_HOST_EGL_ES
/* The native ES backend has no occlusion queries (gles_query_* are the desktop's). */
static void snap_put_queries(SnapW *w, GLESHost *h)
{
    sw32(w, 0);
    sw32(w, 0);
    sw32(w, 0);
}
static void snap_get_queries(SnapR *r, GLESHost *h)
{
    sr32(r);
    sr32(r);
    if (sr32(r)) r->bad = true;
}
static void snap_restore_queries(GLESHost *h) {}
#else
static void snap_put_query(SnapW *w, GLESHost *h, uint32_t id, GLESQuery *q, bool deleted)
{
    GLuint result=0;
    if(h->active_query==id)glEndQuery(GL_SAMPLES_PASSED);
    if(q->begun) {
        if(q->restored)result=q->saved;
        else {glGetQueryObjectuiv(q->host,GL_QUERY_RESULT,&result);result=gles_query_combine(q,result);}
    }
    sw32(w,id);sw32(w,q->target);sw32(w,q->begun);sw32(w,q->ended);sw32(w,result);sw32(w,deleted);
    if(h->active_query==id){q->partial=result;glBeginQuery(GL_SAMPLES_PASSED,q->host);}
}
static void snap_put_queries(SnapW *w, GLESHost *h)
{
    sw32(w,h->next_query);sw32(w,h->active_query);
    sw32(w,(h->queries?g_hash_table_size(h->queries):0)+(h->deleted_query?1:0));
    if(h->queries) {
        GHashTableIter it;gpointer key,value;g_hash_table_iter_init(&it,h->queries);
        while(g_hash_table_iter_next(&it,&key,&value))snap_put_query(w,h,GPOINTER_TO_UINT(key),value,false);
    }
    if(h->deleted_query)snap_put_query(w,h,h->active_query,h->deleted_query,true);
}
static void snap_get_queries(SnapR *r, GLESHost *h)
{
    h->next_query=sr32(r);h->active_query=sr32(r);uint32_t n=sr32(r);
    if(n>16385){r->bad=true;return;}
    h->queries=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
    for(uint32_t i=0;i<n&&!r->bad;i++) {
        uint32_t id=sr32(r);GLESQuery *q=g_new0(GLESQuery,1);
        q->target=sr32(r);q->begun=sr32(r)!=0;q->ended=sr32(r)!=0;q->saved=sr32(r);
        bool deleted=sr32(r)!=0;q->restored=q->ended;
        if((deleted&&(id!=h->active_query||h->deleted_query||!q->begun||q->ended))||!id||g_hash_table_contains(h->queries,GUINT_TO_POINTER(id))||
           (q->begun&&!gles_query_target(q->target))||(!q->begun&&(q->target||q->ended))||
           (q->begun&&!q->ended&&id!=h->active_query)) {g_free(q);r->bad=true;break;}
        if(deleted)h->deleted_query=q;else g_hash_table_insert(h->queries,GUINT_TO_POINTER(id),q);
    }
    if(g_hash_table_size(h->queries)>16384)r->bad=true;
    GLESQuery *active=h->deleted_query?h->deleted_query:g_hash_table_lookup(h->queries,GUINT_TO_POINTER(h->active_query));
    if(h->active_query&&(!active||!active->begun||active->ended))r->bad=true;
}
static void snap_restore_queries(GLESHost *h)
{
    if(!h->queries)return;
    GHashTableIter it;gpointer value;g_hash_table_iter_init(&it,h->queries);
    while(g_hash_table_iter_next(&it,NULL,&value)){GLESQuery *q=value;glGenQueries(1,&q->host);}
    GLESQuery *active=h->deleted_query?h->deleted_query:g_hash_table_lookup(h->queries,GUINT_TO_POINTER(h->active_query));
    if(active){if(h->deleted_query)glGenQueries(1,&active->host);active->partial=active->saved;active->restored=false;glBeginQuery(GL_SAMPLES_PASSED,active->host);}
}
#endif

/* Save one context: its own fields, and when it is the context that carries
 * its namespace's objects, those too. Leaves its GL state as it found it. */
static void snap_put_context(SnapW *w, uint32_t handle, GLESHost *h, bool objects)
{
    GLESCtxGL s;
    sw32(w, handle);
    sw32(w, h->group ? gles_group_handle(h->group) : 0);
    if (!h->group) {
        snap_put_tables(w, h->buffers, h->surfaces, h->rb_sized, h->pvrtc, h->next_buffer_name);
    }
    snap_put_host(w, h);
    sw32(w, h->inited && objects);
    if (!h->inited) return;
    gh_current = h;
    CGLSetCurrentContext(h->cgl);
    snap_put_queries(w,h);
    snap_capture_gl(&s);
    swbytes(w, &s, sizeof(s));
    if (objects) snap_put_objects(w);
    snap_apply_gl(&s, false);        /* the object scan moved bindings */
}

static uint32_t gles_group_handle(GLESGroup *group)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, gles_groups);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (v == group) return GPOINTER_TO_UINT(k);
    }
    return 0;
}

static GHashTable *snap_new_buffers(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, gles_buffer_destroy);
}
static GHashTable *snap_new_owned(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
}

static GByteArray *gles_snapshot_save(void)
{
    SnapW w = { g_byte_array_new() };
    GLESHost *was = gh_current;
    GHashTable *done = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter it;
    gpointer k, v;

    if (!gles_contexts) {
        gles_contexts = g_hash_table_new(g_direct_hash, g_direct_equal);
        gles_groups = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    sw32(&w, SNAP_MAGIC);
    sw32(&w, SNAP_VERSION);
    sw32(&w, sizeof(GLESCtxGL));
    sw32(&w, gles_handle);
    sw32(&w, g_hash_table_size(gles_groups));
    g_hash_table_iter_init(&it, gles_groups);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESGroup *g = v;
        sw32(&w, GPOINTER_TO_UINT(k));
        snap_put_tables(&w, g->buffers, g->surfaces, g->rb_sized, g->pvrtc, g->next_buffer_name);
    }
    sw32(&w, g_hash_table_size(gles_contexts) + 1);
    snap_put_context(&w, 0, &gh_legacy, true);
    g_hash_table_iter_init(&it, gles_contexts);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GLESHost *h = v;
        bool objects = h->inited && !g_hash_table_contains(done, h->group);
        if (objects) g_hash_table_add(done, h->group);
        snap_put_context(&w, GPOINTER_TO_UINT(k), h, objects);
    }
    g_hash_table_destroy(done);
    gh_current = was;
    CGLSetCurrentContext(NULL);     /* the vCPU thread makes its own current */
    return w.b;
}

/*
 * A context's program as the guest left it current, with what glUseProgram derives from it (its framebuffer fetch
 * uniforms), and on Android its drawable back in an AHardwareBuffer. Programs come back with new host names
 * (snap_get_glsl), and a context's objects may come with a later context of its share group, so this runs once every
 * context is back; snap_apply_gl, loading, leaves it alone.
 */
#ifdef GLES_HOST_AHB
/*
 * A restored drawable's color is ordinary texture storage, which the present reads back with glReadPixels. Put it
 * back in an AHardwareBuffer, as gles_drawable_storage made it, so the present reads the pixels in place again;
 * they are copied over. Without one (refused, or LT_GLES_NO_IOSURFACE), the readback stays.
 */
static void snap_drawable_ahb(GLESHost *h)
{
    GLESAhb ahb = { .image = EGL_NO_IMAGE_KHR };
    GLint texture = 0, framebuffer = 0;
    GLuint color, old = h->tex;
    uint32_t w = h->drawable_width, ht = h->drawable_height;
    bool done = false;

    if (gles_no_iosurface || h->iosurface || !old || !h->fbo || !w || !ht) return;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
    color = gles_private_name(glIsTexture);
    glBindTexture(GL_TEXTURE_2D, color);
    if (gles_ahb_attach(&ahb, w, ht)) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, h->fbo);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, ht);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, color, 0);
        done = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT &&
               glGetError() == GL_NO_ERROR;
        if (!done) {
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, old, 0);
        } else if (h->fbo_color) {
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, h->fbo_color);
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, color, 0);
        }
    }
    if (done) {
        glDeleteTextures(1, &old);
        h->tex = color;
        h->ahb = ahb;
        h->iosurface = true;
    } else {
        gles_ahb_free(&ahb);
        glDeleteTextures(1, &color);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture == old ? h->tex : (GLuint)texture);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
    snap_drain("drawable into a hardware buffer");
}
#endif

static void snap_use_program(GLESHost *h)
{
    GLuint host;

    if (!h->inited || h->failed) return;
    gh_current = h;
    CGLSetCurrentContext(h->cgl);
#ifdef GLES_HOST_AHB
    snap_drawable_ahb(h);
#endif
    if (!snap_shaders()) return;
    host = h->program ? gles_glsl_host(h->program) : 0;
    glUseProgram(host);
    h->fetch_sampler = host ? glGetUniformLocation(host, "gles_fetch") : -1;
    h->fetch_size = host ? glGetUniformLocation(host, "gles_fetch_size") : -1;
    snap_drain("program");
}

static bool gles_snapshot_load(const uint8_t *p, size_t n)
{
    SnapR r = { p, n, 0, false };
    uint32_t ngroups, ncontexts, version;

    if (sr32(&r) != SNAP_MAGIC || ((version = sr32(&r)) < 3 || version > SNAP_VERSION) ||
        sr32(&r) != sizeof(GLESCtxGL)) {
        fprintf(stderr, "[gles] snapshot: GL state from an incompatible build; not restored\n");
        return false;
    }
    gles_drop_all();
    if (!gles_contexts) {
        gles_contexts = g_hash_table_new(g_direct_hash, g_direct_equal);
        gles_groups = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    gles_handle = sr32(&r);
    ngroups = sr32(&r);
    if (ngroups > GLES_MAX_CONTEXTS) r.bad = true;
    for (uint32_t i = 0; i < ngroups && !r.bad; i++) {
        uint32_t handle = sr32(&r);
        GLESGroup *g;
        if (g_hash_table_contains(gles_groups, GUINT_TO_POINTER(handle))) { r.bad = true; break; }
        g = g_new0(GLESGroup, 1);
        g->refs = 1;
        g->buffers = snap_new_buffers();
        g->surfaces = snap_new_owned();
        g->rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
        g->pvrtc = snap_new_owned();
        g->charged = g_hash_table_new(g_direct_hash, g_direct_equal);
        g->next_buffer_name = snap_get_tables(&r, g->buffers, g->surfaces, g->rb_sized, g->pvrtc, version);
        g_hash_table_insert(gles_groups, GUINT_TO_POINTER(handle), g);
    }
    ncontexts = sr32(&r);
    if (ncontexts > GLES_MAX_CONTEXTS + 1) r.bad = true;
    for (uint32_t i = 0; i < ncontexts && !r.bad; i++) {
        uint32_t handle = sr32(&r), group = sr32(&r), len;
        GLESHost *h;
        GLESCtxGL s;
        bool objects;

        /* each handle once; the legacy context (0) once, first */
        if (handle ? g_hash_table_contains(gles_contexts, GUINT_TO_POINTER(handle)) : i != 0) {
            r.bad = true;
            break;
        }
        h = handle ? g_new0(GLESHost, 1) : &gh_legacy;

        if (group) {
            h->group = g_hash_table_lookup(gles_groups, GUINT_TO_POINTER(group));
            if (!h->group) { r.bad = true; break; }
            h->group->refs++;
            h->buffers = h->group->buffers;
            h->surfaces = h->group->surfaces;
            h->rb_sized = h->group->rb_sized;
            h->pvrtc = h->group->pvrtc;
            h->charged = h->group->charged;
        } else {
            h->buffers = snap_new_buffers();
            h->surfaces = snap_new_owned();
            h->rb_sized = g_hash_table_new(g_direct_hash, g_direct_equal);
            h->pvrtc = snap_new_owned();
            h->next_buffer_name = snap_get_tables(&r, h->buffers, h->surfaces, h->rb_sized, h->pvrtc, version);
        }
        snap_get_host(&r, h, version);
        objects = sr32(&r);
        if (handle) {
            g_hash_table_insert(gles_contexts, GUINT_TO_POINTER(handle), h);
            qatomic_inc(&gles_live_contexts);
        }
        if (!h->inited) continue;
        if (version >= 8) snap_get_queries(&r, h);
        const uint8_t *sp = srbytes(&r, &len);
        if (r.bad || !sp || len != sizeof(s)) { r.bad = true; break; }
        memcpy(&s, sp, sizeof(s));
        if (!snap_gl_ok(&s)) { r.bad = true; break; }
        gh_current = h;
        h->inited = false;
        if (!gles_platform_context_create()) {
            h->failed = true;
            fprintf(stderr, "[gles] snapshot: cannot recreate context %08x\n", handle);
            r.bad = true;   /* its objects are still in the stream */
            break;
        }
        h->inited = true;
        /* The present reads the drawable's size into this; the panel's would be
         * too small for a larger drawable. The color target is the restored
         * texture, not an IOSurface. */
        h->readback_size = (size_t)h->drawable_width * h->drawable_height * 4;
        h->readback = g_malloc0(h->readback_size);
        gles_charge_drawable(h->drawable_width, h->drawable_height, NULL);
        h->iosurface = false;
        if (!handle) qatomic_inc(&gles_live_contexts);
        if (objects) snap_get_objects(&r);
        snap_apply_gl(&s, true);
        snap_restore_queries(h);
    }
    if (!r.bad) {
      GHashTableIter it;
      gpointer v;
      snap_use_program(&gh_legacy);
      g_hash_table_iter_init(&it, gles_contexts);
      while (g_hash_table_iter_next(&it, NULL, &v))
        snap_use_program(v);
    }
    gh_current = &gh_legacy;
    CGLSetCurrentContext(NULL);
    if (r.bad) {
        fprintf(stderr, "[gles] snapshot: GL state truncated or corrupt\n");
        return false;
    }
    return true;
}

typedef struct {
    uint32_t len;
    uint8_t *data;
} GLESSnap;

/*
 * fn on the GL thread, which owns the host contexts, with the BQL let go meanwhile: the GL thread takes it for a
 * memory transaction (gles_log_region, as a restored surface's pages are logged), and a save or load holds it.
 * Nothing else runs on the machine meanwhile: its vCPUs are stopped, and the main loop is the one waiting here.
 */
static void gles_snap_on_gl_thread(void (*fn)(void *), void *opaque)
{
    bool locked = bql_locked();

    if (locked) {
        bql_unlock();
    }
    gles_queue_run(fn, opaque);
    if (locked) {
        bql_lock();
    }
}

static GLESSnap gles_snap;

static void gles_snap_save_1(void *opaque)
{
    *(GByteArray **)opaque = gles_snapshot_save();
}

static void gles_snap_load_1(void *opaque)
{
    *(bool *)opaque = gles_snapshot_load(gles_snap.data, gles_snap.len);
}

static int gles_snap_pre_save(void *opaque)
{
    GByteArray *b;

    gles_snap_on_gl_thread(gles_snap_save_1, &b);
    gles_snap.len = b->len;
    gles_snap.data = (uint8_t *)g_byte_array_free(b, false);
    return 0;
}

static int gles_snap_post_save(void *opaque)
{
    g_free(gles_snap.data);
    gles_snap.data = NULL;
    gles_snap.len = 0;
    return 0;
}

static int gles_snap_post_load(void *opaque, int version_id)
{
    bool ok;

    gles_snap_on_gl_thread(gles_snap_load_1, &ok);
    gles_queue_forget();
    gles_snap_post_save(opaque);
    return ok ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_gles_host = {
    .name = "gles-host",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = gles_snap_pre_save,
    .post_save = gles_snap_post_save,
    .post_load = gles_snap_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(len, GLESSnap),
        VMSTATE_VBUFFER_ALLOC_UINT32(data, GLESSnap, 0, NULL, len),
        VMSTATE_END_OF_LIST()
    }
};

static void gles_snapshot_register(void)
{
    static bool done;
    if (!done) {
        done = true;
        vmstate_register(NULL, 0, &vmstate_gles_host, &gles_snap);
    }
}
