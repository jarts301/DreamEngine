# Texture cooker "none" (ADR 0043): textures are left as their PNG files, which the runtime decodes
# as they load. Lossless, but four times the GPU memory of a cooked texture, no mipmaps made ahead,
# and slower loading. For pixel art, or projects whose build machine lacks the ktx tool.
set(DREAM_TEXTURE_COOKER_NONE TRUE)
