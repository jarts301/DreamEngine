# Compiles one stage of a Slang shader to GLSL for the OpenGL backends (ADR 0026), run as
# `cmake -P` by _dream_compile_shaders. Slang has no GLSL ES output, so both kinds go through
# SPIR-V, and SPIRV-Cross turns that into GLSL for desktop OpenGL or OpenGL ES.
#
# Expects: SLANGC, SPIRV_CROSS, SOURCE, STAGE (vertex or fragment), OUTPUT, LANGUAGE (glsl or
# essl), VERSION (such as 430 or 310).

foreach(variable IN ITEMS SLANGC SPIRV_CROSS SOURCE STAGE OUTPUT LANGUAGE VERSION)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "DreamCrossCompileShader.cmake needs ${variable}.")
    endif()
endforeach()

set(spirv "${OUTPUT}.spv")
execute_process(
    COMMAND "${SLANGC}" "${SOURCE}" -target spirv -profile spirv_1_3
        -matrix-layout-column-major -warnings-disable 39001
        -entry ${STAGE}_main -stage ${STAGE} -o "${spirv}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "slangc could not compile ${SOURCE} (${STAGE}).")
endif()

if(LANGUAGE STREQUAL "essl")
    set(options --es)
else()
    set(options --no-es)
endif()
# The engine's clip space has depth from 0 to 1, as Vulkan's and Direct3D's do, and OpenGL's goes
# from -1, which --fixup-clipspace converts in the vertex shader.
if(STAGE STREQUAL "vertex")
    list(APPEND options --fixup-clipspace)
endif()
# Push constants become an ordinary uniform block.
execute_process(
    COMMAND "${SPIRV_CROSS}" "${spirv}" ${options} --version ${VERSION}
        --glsl-emit-push-constant-as-ubo
    OUTPUT_VARIABLE glsl
    RESULT_VARIABLE result)
file(REMOVE "${spirv}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "spirv-cross could not convert ${SOURCE} (${STAGE}) to ${LANGUAGE}.")
endif()

# Every other resource has a binding from the shader ([[vk::binding]]), but push constants have
# none in SPIR-V, so their block would default to binding 0, the uniform block's. They take binding
# 2, where the OpenGL backends bind them.
string(REGEX MATCHALL "layout\\(std140\\) uniform" unbound "${glsl}")
list(LENGTH unbound unbound_count)
if(unbound_count GREATER 1)
    message(FATAL_ERROR "${SOURCE} (${STAGE}) has more than one uniform block without a binding; "
                        "only the push constants may lack [[vk::binding]] (ADR 0013).")
endif()
#
# SPIRV-Cross writes push constants' math for a plain, column-major uniform, which is what it emits
# them as by default: a row-major matrix's multiplication comes out reversed. As a block, it still
# marks such a matrix row_major, which would transpose it a second time, so the mark is removed.
if(unbound_count EQUAL 1)
    string(FIND "${glsl}" "layout(std140) uniform" start)
    string(SUBSTRING "${glsl}" ${start} -1 rest)
    string(FIND "${rest}" "}" block_length)
    string(SUBSTRING "${rest}" 0 ${block_length} block)
    string(SUBSTRING "${glsl}" 0 ${start} before)
    string(SUBSTRING "${rest}" ${block_length} -1 after)
    string(REPLACE "layout(row_major) " "" block "${block}")
    string(REPLACE "layout(std140) uniform" "layout(binding = 2, std140) uniform" block "${block}")
    set(glsl "${before}${block}${after}")
endif()
# OpenGL stores a texture's rows from the bottom, and the other APIs from the top, so the device
# turns the picture upside down when it draws into a texture, through this uniform (ADR 0027): 1
# for the window and -1 for textures.
if(STAGE STREQUAL "vertex")
    set(clip_fixup "gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;")
    string(FIND "${glsl}" "${clip_fixup}" fixup_at)
    string(FIND "${glsl}" "void main()" main_at)
    if(fixup_at EQUAL -1 OR main_at EQUAL -1)
        message(FATAL_ERROR "spirv-cross wrote ${SOURCE} (${STAGE}) in an unexpected form.")
    endif()
    string(REPLACE "${clip_fixup}" "${clip_fixup}\n    gl_Position.y *= dream_clip_flip;" glsl
        "${glsl}")
    string(SUBSTRING "${glsl}" 0 ${main_at} before)
    string(SUBSTRING "${glsl}" ${main_at} -1 after)
    set(glsl "${before}uniform float dream_clip_flip;\n\n${after}")
endif()
file(WRITE "${OUTPUT}" "${glsl}")
