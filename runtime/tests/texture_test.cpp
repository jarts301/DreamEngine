#include "dream/runtime/texture.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;

TEST_CASE("is_ktx2 recognizes a KTX2 file by its first bytes") {
    constexpr std::array<unsigned char, 14> k_start{0xAB, 'K', 'T', 'X', ' ', '2', '0',
                                                    0xBB, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0};
    std::vector<std::byte> ktx2;
    for (const unsigned char byte : k_start) {
        ktx2.push_back(static_cast<std::byte>(byte));
    }
    CHECK(is_ktx2(ktx2));
    ktx2.resize(11);
    CHECK_FALSE(is_ktx2(ktx2));
    const std::vector<std::byte> png{std::byte{0x89}, std::byte{'P'}, std::byte{'N'},
                                     std::byte{'G'}};
    CHECK_FALSE(is_ktx2(png));
    CHECK_FALSE(is_ktx2({}));
}

TEST_CASE("cooked_texture_path swaps the extension for .ktx2") {
    CHECK(cooked_texture_path("assets:/models/textures/rock.png") ==
          "assets:/models/textures/rock.ktx2");
    CHECK(cooked_texture_path("assets:/a.b/rock") == "assets:/a.b/rock.ktx2");
    CHECK(cooked_texture_path("rock.jpg") == "rock.ktx2");
}
