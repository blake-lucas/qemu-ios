/* gles_glsl_hoist_extensions: #extension directives after the shader's first token move to the top, the way Apple's
 * compiler read them; everything else stays byte for byte.
 *
 * Mutant Fridge Mayhem (GitHub issue 16) compiles cocos2d 2.x's draw-node shader behind cocos2d's header, which puts
 * `#extension GL_OES_standard_derivatives : enable` on line 12. ANGLE, Adreno and Mesa refused it and cocos2d aborted.
 * With a file argument the test prints that file hoisted instead, to feed a real compiler.
 *
 * SLICE hw/arm/gles-host.c fn gles_glsl_scan gles_glsl_hoist_extensions
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "slice.h"

#define COCOS_HEADER "precision mediump float;\n" \
    "uniform mat4 CC_PMatrix;\nuniform mat4 CC_MVMatrix;\nuniform mat4 CC_MVPMatrix;\nuniform vec4 CC_Time;\n" \
    "uniform vec4 CC_SinTime;\nuniform vec4 CC_CosTime;\nuniform vec4 CC_Random01;\n//CC INCLUDES END\n\n"

static unsigned lines(const char *s)
{
    unsigned n = 0;
    for (; *s; s++) {
        n += *s == '\n';
    }
    return n;
}

/* The text moves as `want` says (NULL: nothing moves); a move only adds lines, and leaves nothing to move. */
static void check(const char *name, const char *src, const char *want)
{
    g_autofree char *got = gles_glsl_hoist_extensions(src);

    if (g_strcmp0(got, want)) {
        fprintf(stderr, "%s: got\n%s\nwant\n%s\n", name, got ? got : "(NULL)", want ? want : "(NULL)");
        exit(1);
    }
    if (got) {
        g_autofree char *again = gles_glsl_hoist_extensions(got);
        if (again || lines(got) <= lines(src)) {
            fprintf(stderr, "%s: not settled after one move\n", name);
            exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        g_autofree char *text = NULL;
        if (!g_file_get_contents(argv[1], &text, NULL, NULL)) {
            return 2;
        }
        g_autofree char *out = gles_glsl_hoist_extensions(text);
        fputs(out ? out : text, stdout);
        return 0;
    }

    /* The issue's shader, shortened: the directive moves to line 1 and its own line goes empty. */
    check("cocos2d draw node",
          COCOS_HEADER "\t\t\n#extension GL_OES_standard_derivatives : enable\t\t\n#ifdef GL_ES\nvarying mediump vec2 v;\n"
          "#endif\nvoid main() { gl_FragColor = vec4(fwidth(v), 0.0, 1.0); }\n",
          "#extension GL_OES_standard_derivatives : enable\t\t\n" COCOS_HEADER "\t\t\n\n#ifdef GL_ES\n"
          "varying mediump vec2 v;\n#endif\nvoid main() { gl_FragColor = vec4(fwidth(v), 0.0, 1.0); }\n");

    /* After #version, and several keep their order. */
    check("version",
          "#version 100\nprecision mediump float;\n#extension GL_A : enable\nvarying vec2 v;\n  #  extension GL_B : require\n"
          "void main() {}\n",
          "#version 100\n#extension GL_A : enable\n#  extension GL_B : require\nprecision mediump float;\n\n"
          "varying vec2 v;\n\nvoid main() {}\n");

    /* Already first, after comments and other directives: untouched. */
    check("first", "/* licence\n * precision mediump float;\n */\n//#extension GL_X : enable\n#define A 1\n"
          "#extension GL_OES_standard_derivatives : enable\nprecision mediump float;\nvoid main() {}\n", NULL);
    check("inside #ifdef at the top",
          "#ifdef GL_OES_standard_derivatives\n#extension GL_OES_standard_derivatives : enable\n#endif\n"
          "void main() {}\n", NULL);

    /* A comment that mentions a directive is no directive, and a comment ahead of a directive is no token. */
    check("commented", "precision mediump float;\n//#extension GL_X : enable\n/*\n#extension GL_X : enable\n*/\n"
          "void main() {}\n", NULL);
    check("comment before #", "/* a */ precision mediump float; /* b */\n /* c */ #extension GL_X : enable\nvoid main() {}\n",
          "#extension GL_X : enable\n/* a */ precision mediump float; /* b */\n\nvoid main() {}\n");

    /* Late but conditional, or opening a block comment: left where it is. */
    check("late in #if", "precision mediump float;\n#if defined(GL_X)\n#extension GL_X : enable\n#endif\nvoid main() {}\n",
          NULL);
    check("late, opens a comment", "precision mediump float;\n#extension GL_X : enable /* see\nbelow */\nvoid main() {}\n",
          NULL);

    /* After a closed #if it is at the top level again. */
    check("after #endif", "#ifdef GL_ES\nprecision mediump float;\n#endif\n#extension GL_X : enable\nvoid main() {}",
          "#extension GL_X : enable\n#ifdef GL_ES\nprecision mediump float;\n#endif\n\nvoid main() {}");
    puts("gles-glsl-hoist: ok");
    return 0;
}
