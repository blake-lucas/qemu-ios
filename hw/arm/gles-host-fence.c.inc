/* SPDX-License-Identifier: GPL-2.0-or-later
 * APPLE_fence portability. A fence is a marker in the command stream: setting one is cheap, testing
 * one never waits and only finishing one waits, for that marker. On an EGL host the marker is an
 * EGL_KHR_fence_sync, so a game that sets a fence after a frame and tests it in the next keeps the
 * CPU and the GPU overlapping (after Un1box's 478fbe51). Without the extension (WGL, a driver that
 * offers none) or with LT_GLES_APPLE_FENCE=glfinish, every call is a glFinish(), the old
 * conservative barrier. glTestObjectAPPLE and glFinishObjectAPPLE name a buffer or a texture and
 * cover every command touching it, so they stay a glFinish().
 */
static GHashTable *gles_egl_fences;     /* name -> its EGLSyncKHR while set, NULL otherwise */
static GLuint gles_egl_next_fence;
#ifdef GLES_HOST_EGL
static struct {
    int state;                          /* -1 not asked yet, 0 glFinish, 1 fence syncs */
    PFNEGLCREATESYNCKHRPROC create;
    PFNEGLCLIENTWAITSYNCKHRPROC wait;
    PFNEGLDESTROYSYNCKHRPROC destroy;
} gles_fence_api = { .state = -1 };
static bool gles_fence_syncs(void)
{
    if (gles_fence_api.state < 0) {
        const char *v = getenv("LT_GLES_APPLE_FENCE");
        if (v && !strcmp(v, "glfinish")) {
            gles_fence_api.state = 0;
        } else if (gles_egl_display == EGL_NO_DISPLAY) {
            return false;               /* no display yet: ask again */
        } else {
            gles_fence_api.create = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
            gles_fence_api.wait = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
            gles_fence_api.destroy = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
            gles_fence_api.state = gles_fence_api.create && gles_fence_api.wait && gles_fence_api.destroy;
        }
    }
    return gles_fence_api.state > 0;
}
static void gles_fence_drop(gpointer sync)
{
    if (sync) gles_fence_api.destroy(gles_egl_display, sync);
}
#else
static bool gles_fence_syncs(void) { return false; }
static void gles_fence_drop(gpointer sync) {}
#endif
static void gles_egl_gen_fences(GLsizei n, GLuint *names)
{
    if (!gles_egl_fences)
        gles_egl_fences = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, gles_fence_drop);
    for (GLsizei i = 0; i < n; i++) {
        names[i] = ++gles_egl_next_fence;
        g_hash_table_insert(gles_egl_fences, GUINT_TO_POINTER(names[i]), NULL);
    }
}
static void gles_egl_delete_fences(GLsizei n, const GLuint *names)
{
    if (gles_egl_fences) for (GLsizei i = 0; i < n; i++)
        g_hash_table_remove(gles_egl_fences, GUINT_TO_POINTER(names[i]));
}
static GLboolean gles_egl_is_fence(GLuint name)
{ return gles_egl_fences && g_hash_table_contains(gles_egl_fences, GUINT_TO_POINTER(name)); }
/* Set: a new marker replaces the name's old one. The flush submits it, so a test made from
 * another context (whose flush bit flushes only that context) still sees it complete. */
static void gles_egl_set_fence(GLuint name)
{
#ifdef GLES_HOST_EGL
    if (gles_egl_is_fence(name) && gles_fence_syncs()) {
        EGLSyncKHR sync = gles_fence_api.create(gles_egl_display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            g_hash_table_insert(gles_egl_fences, GUINT_TO_POINTER(name), sync);
            glFlush();
            return;
        }
    }
#endif
    glFinish();     /* no marker: everything before the set is complete when it returns */
    if (gles_egl_is_fence(name)) g_hash_table_insert(gles_egl_fences, GUINT_TO_POINTER(name), NULL);
}
/* Test: never waits. A name without a marker (never set, or finished) tests true. */
static GLboolean gles_egl_test_fence(GLuint name)
{
    if (!gles_egl_is_fence(name)) return GL_FALSE;
#ifdef GLES_HOST_EGL
    EGLSyncKHR sync = g_hash_table_lookup(gles_egl_fences, GUINT_TO_POINTER(name));
    if (sync) {
        /* Only a fence still pending tests false: an error must not leave a game spinning. */
        if (gles_fence_api.wait(gles_egl_display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0) ==
            EGL_TIMEOUT_EXPIRED_KHR) return GL_FALSE;
        g_hash_table_insert(gles_egl_fences, GUINT_TO_POINTER(name), NULL);
    }
#endif
    return GL_TRUE;
}
/* Finish: waits for this marker only. */
static void gles_egl_finish_fence(GLuint name)
{
#ifdef GLES_HOST_EGL
    EGLSyncKHR sync = gles_egl_fences ? g_hash_table_lookup(gles_egl_fences, GUINT_TO_POINTER(name)) : NULL;
    if (sync) {
        gles_fence_api.wait(gles_egl_display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
        g_hash_table_insert(gles_egl_fences, GUINT_TO_POINTER(name), NULL);
        return;
    }
    if (gles_egl_is_fence(name) && gles_fence_syncs()) return;  /* set without a marker: already met */
#endif
    glFinish();
}
/*
 * Save states: the fence names the guest holds, and the next one to give out. A
 * fence's marker is the host GPU's and can't be saved; a restored machine's GPU
 * work is all done, so each name comes back met. Without this, a name the guest
 * set before the save tested false for ever after a load (gles_egl_test_fence:
 * an unknown name), and the iPhone 5's backboardd, which waits for its fences,
 * never drew again.
 */
#define GLES_FENCES_SAVED 65536
typedef struct {
  uint32_t
      fences; /* the field the table is saved through (gles_fences_put/get) */
} GLESFenceState;
static GLESFenceState gles_fence_state;

static int gles_fences_put(QEMUFile *f, void *pv, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc) {
  uint32_t count = gles_egl_fences ? g_hash_table_size(gles_egl_fences) : 0;
  GHashTableIter it;
  gpointer key;

  if (count > GLES_FENCES_SAVED) {
    return -E2BIG;
  }
  qemu_put_be32(f, gles_egl_next_fence);
  qemu_put_be32(f, count);
  if (count) {
    g_hash_table_iter_init(&it, gles_egl_fences);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
      qemu_put_be32(f, GPOINTER_TO_UINT(key));
    }
  }
  return qemu_file_get_error(f);
}

static int gles_fences_get(QEMUFile *f, void *pv, size_t size,
                           const VMStateField *field) {
  uint32_t next = qemu_get_be32(f), count = qemu_get_be32(f);

  if (count > GLES_FENCES_SAVED) {
    return -EINVAL;
  }
  if (!gles_egl_fences) {
    gles_egl_fences = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                            gles_fence_drop);
  }
  g_hash_table_remove_all(gles_egl_fences);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t name = qemu_get_be32(f);
    if (!name || name > next) {
      return -EINVAL;
    }
    g_hash_table_insert(gles_egl_fences, GUINT_TO_POINTER(name), NULL);
  }
  gles_egl_next_fence = next;
  return qemu_file_get_error(f);
}

static const VMStateInfo vmstate_gles_fences_info = {
    .name = "gles-fences",
    .get = gles_fences_get,
    .put = gles_fences_put,
};

static const VMStateDescription vmstate_gles_fences = {
    .name = "gles-fences",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){VMSTATE_SINGLE(fences, GLESFenceState, 0,
                                                    vmstate_gles_fences_info,
                                                    uint32_t),
                                     VMSTATE_END_OF_LIST()}};

static void gles_fences_register(void) {
  static bool done;
  if (!done) {
    done = true;
    vmstate_register(NULL, 0, &vmstate_gles_fences, &gles_fence_state);
  }
}

static GLboolean gles_egl_test_object(GLenum object, GLuint name) { glFinish(); return GL_TRUE; }
static void gles_egl_finish_object(GLenum object, GLint name) { glFinish(); }
#define glGenFencesAPPLE gles_egl_gen_fences
#define glDeleteFencesAPPLE gles_egl_delete_fences
#define glSetFenceAPPLE gles_egl_set_fence
#define glIsFenceAPPLE gles_egl_is_fence
#define glTestFenceAPPLE gles_egl_test_fence
#define glFinishFenceAPPLE gles_egl_finish_fence
#define glTestObjectAPPLE gles_egl_test_object
#define glFinishObjectAPPLE gles_egl_finish_object
