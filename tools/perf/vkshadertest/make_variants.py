#!/usr/bin/env python3
"""Builds SPIR-V variants of sampler_feedback.comp (parallel-gs) to bisect which construct crashes a driver in vkCreateShaderModule.

  python make_variants.py --pgs C:/ps2x-gpu/parallel-gs --slangmosh out/x4-tools/Release/slangmosh.exe --out out/sfvar/pack [--spirv-val spirv-val.exe]

Two families, all compiled with the same tool chain and flags as the game's shaders (slangmosh -O --strip, SPIR-V 1.3):
  v0x  "destructive": the real shader with one group of constructs replaced by an equivalent-or-simpler one;
  v2x  "constructive": tiny shaders that contain only one suspect construct each (plus a trivial control).
The baseline (v00) is checked to be bit-identical to the SPIR-V the game builds from the unmodified source (FNV-1a is printed).
Variants only have to be VALID SPIR-V that a driver can compile; they are NOT meant to render (the test only calls vkCreateShaderModule).
Writes <out>/<name>.spv, <out>/variants.txt (name<TAB>description) and runs spirv-val on each when --spirv-val is given.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys


def sub(text, old, new, count=1):
    if text.count(old) != count:
        sys.exit("variant source out of sync with sampler_feedback.comp: %r found %d times" % (old[:60], text.count(old)))
    return text.replace(old, new)


STRUCT_OLD = """struct TextureAnalysis
{
    IndirectDispatch indirect_dispatch_va;
    WorkGroupBuffer indirect_workgroups_va;
    BitMaskBuffer indirect_bitmask_va;
    uvec2 size_minus_1;
    u16vec2 base;"""

ALLOC_OLD = """void allocate_block(IndirectDispatch dispatch_va, WorkGroupBuffer workgroup_va, BitMaskBuffer bitmask_va,
                    uvec2 block, uint stride, uint layers)
{"""

UBO_OLD = """layout(set = 1, binding = 0, std140) uniform AnalysisBuffer
{
    TextureAnalysis data[1024];
} texture_analysis;"""

ATOMICS_OLD = """    uint old_mask = atomicOr(bitmask_va.data[linear_block / 32], mask);

    if ((mask & old_mask) == 0u)
    {
        uint old_value = atomicExchange(workgroup_va.valid, 1u);
        if (old_value == 0u)
        {
            dispatch_va.dispatch[1] = 1;
            dispatch_va.dispatch[2] = layers;
        }

        uint index = atomicAdd(dispatch_va.dispatch[0], 1u);
        workgroup_va.workgroup_indices[index] = block;
    }"""

ATOMICS_NEW = """    uint old_mask = bitmask_va.data[linear_block / 32];
    bitmask_va.data[linear_block / 32] = old_mask | mask;

    if ((mask & old_mask) == 0u)
    {
        uint old_value = workgroup_va.valid;
        workgroup_va.valid = 1u;
        if (old_value == 0u)
        {
            dispatch_va.dispatch[1] = 1;
            dispatch_va.dispatch[2] = layers;
        }

        uint index = dispatch_va.dispatch[0];
        dispatch_va.dispatch[0] = index + 1u;
        workgroup_va.workgroup_indices[index] = block;
    }"""

DEFERRED_LOOP = "    for (uint i = 0; i < shared_large_access_count; i++)\n"

MINI_HEAD = """#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
layout(local_size_x = 256) in;
layout(buffer_reference, buffer_reference_align = 16, std430) buffer BitMask { uint data[]; };
layout(set = 0, binding = 0, std430) buffer Out { uvec4 data[]; } out_buf;
"""


def variants(src):
    v = []

    def add(name, desc, text):
        v.append((name, desc, text))

    add("v00_base", "shader real sin tocar (control: debe fallar como el #22)", src)

    t = sub(src, "    u16vec2 base;", "    uvec2 base;")
    add("v01_ubo_sin_int16", "UBO sin u16vec2 (base como uvec2): sin UniformAndStorageBuffer16BitAccess", t)

    ptr = sub(src, STRUCT_OLD, STRUCT_OLD.replace("IndirectDispatch indirect_dispatch_va;", "uvec2 indirect_dispatch_va;")
              .replace("WorkGroupBuffer indirect_workgroups_va;", "uvec2 indirect_workgroups_va;").replace("BitMaskBuffer indirect_bitmask_va;", "uvec2 indirect_bitmask_va;"))
    ptr = sub(ptr, ALLOC_OLD, """void allocate_block(uvec2 dispatch_u, uvec2 workgroup_u, uvec2 bitmask_u,
                    uvec2 block, uint stride, uint layers)
{
    IndirectDispatch dispatch_va = IndirectDispatch(dispatch_u);
    WorkGroupBuffer workgroup_va = WorkGroupBuffer(workgroup_u);
    BitMaskBuffer bitmask_va = BitMaskBuffer(bitmask_u);""")
    ptr = sub(ptr, "#extension GL_EXT_spirv_intrinsics : require", "#extension GL_EXT_spirv_intrinsics : require\n#extension GL_EXT_buffer_reference_uvec2 : require")
    add("v02_ubo_punteros_como_uvec2", "UBO con las 3 direcciones como uvec2 (convertidas a puntero despues): sin punteros PhysicalStorageBuffer dentro del UBO", ptr)

    both = sub(ptr, "    u16vec2 base;", "    uvec2 base;")
    add("v03_ubo_sin_int16_ni_punteros", "las dos anteriores a la vez: UBO solo con enteros de 32 bits", both)

    ssbo = sub(src, UBO_OLD, """layout(set = 1, binding = 0, std430) readonly buffer AnalysisBuffer
{
    TextureAnalysis data[];
} texture_analysis;""")
    add("v04_analisis_en_ssbo", "TextureAnalysis (con punteros y u16vec2) en un SSBO std430 en vez de UBO std140: sin clase Uniform", ssbo)

    add("v05_sin_atomicas", "allocate_block sin atomicOr/Exchange/Add (lecturas y escrituras normales a traves de los punteros)", sub(src, ATOMICS_OLD, ATOMICS_NEW))

    idx = src.index(DEFERRED_LOOP)
    add("v06_sin_bucle_diferido", "sin el bucle final sobre memoria compartida (large access): sin lecturas de shared tras barrier",
        src[:idx].rstrip() + "\n}\n")

    nc = src
    nc = re.sub(r"spvNClamp\(([^;]*?), vec2\(-2047\.0\), vec2\(2047\.0\)\)", r"clamp(\1, vec2(-2047.0), vec2(2047.0))", nc)
    if nc == src:
        sys.exit("no spvNClamp call replaced")
    add("v07_sin_nclamp", "clamp() normal en lugar de la instruccion GLSL.std.450 NClamp (spirv_instruction)", nc)

    nm = sub(src, """    analysis = texture_analysis.data[tex_index];
    if ((analysis.flags & ENABLED_BIT) == 0)
        return false;
""", """    analysis = texture_analysis.data[tex_index];
    if ((analysis.flags & ENABLED_BIT) == 0)
        return false;
    lo = ivec2(analysis.base);
    hi = ivec2(analysis.size_minus_1 >> 3);
    return true;
""")
    add("v08_sin_calculo_uv", "compute_tex_index_and_damage_region devuelve justo despues de leer el UBO: se elimina todo el calculo de baricentricas/UV (y NClamp)", nm)

    add("v09_array_fijo_en_puntero", "WorkGroupBuffer con array de tamano fijo en lugar de array sin tamano (runtime array dentro de un puntero)",
        sub(src, "layout(offset = 16) uvec2 workgroup_indices[];", "layout(offset = 16) uvec2 workgroup_indices[65536];"))

    mini = {}
    mini["v20_min_trivial"] = ("control: modulo vacio (si falla, falla cualquier modulo en este driver)",
                               "#version 450\nlayout(local_size_x = 256) in;\nvoid main() {}\n")
    mini["v21_min_puntero_en_ubo"] = ("solo: un puntero PhysicalStorageBuffer guardado en un UBO std140 + atomicOr a traves de el",
                                      MINI_HEAD + """struct A { BitMask va; uvec2 size; uvec2 pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
void main() { A a = u.data[gl_GlobalInvocationID.x & 1023u]; atomicOr(a.va.data[a.size.x], 1u); out_buf.data[0] = uvec4(a.size, 0, 0); }
""")
    mini["v22_min_u16_en_ubo"] = ("solo: u16vec2 en un UBO std140 (UniformAndStorageBuffer16BitAccess), sin punteros",
                                  MINI_HEAD + """struct A { uvec2 size; u16vec2 base; uint flags; uint pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
void main() { A a = u.data[gl_GlobalInvocationID.x & 1023u]; out_buf.data[gl_GlobalInvocationID.x] = uvec4(a.size, uvec2(a.base)); }
""")
    mini["v23_min_puntero_y_u16_en_ubo"] = ("ambos: puntero PSB y u16vec2 en el mismo UBO std140 (como TextureAnalysis)",
                                            MINI_HEAD + """struct A { BitMask va; uvec2 size; u16vec2 base; uint flags; uint pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
void main() { A a = u.data[gl_GlobalInvocationID.x & 1023u]; atomicOr(a.va.data[a.size.x], uint(a.base.x)); out_buf.data[0] = uvec4(a.size, a.flags, 0); }
""")
    mini["v24_min_puntero_en_ssbo"] = ("control: puntero PSB guardado en un SSBO (no UBO) + atomicOr",
                                       MINI_HEAD + """struct A { BitMask va; uvec2 size; uvec2 pad; };
layout(set = 1, binding = 0, std430) readonly buffer U { A data[]; } u;
void main() { A a = u.data[gl_GlobalInvocationID.x]; atomicOr(a.va.data[a.size.x], 1u); out_buf.data[0] = uvec4(a.size, 0, 0); }
""")
    mini["v25_min_copia_local_con_puntero"] = ("copia local (variable de funcion) de un struct con punteros PSB leido de un UBO, pasado por parametro out",
                                               MINI_HEAD + """struct A { BitMask va; uvec2 size; uvec2 pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
bool get(uint i, out A a) { if (i >= 1024u) return false; a = u.data[i]; return true; }
void main() { A a; if (get(gl_GlobalInvocationID.x, a)) { atomicOr(a.va.data[a.size.x], 1u); out_buf.data[0] = uvec4(a.size, 0, 0); } }
""")
    mini["v26_min_u16_ubo_sin_punteros"] = ("solo u16vec2 en un UBO std140 y NINGUNA declaracion de buffer_reference (v22 declara un puntero sin usarlo, lo que anade PhysicalStorageBufferAddresses)",
                                            """#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
layout(local_size_x = 256) in;
layout(set = 0, binding = 0, std430) buffer Out { uvec4 data[]; } out_buf;
struct A { uvec2 size; u16vec2 base; uint flags; uint pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
void main() { A a = u.data[gl_GlobalInvocationID.x & 1023u]; out_buf.data[gl_GlobalInvocationID.x] = uvec4(a.size, uvec2(a.base)); }
""")
    mini["v30_min_phi_sin_undef"] = ("como v25 pero el parametro out se inicializa con un puntero nulo: el OpPhi de punteros PSB sigue, pero sin OpUndef de puntero",
                                     MINI_HEAD.replace("#extension GL_EXT_buffer_reference : require", "#extension GL_EXT_buffer_reference : require\n#extension GL_EXT_buffer_reference_uvec2 : require") + """struct A { BitMask va; uvec2 size; uvec2 pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
bool get(uint i, out A a) { a = A(BitMask(uvec2(0)), uvec2(0), uvec2(0)); if (i >= 1024u) return false; a = u.data[i]; return true; }
void main() { A a; if (get(gl_GlobalInvocationID.x, a)) { atomicOr(a.va.data[a.size.x], 1u); out_buf.data[0] = uvec4(a.size, 0, 0); } }
""")
    mini["v31_min_seleccion_de_dos_punteros"] = ("seleccion entre dos punteros PSB cargados del UBO, sin parametros out ni valores indefinidos",
                                                 MINI_HEAD + """struct A { BitMask va; uvec2 size; uvec2 pad; };
layout(set = 1, binding = 0, std140) uniform U { A data[1024]; } u;
void main() { uint i = gl_GlobalInvocationID.x & 1023u; A a = u.data[i]; A b = u.data[(i + 1u) & 1023u]; BitMask p = ((a.size.x & 1u) != 0u) ? a.va : b.va;
              atomicOr(p.data[a.size.y], 1u); out_buf.data[0] = uvec4(a.size, 0, 0); }
""")
    for n, (d, t) in mini.items():
        add(n, d, t)
    return v


def read_bank(hpp):
    t = open(hpp).read()
    m = re.search(r"spirv_bank\[\]\s*=\s*\{(.*?)\};", t, re.S)
    words = [int(x, 16) if x.lower().startswith("0x") else int(x) for x in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", m.group(1))]
    entries = [(int(a), int(b)) for a, b in re.findall(r"request_program\(spirv_bank \+ (\d+), (\d+)", t)]
    return words, entries


def fnv(b):
    h = 1469598103934665603
    for i in range(len(b) // 4):
        h = ((h ^ struct.unpack_from("<I", b, i * 4)[0]) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pgs", required=True, help="parallel-gs checkout (the one the game is built from)")
    ap.add_argument("--slangmosh", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--spirv-val")
    a = ap.parse_args()
    a.pgs, a.slangmosh, a.out = (os.path.abspath(x) for x in (a.pgs, a.slangmosh, a.out))
    if a.spirv_val:
        a.spirv_val = os.path.abspath(a.spirv_val)

    work = os.path.join(a.out, "_work")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(os.path.join(a.pgs, "gs", "shaders"), os.path.join(work, "shaders"))
    os.makedirs(os.path.join(work, "Granite", "assets", "shaders"), exist_ok=True)
    shutil.copytree(os.path.join(a.pgs, "Granite", "assets", "shaders", "inc"), os.path.join(work, "Granite", "assets", "shaders", "inc"))
    src = open(os.path.join(work, "shaders", "sampler_feedback.comp"), newline="").read().replace("\r\n", "\n")

    vs = variants(src)
    entries = []
    for name, desc, text in vs:
        open(os.path.join(work, "shaders", name + ".comp"), "w", newline="\n").write(text)
        entries.append('{ "name": "%s", "path": "%s.comp", "compute": true }' % (name, name))
    open(os.path.join(work, "shaders", "variants.json"), "w").write('{ "include": [ "../Granite/assets/shaders/inc" ], "shaders": [\n' + ",\n".join(entries) + "\n] }\n")
    r = subprocess.run([a.slangmosh, "variants.json", "-O", "--strip", "--namespace", "ParallelGS", "--output", "variants.hpp", "--output-interface", "variants_iface.hpp"],
                       cwd=os.path.join(work, "shaders"), capture_output=True, text=True)
    sys.stdout.write(r.stdout + r.stderr)
    if r.returncode:
        sys.exit("slangmosh failed")
    words, offs = read_bank(os.path.join(work, "shaders", "variants.hpp"))
    if len(offs) != len(vs):
        sys.exit("expected %d programs in the bank, found %d" % (len(vs), len(offs)))

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "variants.txt"), "w", encoding="utf-8") as lst:
        for (name, desc, _), (off, size) in zip(vs, offs):
            b = struct.pack("<%dI" % (size // 4), *words[off:off + size // 4])
            path = os.path.join(a.out, name + ".spv")
            open(path, "wb").write(b)
            ok = "-"
            if a.spirv_val:
                ok = "val OK" if subprocess.run([a.spirv_val, "--target-env", "vulkan1.3", path], capture_output=True).returncode == 0 else "val FAILED"
            print("%-34s %6d bytes  fnv %016x  %s" % (name, size, fnv(b), ok))
            lst.write("%s\t%s\n" % (name, desc))
    print("written to", a.out)


if __name__ == "__main__":
    main()
