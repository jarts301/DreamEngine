# Skeletal animation: skinned tentacles playing clips at different speeds, backward, once, and
# fading from one clip to another. See docs/guides/animation.md.
dream_add_feature_sample(animation
    SOURCES src/animation.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.animation
    DISPLAY_NAME "Dream Animation"
    NULL_PASS "frames=5 draws=[1-9][0-9]* animated=7"
    WARP
)
