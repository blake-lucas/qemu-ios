/* glIsEnabled on a client array answers the guest's own enable state, not the host's, which with the array cache
 * changes only at a draw.
 *
 * Dino Jump (GitHub issue 23) ends its scene pass with glDisableClientState on the three arrays it enabled, then
 * saves state with glIsEnabled and enables only what it finds off. The host still held the arrays enabled from the
 * last draw, said so, and the game never re-enabled the vertex array: the frame's full-screen quad drew nothing and
 * the picture stayed on the level's first frame. The texture-coordinate array is the client-active unit's.
 *
 * SLICE hw/arm/gles-host.c fn gles_client_array_enabled
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_POINT_SIZE_ARRAY_OES 0x8B9C
#define GL_WEIGHT_ARRAY_OES 0x86AD
#define GL_MATRIX_INDEX_ARRAY_OES 0x8844
#define GL_BLEND 0x0BE2
typedef struct {
    bool enabled;
} GLESArray;
static struct {
    GLESArray vertex, color, normal, pointsize, weight, matrix_index, texcoord[4];
    unsigned client_active_unit;
} gh;
#include "slice.h"

static bool query(uint32_t cap)
{
    bool on = true;     /* untouched when the cap is not a client array */

    assert(gles_client_array_enabled(cap, &on));
    return on;
}

int main(void)
{
    bool on = false;

    /* Everything off to begin with, as a context starts. */
    assert(!query(GL_VERTEX_ARRAY) && !query(GL_COLOR_ARRAY) && !query(GL_NORMAL_ARRAY));
    assert(!query(GL_TEXTURE_COORD_ARRAY) && !query(GL_POINT_SIZE_ARRAY_OES));
    assert(!query(GL_WEIGHT_ARRAY_OES) && !query(GL_MATRIX_INDEX_ARRAY_OES));

    /* The scene pass enables three; the end of it disables them again: all three read off. */
    gh.vertex.enabled = gh.color.enabled = gh.texcoord[0].enabled = true;
    assert(query(GL_VERTEX_ARRAY) && query(GL_COLOR_ARRAY) && query(GL_TEXTURE_COORD_ARRAY));
    gh.vertex.enabled = gh.color.enabled = gh.texcoord[0].enabled = false;
    assert(!query(GL_VERTEX_ARRAY) && !query(GL_COLOR_ARRAY) && !query(GL_TEXTURE_COORD_ARRAY));

    /* Texture coordinates answer for the client-active unit alone. */
    gh.texcoord[1].enabled = true;
    assert(!query(GL_TEXTURE_COORD_ARRAY));
    gh.client_active_unit = 1;
    assert(query(GL_TEXTURE_COORD_ARRAY));

    /* The OES arrays have flags of their own. */
    gh.pointsize.enabled = gh.weight.enabled = gh.matrix_index.enabled = gh.normal.enabled = true;
    assert(query(GL_POINT_SIZE_ARRAY_OES) && query(GL_WEIGHT_ARRAY_OES) && query(GL_MATRIX_INDEX_ARRAY_OES) &&
           query(GL_NORMAL_ARRAY));

    /* Any other capability is the host's to answer, and the answer slot is left alone. */
    on = true;
    assert(!gles_client_array_enabled(GL_BLEND, &on) && on);

    puts("PASS: glIsEnabled on a client array reads the guest's enable state");
    return 0;
}
