#include <print>

#include "Base/BuildInfo.h"

int main()
{
    std::println("mmo-server-lab Gateway");
    std::println("build: {}", Base::BuildInfo());
    return 0;
}
