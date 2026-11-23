# Texture cooker "ktx2_etc1s" (ADR 0043): KTX2 with ETC1S data (Basis Universal's smaller mode), and
# mipmaps, for games that must be small to download, such as on phones. Files are several times
# smaller than ktx2_uastc's, at a visible cost in quality: blocky gradients and color bleeding. The
# runtime transcodes it as it does UASTC. Normal maps lose too much in ETC1S, so they stay UASTC.
set(DREAM_TEXTURE_COOKER_DECODER ktx)
function(dream_texture_cooker_ktx2_etc1s kind out_arguments)
    if(kind STREQUAL "color")
        set(arguments --format R8G8B8A8_SRGB --assign-tf srgb --encode basis-lz --clevel 2
            --qlevel 192)
    elseif(kind STREQUAL "normal")
        set(arguments --format R8G8B8A8_UNORM --assign-tf linear --normalize --encode uastc
            --uastc-quality 2 --zstd 18)
    else()
        set(arguments --format R8G8B8A8_UNORM --assign-tf linear --encode basis-lz --clevel 2
            --qlevel 192)
    endif()
    list(APPEND arguments --generate-mipmap)
    set(${out_arguments} "${arguments}" PARENT_SCOPE)
endfunction()
