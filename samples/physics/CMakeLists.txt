# Physics in 3D: dominoes knocked over by a ball, a Newton's cradle on distance joints, a hinged
# gate, bouncy balls falling into a sensor, and a laser of ray casts. See docs/adr/0011.
dream_add_feature_sample(physics
    SOURCES src/physics.cpp
    ASSETS assets ../assets
    APPLICATION_ID org.dreamengine.physics3d
    DISPLAY_NAME "Dream Physics 3D"
    NULL_PASS "frames=5 bodies=[1-9][0-9]+"
    WARP
)
