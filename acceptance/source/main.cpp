#include "memory/fatal.hpp"
#include <cstdio>

void verify_source_shutdown();

void verify_source_projects();

int main() {
    atelier::install_fatal_handler();
    verify_source_projects();
    verify_source_shutdown();
    std::puts("Source public consumer: PASS");
}
