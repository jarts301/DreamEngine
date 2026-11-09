#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"

#include <doctest/doctest.h>

#include <optional>

using namespace dream::core;
using doctest::Approx;

namespace {

constexpr double k_tolerance = 0.0001;

void check_near(Vec3 actual, Vec3 expected) {
    CHECK(actual.x == Approx(expected.x).epsilon(k_tolerance));
    CHECK(actual.y == Approx(expected.y).epsilon(k_tolerance));
    CHECK(actual.z == Approx(expected.z).epsilon(k_tolerance));
}

void check_near(const Mat4& actual, const Mat4& expected) {
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(actual.columns[i].x == Approx(expected.columns[i].x).epsilon(k_tolerance));
        CHECK(actual.columns[i].y == Approx(expected.columns[i].y).epsilon(k_tolerance));
        CHECK(actual.columns[i].z == Approx(expected.columns[i].z).epsilon(k_tolerance));
        CHECK(actual.columns[i].w == Approx(expected.columns[i].w).epsilon(k_tolerance));
    }
}

Vec3 perspective_divide(Vec4 clip) {
    return {clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
}

} // namespace

// Compile-time checks: these operations must stay constexpr.
static_assert(Vec3{1.0f, 2.0f, 3.0f} + Vec3{1.0f, 1.0f, 1.0f} == Vec3{2.0f, 3.0f, 4.0f});
static_assert(cross(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}) == Vec3{0.0f, 0.0f, 1.0f});
static_assert(Mat4{} * Mat4{} == Mat4{});

TEST_CASE("vector arithmetic") {
    Vec3 value{1.0f, 2.0f, 3.0f};
    value += Vec3{1.0f, 1.0f, 1.0f};
    value *= 2.0f;

    CHECK(value == Vec3{4.0f, 6.0f, 8.0f});
    CHECK(2.0f * Vec2{1.0f, 2.0f} == Vec2{2.0f, 4.0f});
    CHECK(-Vec4{1.0f, 2.0f, 3.0f, 4.0f} == Vec4{-1.0f, -2.0f, -3.0f, -4.0f});
    CHECK(dot(Vec3{1.0f, 2.0f, 3.0f}, Vec3{4.0f, 5.0f, 6.0f}) == Approx(32.0f));
    CHECK(lerp(Vec2{0.0f, 10.0f}, Vec2{10.0f, 20.0f}, 0.5f) == Vec2{5.0f, 15.0f});
}

TEST_CASE("length and normalization") {
    CHECK(length(Vec2{3.0f, 4.0f}) == Approx(5.0f));
    check_near(normalized(Vec3{0.0f, 0.0f, 9.0f}), Vec3{0.0f, 0.0f, 1.0f});
    CHECK(normalized(Vec3{}) == Vec3{});
}

TEST_CASE("quaternion rotation follows the right-hand rule") {
    const Quat quarter_turn_y = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, radians(90.0f));
    check_near(rotate(quarter_turn_y, {1.0f, 0.0f, 0.0f}), {0.0f, 0.0f, -1.0f});

    const Quat quarter_turn_z = quat_from_axis_angle({0.0f, 0.0f, 2.0f}, radians(90.0f));
    check_near(rotate(quarter_turn_z, {1.0f, 0.0f, 0.0f}), {0.0f, 1.0f, 0.0f});
}

TEST_CASE("quaternion product applies the right-hand operand first") {
    const Quat y_turn = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, radians(90.0f));
    const Quat z_turn = quat_from_axis_angle({0.0f, 0.0f, 1.0f}, radians(90.0f));
    const Vec3 input{1.0f, 0.0f, 0.0f};

    check_near(rotate(y_turn * z_turn, input), rotate(y_turn, rotate(z_turn, input)));
}

TEST_CASE("a quaternion times its conjugate is the identity") {
    const Quat q = normalized(Quat{0.1f, 0.2f, 0.3f, 0.9f});
    const Quat product = q * conjugate(q);

    CHECK(product.x == Approx(0.0f).epsilon(k_tolerance));
    CHECK(product.y == Approx(0.0f).epsilon(k_tolerance));
    CHECK(product.z == Approx(0.0f).epsilon(k_tolerance));
    CHECK(product.w == Approx(1.0f).epsilon(k_tolerance));
}

TEST_CASE("slerp interpolates along the shortest path") {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Quat start{};
    const Quat end = quat_from_axis_angle(up, radians(90.0f));
    const Quat middle = quat_from_axis_angle(up, radians(45.0f));
    const Vec3 input{1.0f, 0.0f, 0.0f};

    check_near(rotate(slerp(start, end, 0.5f), input), rotate(middle, input));
    check_near(rotate(slerp(start, end, 0.0f), input), input);
    check_near(rotate(slerp(start, end, 1.0f), input), rotate(end, input));

    const Quat negated_end{-end.x, -end.y, -end.z, -end.w};
    check_near(rotate(slerp(start, negated_end, 0.5f), input), rotate(middle, input));
}

TEST_CASE("rotation matrix agrees with quaternion rotation") {
    const Quat q = quat_from_axis_angle({1.0f, 2.0f, 3.0f}, radians(37.0f));
    const Vec3 input{4.0f, -5.0f, 6.0f};

    check_near(transform_point(rotation(q), input), rotate(q, input));
}

TEST_CASE("transform applies scale, then rotation, then translation") {
    const Quat quarter_turn_y = quat_from_axis_angle({0.0f, 1.0f, 0.0f}, radians(90.0f));
    const Mat4 matrix =
        transform({10.0f, 0.0f, 0.0f}, quarter_turn_y, {2.0f, 2.0f, 2.0f});

    check_near(transform_point(matrix, {1.0f, 0.0f, 0.0f}), {10.0f, 0.0f, -2.0f});
    check_near(transform_direction(matrix, {1.0f, 0.0f, 0.0f}), {0.0f, 0.0f, -2.0f});
}

TEST_CASE("matrix product applies the right-hand operand first") {
    const Mat4 move = translation({1.0f, 0.0f, 0.0f});
    const Mat4 scale = scaling({2.0f, 2.0f, 2.0f});

    check_near(transform_point(move * scale, {1.0f, 0.0f, 0.0f}), {3.0f, 0.0f, 0.0f});
    check_near(transform_point(scale * move, {1.0f, 0.0f, 0.0f}), {4.0f, 0.0f, 0.0f});
}

TEST_CASE("transpose swaps rows and columns") {
    const Mat4 matrix = translation({1.0f, 2.0f, 3.0f});
    CHECK(transpose(matrix).columns[0].w == 1.0f);
    CHECK(transpose(transpose(matrix)) == matrix);
}

TEST_CASE("inverse undoes a transform") {
    const Quat q = quat_from_axis_angle({1.0f, 1.0f, 0.0f}, radians(50.0f));
    const Mat4 matrix = transform({1.0f, -2.0f, 3.0f}, q, {2.0f, 3.0f, 4.0f});

    const std::optional<Mat4> inverted = inverse(matrix);
    REQUIRE(inverted.has_value());
    check_near(matrix * *inverted, Mat4{});
    check_near(*inverted * matrix, Mat4{});
}

TEST_CASE("inverse reports a singular matrix") {
    CHECK_FALSE(inverse(scaling({1.0f, 0.0f, 1.0f})).has_value());
}

TEST_CASE("look_at places the camera at the origin looking down -Z") {
    const Vec3 eye{5.0f, 2.0f, 1.0f};
    const Vec3 target{5.0f, 2.0f, -9.0f};
    const Mat4 view = look_at(eye, target, {0.0f, 1.0f, 0.0f});

    check_near(transform_point(view, eye), {0.0f, 0.0f, 0.0f});
    check_near(transform_point(view, target), {0.0f, 0.0f, -10.0f});
    check_near(transform_point(view, eye + Vec3{1.0f, 0.0f, 0.0f}), {1.0f, 0.0f, 0.0f});
    check_near(transform_point(view, eye + Vec3{0.0f, 1.0f, 0.0f}), {0.0f, 1.0f, 0.0f});
}

TEST_CASE("perspective maps the near and far planes to depth 0 and 1 with +Y up") {
    const Mat4 projection = perspective(radians(90.0f), 2.0f, 1.0f, 100.0f);

    const Vec3 on_near = perspective_divide(projection * Vec4{0.0f, 0.0f, -1.0f, 1.0f});
    const Vec3 on_far = perspective_divide(projection * Vec4{0.0f, 0.0f, -100.0f, 1.0f});
    CHECK(on_near.z == Approx(0.0f).epsilon(k_tolerance));
    CHECK(on_far.z == Approx(1.0f).epsilon(k_tolerance));

    // With a 90 degree field of view, the top edge at depth 1 is at y = 1.
    const Vec3 top = perspective_divide(projection * Vec4{0.0f, 1.0f, -1.0f, 1.0f});
    CHECK(top.y == Approx(1.0f).epsilon(k_tolerance));

    // An aspect ratio of 2 makes the right edge at depth 1 x = 2.
    const Vec3 right = perspective_divide(projection * Vec4{2.0f, 0.0f, -1.0f, 1.0f});
    CHECK(right.x == Approx(1.0f).epsilon(k_tolerance));
}

TEST_CASE("orthographic maps its box to clip space") {
    const Mat4 projection = orthographic(-2.0f, 6.0f, -1.0f, 3.0f, 1.0f, 11.0f);

    check_near(transform_point(projection, {-2.0f, -1.0f, -1.0f}), {-1.0f, -1.0f, 0.0f});
    check_near(transform_point(projection, {6.0f, 3.0f, -11.0f}), {1.0f, 1.0f, 1.0f});
}
