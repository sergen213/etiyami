#!/usr/bin/env python3
"""Generate the FidelityFX Vulkan shader-blob accessor (FSR3 upscaler, frame interpolation, optical flow)
from compiled SPIR-V.

Inputs are named <shader>.spv, where <shader> is the upstream pass file stem; FSR3 upscaler inputs add
.h<H>s<S> before .spv, the FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT/APPLY_SHARPENING values. Every other
permutation option is fixed (yami_ffx_clamp_capabilities makes the SDK request only this set).
Reflection mirrors the SDK ffx_sc GLSL path (SPIRV-Reflect ExtractReflectionData).
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import struct
import tempfile

# Per effect: (FfxEffect, pass enum type, label, permutation suffixes, valid-options and variant C++ expressions,
# passes as (pass enumerators, shader stem)) mirroring src/backends/shared/blob_accessors/ffx_<effect>_shaderblobs.cpp.
EFFECTS = (
    ("FFX_EFFECT_FSR3UPSCALER", "FfxFsr3UpscalerPass", "FSR3 upscaler",
     # variant = HDR | sharpen << 1
     (".h0s0", ".h1s0", ".h0s1", ".h1s1"),
     "(permutationOptions & ~(FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT | FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING)) == "
     "FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS",
     "(FFX_CONTAINS_FLAG(permutationOptions, FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT) ? 1 : 0) | "
     "(FFX_CONTAINS_FLAG(permutationOptions, FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING) ? 2 : 0)",
     ((("FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS",), "ffx_fsr3upscaler_prepare_inputs_pass"),
      (("FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID",), "ffx_fsr3upscaler_luma_pyramid_pass"),
      (("FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID",), "ffx_fsr3upscaler_shading_change_pyramid_pass"),
      (("FFX_FSR3UPSCALER_PASS_SHADING_CHANGE",), "ffx_fsr3upscaler_shading_change_pass"),
      (("FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY",), "ffx_fsr3upscaler_prepare_reactivity_pass"),
      (("FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY",), "ffx_fsr3upscaler_luma_instability_pass"),
      (("FFX_FSR3UPSCALER_PASS_ACCUMULATE", "FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN"), "ffx_fsr3upscaler_accumulate_pass"),
      (("FFX_FSR3UPSCALER_PASS_RCAS",), "ffx_fsr3upscaler_rcas_pass"),
      (("FFX_FSR3UPSCALER_PASS_DEBUG_VIEW",), "ffx_fsr3upscaler_debug_view_pass"),
      (("FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE",), "ffx_fsr3upscaler_autogen_reactive_pass"))),
    ("FFX_EFFECT_FRAMEINTERPOLATION", "FfxFrameInterpolationPass", "frame interpolation", ("",),
     "permutationOptions == FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS", "0",
     ((("FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE",), "ffx_frameinterpolation_reconstruct_and_dilate_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_SETUP",), "ffx_frameinterpolation_setup_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH",), "ffx_frameinterpolation_reconstruct_previous_depth_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD",), "ffx_frameinterpolation_game_motion_vector_field_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD",), "ffx_frameinterpolation_optical_flow_vector_field_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK",), "ffx_frameinterpolation_disocclusion_mask_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION",), "ffx_frameinterpolation_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID",), "ffx_frameinterpolation_compute_inpainting_pyramid_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_INPAINTING",), "ffx_frameinterpolation_inpainting_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID",),
       "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass"),
      (("FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW",), "ffx_frameinterpolation_debug_view_pass"))),
    ("FFX_EFFECT_OPTICALFLOW", "FfxOpticalflowPass", "optical flow", ("",), "permutationOptions == 0", "0",
     ((("FFX_OPTICALFLOW_PASS_PREPARE_LUMA",), "ffx_opticalflow_prepare_luma_pass"),
      (("FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID",), "ffx_opticalflow_compute_luminance_pyramid_pass"),
      (("FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM",), "ffx_opticalflow_generate_scd_histogram_pass"),
      (("FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE",), "ffx_opticalflow_compute_scd_divergence_pass"),
      (("FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5",), "ffx_opticalflow_compute_optical_flow_advanced_pass_v5"),
      (("FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5",), "ffx_opticalflow_filter_optical_flow_pass_v5"),
      (("FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5",), "ffx_opticalflow_scale_optical_flow_advanced_pass_v5"))),
)
# FfxShaderBlob order (ffx_types.h) and SDK category names.
CATEGORIES = ("CBV", "TextureSRV", "TextureUAV", "BufferSRV", "BufferUAV", "Sampler", "RTAccelerationStructure")

# SPIR-V enumerants.
OP_NAME, OP_DECORATE, OP_MEMBER_DECORATE = 5, 71, 72
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE = 25, 26, 27
OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 28, 29, 30, 32
OP_CONSTANT, OP_SPEC_CONSTANT, OP_SPEC_CONSTANT_OP, OP_VARIABLE = 43, 50, 52, 59
OP_TYPE_ACCELERATION_STRUCTURE = 5341
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE, DEC_BINDING, DEC_SET = 2, 3, 24, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12
DIM_BUFFER, DIM_SUBPASS_DATA = 5, 6


def spirv_words(path: Path) -> tuple[int, ...]:
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4:
        raise ValueError(f"{path}: SPIR-V size must be at least 20 bytes and divisible by 4")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203:
        raise ValueError(f"{path}: invalid little-endian SPIR-V magic")
    return words


def literal_string(words: tuple[int, ...]) -> str:
    raw = b"".join(struct.pack("<I", word) for word in words)
    return raw[:raw.index(b"\0")].decode("utf-8")


def reflect(path: Path, words: tuple[int, ...]) -> dict[str, list[tuple[str, int, int, int]]]:
    """Descriptor bindings per category as (name, binding, count, set), in SPIRV-Reflect order."""
    names, decorations, member_non_writable = {}, {}, set()
    types, constants, variables = {}, {}, []
    offset = 5
    while offset < len(words):
        count, op = words[offset] >> 16, words[offset] & 0xFFFF
        if count == 0 or offset + count > len(words):
            raise ValueError(f"{path}: truncated SPIR-V instruction at word {offset}")
        operands = words[offset + 1:offset + count]
        if op == OP_NAME:
            names[operands[0]] = literal_string(operands[1:])
        elif op == OP_DECORATE:
            decorations.setdefault(operands[0], {})[operands[1]] = operands[2:]
        elif op == OP_MEMBER_DECORATE and operands[2] == DEC_NON_WRITABLE:
            member_non_writable.add(operands[0])
        elif op in (OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY,
                    OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_TYPE_ACCELERATION_STRUCTURE):
            types[operands[0]] = (op, operands[1:])
        elif op == OP_CONSTANT:
            constants[operands[1]] = operands[2]
        elif op in (OP_SPEC_CONSTANT, OP_SPEC_CONSTANT_OP):
            constants[operands[1]] = None
        elif op == OP_VARIABLE:
            variables.append((operands[1], operands[0], operands[2]))
        offset += count

    bindings = []
    for variable, pointer, storage in variables:
        decorated = decorations.get(variable, {})
        if storage not in (SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER) or DEC_SET not in decorated or DEC_BINDING not in decorated:
            continue
        name = names.get(variable, "")
        pointer_op, pointer_operands = types[pointer]
        if pointer_op != OP_TYPE_POINTER:
            raise ValueError(f"{path}: resource '{name}' is not a pointer")
        type_id, descriptor_count = pointer_operands[1], 1
        while types.get(type_id, (None,))[0] in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            array_op, array_operands = types[type_id]
            if array_op == OP_TYPE_RUNTIME_ARRAY:
                descriptor_count = 0
            else:
                length = constants.get(array_operands[1])
                if length is None:
                    raise ValueError(f"{path}: resource '{name}' uses an unsupported array length")
                descriptor_count *= length
            type_id = array_operands[0]
        base_op, base_operands = types.get(type_id, (None, ()))
        if base_op == OP_TYPE_IMAGE:
            dim, sampled = base_operands[1], base_operands[5]
            if dim in (DIM_BUFFER, DIM_SUBPASS_DATA) or sampled not in (1, 2):
                raise ValueError(f"{path}: shader uses an unsupported resource type ('{name}')")
            category = "TextureSRV" if sampled == 1 else "TextureUAV"
        elif base_op == OP_TYPE_SAMPLER:
            category = "Sampler"
        elif base_op == OP_TYPE_ACCELERATION_STRUCTURE:
            category = "RTAccelerationStructure"
        elif base_op == OP_TYPE_STRUCT:
            block = decorations.get(type_id, {})
            if storage == SC_STORAGE_BUFFER or (DEC_BUFFER_BLOCK in block and DEC_BLOCK not in block):
                category = "BufferSRV" if type_id in member_non_writable else "BufferUAV"
            elif DEC_BLOCK in block:
                category = "CBV"
            else:
                raise ValueError(f"{path}: resource '{name}' is an unknown struct")
        else:
            # Combined image samplers, texel buffers and input attachments, as rejected by ffx_sc.
            raise ValueError(f"{path}: shader uses an unsupported resource type ('{name}')")
        descriptor_set, binding = decorated[DEC_SET][0], decorated[DEC_BINDING][0]
        bindings.append(((descriptor_set, binding, variable), category, (name, binding, descriptor_count, descriptor_set)))

    reflected = {category: [] for category in CATEGORIES}
    for _, category, info in sorted(bindings):
        reflected[category].append(info)
    return reflected


def collect(inputs: list[Path]) -> dict[str, Path]:
    found = {}
    for path in inputs:
        if not path.name.endswith(".spv"):
            raise ValueError(f"{path}: expected <pass>[.h<0|1>s<0|1>].spv")
        if path.name[:-4] in found:
            raise ValueError(f"{path}: duplicate permutation")
        found[path.name[:-4]] = path
    required = {stem + suffix for *_, suffixes, _, _, passes in EFFECTS for _, stem in passes for suffix in suffixes}
    if missing := sorted(required - found.keys()):
        raise ValueError("missing permutations: " + ", ".join(missing))
    if extra := sorted(found.keys() - required):
        raise ValueError("unreachable permutations: " + ", ".join(extra))
    return found


def generate(inputs: list[Path]) -> tuple[str, int]:
    found = collect(inputs)
    blobs, blob_by_digest, table = [], {}, {}
    for key in sorted(found):
        words = spirv_words(found[key])
        digest = hashlib.sha256(struct.pack(f"<{len(words)}I", *words)).hexdigest()
        if digest not in blob_by_digest:
            reflected = reflect(found[key], words)
            if not any(reflected.values()):
                raise ValueError(f"{found[key]}: no descriptor bindings reflected")
            blob_by_digest[digest] = len(blobs)
            blobs.append((words, reflected))
        table[key] = blob_by_digest[digest]
    if len(blobs) > 256:
        raise ValueError(f"{len(blobs)} unique SPIR-V modules exceed the uint8_t blob tables")

    lines = ["// Generated by tools/embed_fsr_shaders.py. Do not edit.",
             "#include <FidelityFX/host/ffx_fsr3upscaler.h>",
             "#include <FidelityFX/host/ffx_frameinterpolation.h>",
             "#include <FidelityFX/host/ffx_opticalflow.h>",
             "#include <ffx_shader_blobs.h>",
             "#include <fsr3upscaler/ffx_fsr3upscaler_private.h>",
             "#include <frameinterpolation/ffx_frameinterpolation_private.h>",
             "#include <opticalflow/ffx_opticalflow_private.h>",
             "#include <cstdio>", "#include <cstring>", "", "namespace {"]
    for index, (words, reflected) in enumerate(blobs):
        lines.append(f"const uint32_t g_spirv{index}[] = {{")
        for start in range(0, len(words), 8):
            lines.append("    " + ", ".join(f"0x{word:08x}u" for word in words[start:start + 8]) + ",")
        lines.append("};")
        for category, infos in reflected.items():
            if not infos:
                continue
            lines.append(f"const char* g_{category}Names{index}[] = {{ " + ", ".join(f'"{name}"' for name, _, _, _ in infos) + " };")
            for field, column in (("Bindings", 1), ("Counts", 2), ("Sets", 3)):
                lines.append(f"const uint32_t g_{category}{field}{index}[] = {{ " + ", ".join(str(info[column]) for info in infos) + " };")
    lines.append("const FfxShaderBlob g_blobs[] = {")
    for index, (words, reflected) in enumerate(blobs):
        counts = ", ".join(str(len(reflected[category])) for category in CATEGORIES)
        pointers = ", ".join(
            f"g_{c}Names{index}, g_{c}Bindings{index}, g_{c}Counts{index}, g_{c}Sets{index}" if reflected[c]
            else "nullptr, nullptr, nullptr, nullptr" for c in CATEGORIES)
        lines.append(f"    {{ reinterpret_cast<const uint8_t*>(g_spirv{index}), {len(words) * 4}u, {counts}, {pointers} }},")
    lines.append("};")
    for effect, _, _, suffixes, _, _, passes in EFFECTS:
        lines.append(f"// {effect}: [pass][variant]")
        lines.append(f"const uint8_t g_{effect.lower()}[{len(passes)}][{len(suffixes)}] = {{")
        for _, stem in passes:
            lines.append("    {" + ", ".join(str(table[stem + suffix]) for suffix in suffixes) + f"}}, // {stem}")
        lines.append("};")
    lines += [
        "", "FfxErrorCode unsupported(const char* effect, const char* what, uint32_t value, FfxShaderBlob* outBlob)", "{",
        "    std::fprintf(stderr, \"FidelityFX Vulkan: unsupported %s %s 0x%x (no shader blob)\\n\", effect, what, value);",
        "    std::memset(outBlob, 0, sizeof(FfxShaderBlob));",
        "    return FFX_ERROR_INVALID_ARGUMENT;", "}", "} // namespace", "",
        "FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effectId, FfxPass passId, FfxBindStage, uint32_t permutationOptions, FfxShaderBlob* outBlob)",
        "{",
        "    int pass = -1;",
        "    uint8_t blob = 0;",
        "    switch (effectId)", "    {"]
    for effect, pass_type, label, _, valid, variant, passes in EFFECTS:
        lines += [f"    case {effect}:",
                  f"        if (!({valid}))",
                  f"            return unsupported(\"{label}\", \"permutation options\", permutationOptions, outBlob);",
                  f"        switch (static_cast<{pass_type}>(passId))", "        {"]
        for index, (enumerators, _) in enumerate(passes):
            lines.append("        " + " ".join(f"case {name}:" for name in enumerators) + f" pass = {index}; break;")
        lines += ["        default: return unsupported(\"" + label + "\", \"pass\", passId, outBlob);", "        }",
                  f"        blob = g_{effect.lower()}[pass][{variant}];", "        break;"]
    lines += [
        "    default: return unsupported(\"effect\", \"id\", effectId, outBlob);", "    }",
        "    std::memcpy(outBlob, &g_blobs[blob], sizeof(FfxShaderBlob));",
        "    return FFX_OK;", "}", "",
        "// Wave64 permutations are never requested: yami_ffx_clamp_capabilities reports wave32 only.",
        "FfxErrorCode ffxIsWave64(FfxEffect effectId, uint32_t, bool& isWave64)", "{",
        "    isWave64 = false;",
        "    return " + " || ".join(f"effectId == {effect}" for effect, *_ in EFFECTS) + " ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;", "}"]
    return "\n".join(lines) + "\n", len(blobs)


def write_atomically(output: Path, text: str) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", dir=output.parent,
                                         prefix=output.name + ".", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        text, unique = generate(args.inputs)
        write_atomically(args.output, text)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
    print(f"FidelityFX shader blobs: {len(args.inputs)} permutations, {unique} unique SPIR-V modules")


if __name__ == "__main__":
    main()
