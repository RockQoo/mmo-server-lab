#include <print>

#include "Core/Util/BuildInfo.h"

int main()
{
    std::println("mmo-server-lab GameServer");
    std::println("build: {}", Core::BuildInfo());
    return 0;
}
