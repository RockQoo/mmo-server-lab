#include <print>

#include "Core/Util/BuildInfo.h"

int main()
{
    std::println("mmo-server-lab game server");
    std::println("build: {}", Core::BuildInfo());
    return 0;
}
