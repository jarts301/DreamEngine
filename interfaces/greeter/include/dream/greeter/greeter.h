#pragma once

#include <string>
#include <string_view>

namespace dream::greeter {

// A toy interface that exercises the module system from configuration to running code. It is the
// template for real interfaces and can be removed once they cover the same ground.
class Greeter {
public:
    static constexpr std::string_view k_module_interface = "greeter";

    virtual ~Greeter() = default;

    [[nodiscard]] virtual std::string greet(std::string_view name) const = 0;
};

} // namespace dream::greeter
