// forward.slang for materials whose alpha cuts holes in them, glTF's MASK alpha mode (ADR 0034):
// pixels whose base color alpha is below the material's cutoff aren't drawn.

#define DREAM_ALPHA_MASK 1
#include "forward.slang"
