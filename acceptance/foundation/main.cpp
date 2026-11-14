#include "memory/fatal.hpp"
#include "../helpers.hpp"
#include "foundation/array.hpp"
#include "foundation/string.hpp"
#include <cstdio>

void verify_tasks();

int main() {
    atelier::install_fatal_handler();
    using namespace atelier;
    using namespace acceptance::helpers;
    auto text = String::from_utf8("abcdefghijklmnopqrst");
    require(text && text->append(text->view().slice(1)), "String self-append failed.");
    require(text->view() == "abcdefghijklmnopqrstbcdefghijklmnopqrst", "String self-view was invalidated.");
    Array<String> values{String::literal<"seven">(), String::literal<"nine">()};
    values.append(values.view());
    require(values.size() == 4 && values[2].view() == "seven" && values[3].view() == "nine",
            "Array self-append lost its inputs.");
    verify_tasks();
    std::puts("Foundation acceptance: PASS (owning aliases and failed join cleanup)");
}
