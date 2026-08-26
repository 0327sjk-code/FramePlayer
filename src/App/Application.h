#pragma once

#include "App/UpdateCommandLine.h"

struct HINSTANCE__;
using HINSTANCE = HINSTANCE__*;

namespace zt::sequence {

class Application final {
public:
    [[nodiscard]] int Run(
        HINSTANCE instance,
        int showCommand,
        const UpdateCommandLineOptions& commandLine);
};

}  // namespace zt::sequence
