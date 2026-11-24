#!/usr/bin/env python3
"""Generate the GL call marshalling shared by guest and host.

One table (CALLS plus the families built in families()) drives four
generated files, which are checked in:

  qemu/include/hw/display/ppcosx-gl-ops.h      opcode numbers (guest + host)
  qemu/hw/display/ppcosx-gl-decode.c.inc       host: decode a command, call GL
  guest/libppcosxgl/gl_encode.inc              guest: gl*() entry points
  guest/PPCOSXGL/gl_interpose.inc              guest: wrappers around Apple's
                                               gl*() for the interposer

Wire format of one call: the usual 8-byte command header, then each argument
as big-endian 32-bit words. Doubles take two words (high, low); every other
scalar or vector element takes one. Fixed-size vectors are sent inline.
Opcode numbers are positional, and both sides are always built from the same
table, so entries may be added anywhere.

Argument codes: one letter per scalar (see SCALARS); a capital letter and a
count for a const vector (F3, D16, see VECTORS). A trailing "p" (F4p) marks
a parameter vector whose real length depends on the pname argument before
it; the guest reads only that many elements and zero-fills the rest.

A name ending in "!" is a drawing call. The interposer sends those to the
host only; everything else it also passes to Apple's software renderer, so
that renderer's state stays right and can answer queries without ever
rasterising anything.

FALLBACK lists calls the host path does not implement and that cannot be
ignored. The interposer wraps each so that the first use hands the context
back to Apple's renderer for good.

tools/tiger-gl-symbols.txt, when present, is the list of gl* functions the
guest OS exports; calls it lacks are left out of the interposer.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

# code -> (C type, words, wire kind)
SCALARS = {
    "e": ("GLenum", 1, "u"), "i": ("GLint", 1, "u"), "u": ("GLuint", 1, "u"),
    "s": ("GLsizei", 1, "u"), "b": ("GLbitfield", 1, "u"),
    "B": ("GLboolean", 1, "u"), "y": ("GLubyte", 1, "u"),
    "k": ("GLbyte", 1, "u"), "h": ("GLshort", 1, "u"),
    "w": ("GLushort", 1, "u"),
    "f": ("GLfloat", 1, "f"), "c": ("GLclampf", 1, "f"),
    "d": ("GLdouble", 2, "d"), "C": ("GLclampd", 2, "d"),
}
VECTORS = {
    "F": ("GLfloat", 1, "f"), "D": ("GLdouble", 2, "d"),
    "I": ("GLint", 1, "u"), "U": ("GLuint", 1, "u"),
    "S": ("GLshort", 1, "u"), "W": ("GLushort", 1, "u"),
    "K": ("GLbyte", 1, "u"), "Y": ("GLubyte", 1, "u"),
}

CALLS = """
ClearColor c c c c
Clear! b
ClearDepth C
ClearStencil i
Viewport i i s s
Scissor i i s s
Enable e
Disable e
DepthFunc e
DepthMask B
DepthRange C C
BlendFunc e e
AlphaFunc e c
CullFace e
FrontFace e
ShadeModel e
PolygonMode e e
PolygonOffset f f
LineWidth f
PointSize f
ColorMask B B B B
StencilFunc e i u
StencilOp e e e
StencilMask u
LogicOp e
Hint e e
MatrixMode e
LoadIdentity
PushMatrix
PopMatrix
LoadMatrixf F16
MultMatrixf F16
LoadMatrixd D16
MultMatrixd D16
Translatef f f f
Translated d d d
Rotatef f f f f
Rotated d d d d
Scalef f f f
Scaled d d d
Frustum d d d d d d
Ortho d d d d d d
Begin! e
End!
Lightf e e f
Lightfv e e F4p
Lighti e e i
Lightiv e e I4p
LightModelf e f
LightModeli e i
LightModelfv e F4p
LightModeliv e I4p
Materialf e e f
Materialfv e e F4p
Materiali e e i
Materialiv e e I4p
ColorMaterial e e
Fogf e f
Fogi e i
Fogfv e F4p
Fogiv e I4p
NewList u e
EndList
CallList! u
DeleteLists u s
ListBase u
PushAttrib b
PopAttrib
BindTexture e u
TexParameteri e e i
TexParameterf e e f
TexParameterfv e e F4p
TexParameteriv e e I4p
TexEnvi e e i
TexEnvf e e f
TexEnvfv e e F4p
TexEnviv e e I4p
TexGeni e e i
TexGenf e e f
TexGend e e d
TexGenfv e e F4p
TexGendv e e D4p
TexGeniv e e I4p
ClipPlane e D4
BlendEquation e
BlendColor c c c c
BlendFuncSeparate e e e e
LineStipple i w
EdgeFlag B
LoadTransposeMatrixf F16
MultTransposeMatrixf F16
LoadTransposeMatrixd D16
MultTransposeMatrixd D16
SampleCoverage c B
PointParameterf e f
SecondaryColor3f f f f
SecondaryColor3ub y y y
FogCoordf f
ActiveTexture e
ActiveTextureARB e
CopyTexImage2D! e i e i i s s i
CopyTexSubImage2D! e i i i i i s s
GenerateMipmapEXT e
Flush
Finish
"""

# Interposer wrappers written by hand in ppcosxgl_interpose.c.
CUSTOM = {"Viewport", "NewList", "EndList", "Flush", "Finish"}

FALLBACK = """
void glArrayElement(GLint i)
void glDrawElementArrayAPPLE(GLenum mode, GLint first, GLsizei count)
void glDrawRangeElementArrayAPPLE(GLenum mode, GLuint start, GLuint end, GLint first, GLsizei count)
void glEnableVertexAttribArray(GLuint index)
void glEnableVertexAttribArrayARB(GLuint index)
void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, const GLubyte *bitmap)
void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type)
void glAccum(GLenum op, GLfloat value)
void glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
void glEvalMesh1(GLenum mode, GLint i1, GLint i2)
void glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2)
void glEvalPoint1(GLint i)
void glEvalPoint2(GLint i, GLint j)
void glEvalCoord1f(GLfloat u)
void glEvalCoord1d(GLdouble u)
void glEvalCoord2f(GLfloat u, GLfloat v)
void glEvalCoord2d(GLdouble u, GLdouble v)
void glEvalCoord1fv(const GLfloat *u)
void glEvalCoord1dv(const GLdouble *u)
void glEvalCoord2fv(const GLfloat *u)
void glEvalCoord2dv(const GLdouble *u)
void glTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels)
void glTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels)
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data)
void glCompressedTexImage2DARB(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data)
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid *data)
void glCompressedTexSubImage2DARB(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid *data)
void glProgramStringARB(GLenum target, GLenum format, GLsizei len, const GLvoid *string)
void glUseProgramObjectARB(GLhandleARB programObj)
void glUseProgram(GLuint program)
void glPolygonStipple(const GLubyte *mask)
void glPixelTransferf(GLenum pname, GLfloat param)
void glPixelTransferi(GLenum pname, GLint param)
"""

BANNER = "/* Generated by tools/gen_gl.py - do not edit. */\n"

SUFFIX = {  # immediate-mode type suffix -> (scalar code, vector code)
    "b": ("k", "K"), "s": ("h", "S"), "i": ("i", "I"), "f": ("f", "F"),
    "d": ("d", "D"), "ub": ("y", "Y"), "us": ("w", "W"), "ui": ("u", "U"),
}


def families():
    """The glVertex / glColor / ... families, in every type and size."""
    lines = []

    def family(base, sizes, types, draw=False, lead="", tail=""):
        bang = "!" if draw else ""
        for n in sizes:
            for t in types:
                scalar, vector = SUFFIX[t]
                lines.append(f"{base}{n}{t}{tail}{bang} {lead}"
                             + " ".join([scalar] * n))
                lines.append(f"{base}{n}{t}v{tail}{bang} {lead}{vector}{n}")

    family("Vertex", (2, 3, 4), "sifd", draw=True)
    family("Color", (3, 4), ("b", "s", "i", "f", "d", "ub", "us", "ui"))
    family("Normal", (3,), "bsifd")
    family("TexCoord", (1, 2, 3, 4), "sifd")
    family("MultiTexCoord", (1, 2, 3, 4), "sifd", lead="e ")
    family("MultiTexCoord", (1, 2, 3, 4), "sifd", lead="e ", tail="ARB")
    for t in "sifd":
        scalar, vector = SUFFIX[t]
        lines.append(f"Rect{t}! " + " ".join([scalar] * 4))
        lines.append(f"Rect{t}v! {vector}2 {vector}2")
    return lines


def parse():
    calls = []
    for line in CALLS.strip().splitlines() + families():
        name, *codes = line.split()
        draw = name.endswith("!")
        name = name.rstrip("!")
        args = []
        for n, code in enumerate(codes):
            if code[0] in VECTORS:
                ctype, words, kind = VECTORS[code[0]]
                by_pname = code.endswith("p")
                count = int(code[1:].rstrip("p"))
                args.append((f"a{n}", ctype, words, kind, count, by_pname))
            else:
                ctype, words, kind = SCALARS[code]
                args.append((f"a{n}", ctype, words, kind, 0, False))
        calls.append((name, args, draw))
    return calls


def parse_fallback():
    out = []
    for line in FALLBACK.strip().splitlines():
        m = re.match(r"void (gl\w+)\((.*)\)$", line)
        name, params = m.group(1), m.group(2)
        names = [re.search(r"(\w+)$", p.strip()).group(1)
                 for p in params.split(",")]
        out.append((name, params, names))
    return out


def guest_symbols():
    path = os.path.join(ROOT, "tools", "tiger-gl-symbols.txt")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return set(f.read().split())


def nwords(args):
    return sum(words * (count or 1) for _, _, words, _, count, _ in args)


def gen_ops(calls):
    out = [BANNER, "#ifndef HW_DISPLAY_PPCOSX_GL_OPS_H",
           "#define HW_DISPLAY_PPCOSX_GL_OPS_H", "", "enum {"]
    for n, (name, _, _) in enumerate(calls):
        out.append(f"    PPCOSX_GL_{name} = 0x{0x1000 + n:04x},")
    out += [f"    PPCOSX_GL_OP_END = 0x{0x1000 + len(calls):04x}", "};", "",
            "#endif", ""]
    return "\n".join(out)


def gen_decode(calls):
    out = [BANNER,
           "/* Call names by opcode, for tracing. */",
           "static const char *const ppcosx_gl_names[] = {"]
    out += [f'    "gl{name}",' for name, _, _ in calls]
    out += ["};", "",
           "/* p points at the first argument word; words = argument words. */",
           "static uint32_t ppcosx_gl_decode(uint32_t op, const uint8_t *p,",
           "                                 uint32_t words)", "{",
           "    switch (op) {"]
    for name, args, _ in calls:
        out.append(f"    case PPCOSX_GL_{name}: {{")
        out.append(f"        if (words != {nwords(args)}) {{")
        out.append("            return PPCOSX_GPU_ST_BAD_ARG;")
        out.append("        }")
        pos, params = 0, []
        for arg, ctype, words, kind, count, _ in args:
            getter = {"u": "rd_u", "f": "rd_f", "d": "rd_d"}[kind]
            if count:
                out.append(f"        {ctype} {arg}[{count}];")
                out.append(f"        for (int k = 0; k < {count}; k++) {{")
                out.append(f"            {arg}[k] = {getter}(p + {pos * 4}"
                           f" + k * {words * 4});")
                out.append("        }")
                pos += words * count
            else:
                out.append(f"        {ctype} {arg} = {getter}(p + {pos * 4});")
                pos += words
            params.append(arg)
        out.append(f"        gl{name}({', '.join(params)});")
        out.append("        return PPCOSX_GPU_ST_OK;")
        out.append("    }")
    out += ["    default:", "        return PPCOSX_GPU_ST_UNSUPPORTED;",
            "    }", "}", ""]
    return "\n".join(out)


def c_params(args):
    return ", ".join(
        f"const {ctype} *{arg}" if count else f"{ctype} {arg}"
        for arg, ctype, _, _, count, _ in args) or "void"


def put_args(args, indent):
    out = []
    for n, (arg, ctype, words, kind, count, by_pname) in enumerate(args):
        putter = {"u": "wr_u", "f": "wr_f", "d": "wr_d"}[kind]
        if by_pname:
            pname = args[n - 1][0]
            out.append(f"{indent}{{ int k, n = px_pname_count({pname}); "
                       f"for (k = 0; k < {count}; k++) "
                       f"w = {putter}(w, k < n ? {arg}[k] : 0); }}")
        elif count:
            out.append(f"{indent}{{ int k; for (k = 0; k < {count}; k++) "
                       f"w = {putter}(w, {arg}[k]); }}")
        else:
            out.append(f"{indent}w = {putter}(w, {arg});")
    return out


def gen_encode(calls):
    """gl*() definitions; the includer defines PX_EMIT(opcode, words)."""
    out = [BANNER]
    for name, args, _ in calls:
        out.append(f"void gl{name}({c_params(args)})")
        out.append("{")
        if args:
            out.append(f"    uint32_t *w = PX_EMIT(PPCOSX_GL_{name}, "
                       f"{nwords(args)});")
            out.append("    if (!w) return;")
            out += put_args(args, "    ")
        else:
            out.append(f"    PX_EMIT(PPCOSX_GL_{name}, 0);")
        out.append("}")
        out.append("")
    return "\n".join(out)


def gen_interpose(calls, fallback, symbols):
    """pxi_gl*() wrappers plus the table of what they replace.

    The includer defines pxi_current(), returning the PXConn for the
    calling thread's GL context or NULL if that context is not redirected,
    and pxi_unsupported(name), which stops redirecting the current context.
    """
    out = [BANNER]
    table = []

    def known(name):
        return symbols is None or f"gl{name}" in symbols

    for name, args, draw in calls:
        if not known(name):
            continue
        table.append(f"    PXI_REPLACE(gl{name})")
        if name in CUSTOM:
            continue
        passed = ", ".join(arg for arg, *_ in args)
        out.append(f"static void pxi_gl{name}({c_params(args)})")
        out.append("{")
        out.append("    PXConn *px = pxi_current();")
        out.append("    if (px) {")
        if name == "Begin":
            out.append("        pxi_before_draw(px);")
        if args:
            out.append(f"        uint32_t *w = px_emit(px, PPCOSX_GL_{name}, "
                       f"{nwords(args)});")
            out.append("        if (w) {")
            out += put_args(args, "            ")
            out.append("        }")
        else:
            out.append(f"        px_emit(px, PPCOSX_GL_{name}, 0);")
        if draw:
            # While a display list is being compiled the software renderer
            # records the call too, so the list is whole if we ever fall
            # back to it.
            out.append("        if (!px->compiling) return;")
        out.append("    }")
        out.append(f"    gl{name}({passed});")
        out.append("}")
        out.append("")

    for name, params, names in fallback:
        if not known(name[2:]):
            continue
        table.append(f"    PXI_REPLACE({name})")
        out.append(f"static void pxi_{name}({params})")
        out.append("{")
        out.append(f"    pxi_unsupported(\"{name}\");")
        out.append(f"    {name}({', '.join(names)});")
        out.append("}")
        out.append("")

    out.append("#define PXI_GL_REPLACEMENTS \\")
    out.append(" \\\n".join(table))
    out.append("")
    return "\n".join(out)


def write(path, text):
    path = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    print(path)


def main():
    calls = parse()
    names = [name for name, _, _ in calls]
    assert len(names) == len(set(names)), "duplicate call in table"
    symbols = guest_symbols()
    write("qemu/include/hw/display/ppcosx-gl-ops.h", gen_ops(calls))
    write("qemu/hw/display/ppcosx-gl-decode.c.inc", gen_decode(calls))
    write("guest/libppcosxgl/gl_encode.inc", gen_encode(calls))
    write("guest/PPCOSXGL/gl_interpose.inc",
          gen_interpose(calls, parse_fallback(), symbols))
    print(f"{len(calls)} calls")


if __name__ == "__main__":
    main()
