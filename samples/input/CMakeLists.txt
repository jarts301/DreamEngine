# Input: a ball moved by keys, a gamepad's stick and triggers, or a finger, with a panel showing each
# action's state and strength as it changes. See docs/guides/input.md.
dream_add_feature_sample(input
    SOURCES src/input.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.input
    DISPLAY_NAME "Dream Input"
    NULL_PASS "frames=5 actions=8 source=none"
    WARP
)
