# Texture cooker "ktx2_uastc", the default (ADR 0024, 0043): KTX2 with UASTC data, compressed with
# Zstandard, and mipmaps. High quality; the runtime transcodes it to BC7 on PCs and ASTC 4x4 on
# phones. About a quarter of a PNG's memory on the GPU, and similar in size on disk.
#
# A texture cooker file defines dream_texture_cooker_<name>(<kind> <out_arguments>), which gives the
# arguments `ktx create` cooks one image with, for its kind: color, normal or linear.
function(dream_texture_cooker_ktx2_uastc kind out_arguments)
    if(kind STREQUAL "color")
        set(arguments --format R8G8B8A8_SRGB --assign-tf srgb)
    elseif(kind STREQUAL "normal")
        set(arguments --format R8G8B8A8_UNORM --assign-tf linear --normalize)
    else()
        set(arguments --format R8G8B8A8_UNORM --assign-tf linear)
    endif()
    list(APPEND arguments --encode uastc --uastc-quality 2 --zstd 18 --generate-mipmap)
    set(${out_arguments} "${arguments}" PARENT_SCOPE)
endfunction()
