#include <print>

#include "core/BuildInfo.h"

int main()
{
    std::println("mmo-server-lab game server");
    std::println("build: {}", core::BuildInfo());
    return 0;
}
