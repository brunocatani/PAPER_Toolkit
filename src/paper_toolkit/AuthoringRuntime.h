#pragma once

#include <cstdint>
#include <memory>

#include "api/ROCKProviderApi.h"

namespace paper_toolkit
{
    // Standalone PAPER_Toolkit animation workstation. It consumes PAPER's
    // immutable/value-only exact clip API and publishes preview-only ROCK part
    // drives. It never calls the native clip activation/update/deactivation
    // boundary, so timeline scrubbing cannot change the game reload graph.
    class AuthoringRuntime
    {
    public:
        AuthoringRuntime();
        ~AuthoringRuntime();
        AuthoringRuntime(const AuthoringRuntime&) = delete;
        AuthoringRuntime& operator=(const AuthoringRuntime&) = delete;

        void onGameLoaded();
        void onFrame(const rock::provider::RockProviderFrameSnapshot& snapshot);
        void shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
