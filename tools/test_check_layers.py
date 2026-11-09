#!/usr/bin/env python3
"""Tests for check_layers.py. Run with: python tools/test_check_layers.py"""

import tempfile
import unittest
from pathlib import Path

from check_layers import check_repository


class CheckLayersTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.write("interfaces/physics/CMakeLists.txt", "dream_add_interface(physics)\n")

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def assert_violation(self, relative, text, expected):
        self.write(relative, text)
        problems = check_repository(self.root)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(expected, problems[0])
        self.assertTrue(problems[0].startswith(f"{relative}:1:"), problems[0])

    def assert_clean(self, relative, text):
        self.write(relative, text)
        self.assertEqual(check_repository(self.root), [])

    def test_empty_repository_is_clean(self):
        self.assertEqual(check_repository(self.root), [])

    def test_allowed_dependencies(self):
        self.assert_clean(
            "modules/physics/jolt/src/jolt.cpp",
            '#include "dream/core/log.h"\n'
            '#include "dream/physics/physics.h"\n'
            '#include "local.h"\n'
            "#include <Jolt/Jolt.h>\n",
        )
        self.assert_clean(
            "samples/game/src/main.cpp",
            '#include "dream/runtime/app.h"\n#include "dream/physics/physics.h"\n#include <vector>\n',
        )

    def test_core_must_stay_at_the_bottom(self):
        self.assert_violation(
            "core/src/a.cpp", '#include "dream/physics/physics.h"\n', "core must not include"
        )

    def test_module_must_not_use_the_runtime(self):
        self.assert_violation(
            "modules/physics/jolt/src/a.cpp",
            '#include "dream/runtime/app.h"\n',
            "modules must not include",
        )

    def test_module_must_not_use_another_backend(self):
        self.assert_violation(
            "modules/physics/jolt/src/a.cpp",
            '#include "../../bullet/src/bullet.h"\n',
            "reaches outside",
        )

    def test_unknown_library_is_rejected(self):
        self.assert_violation(
            "runtime/src/a.cpp", '#include "dream/modules/jolt.h"\n', "runtime must not include"
        )

    def test_interface_must_not_expose_backend_types(self):
        self.assert_violation(
            "interfaces/physics/include/dream/physics/physics.h",
            "#include <Jolt/Jolt.h>\n",
            "third-party header",
        )

    def test_interface_must_not_include_system_c_headers(self):
        self.assert_violation(
            "interfaces/physics/include/dream/physics/physics.h",
            "#include <windows.h>\n",
            "third-party header",
        )

    def test_game_must_not_include_backend_headers(self):
        self.assert_violation("samples/game/a.cpp", "#include <SDL3/SDL.h>\n", "third-party header")

    def test_game_must_not_include_backend_with_quotes(self):
        self.assert_violation("samples/game/a.cpp", '#include "SDL3/SDL.h"\n', "must not include")

    def test_game_may_include_its_own_headers(self):
        self.write("samples/game/local.h", "#pragma once\n")
        self.assert_clean("samples/game/a.cpp", '#include "local.h"\n')

    def test_game_may_use_its_own_include_directory(self):
        self.write("samples/game/include/game/player.h", "#pragma once\n")
        self.assert_clean("samples/game/src/a.cpp", '#include "game/player.h"\n')

    def test_tests_may_use_their_framework(self):
        self.assert_clean("samples/game/tests/a.cpp", "#include <doctest/doctest.h>\n")

    def test_game_must_not_link_a_backend(self):
        self.assert_violation(
            "samples/game/CMakeLists.txt",
            "target_link_libraries(game PRIVATE dream::module_physics_jolt)\n",
            "must not link",
        )

    def test_module_must_not_link_the_runtime(self):
        self.assert_violation(
            "modules/physics/jolt/CMakeLists.txt",
            "target_link_libraries(a PRIVATE dream::runtime)\n",
            "must not link",
        )

    def test_cmake_comments_are_ignored(self):
        self.assert_clean("core/CMakeLists.txt", "# Used by dream::runtime later.\n")

    def test_commented_include_is_ignored(self):
        self.assert_clean("core/src/a.cpp", '// #include "dream/runtime/app.h"\n')


if __name__ == "__main__":
    unittest.main()
