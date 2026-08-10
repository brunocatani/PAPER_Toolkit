#include "paper_toolkit/AuthoringPointerClickGate.h"
#include "paper_toolkit/AuthoringTimelinePolicy.h"

#include <cmath>
#include <cstdio>

namespace
{
    int s_failures = 0;

    void require(bool condition, const char* message)
    {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++s_failures;
        }
    }
}

int main()
{
    using namespace paper_toolkit;

    require(
        authoring_timeline::normalizedBoneName("Magazine:17") == "magazine",
        "normalized evidence names must strip numeric instance suffixes");
    require(
        authoring_timeline::normalizedBoneName("BOLT") == "bolt",
        "normalized evidence names must be case independent");

    float uniform = 0.0f;
    require(
        authoring_timeline::uniformPositiveScale({ 1.0f, 1.0f, 1.0f }, uniform) &&
            std::fabs(uniform - 1.0f) < 1.0e-6f,
        "uniform positive Qs scale must be representable by ROCK NiTransform");
    require(
        !authoring_timeline::uniformPositiveScale({ 1.0f, 2.0f, 1.0f }, uniform),
        "non-uniform Qs scale must fail closed");
    require(
        !authoring_timeline::uniformPositiveScale({ -1.0f, -1.0f, -1.0f }, uniform),
        "negative Qs scale must fail closed");

    authoring_timeline::QsTransform first{};
    first.rotate = { 0.0f, 0.0f, 0.0f, 1.0f };
    authoring_timeline::QsTransform equivalent = first;
    equivalent.rotate = { 0.0f, 0.0f, 0.0f, -1.0f };
    equivalent.translate = { 10.0f, 0.0f, 0.0f };
    authoring_timeline::QsTransform midpoint{};
    require(
        authoring_timeline::interpolate(first, equivalent, 0.5f, midpoint) &&
            std::fabs(midpoint.translate[0] - 5.0f) < 1.0e-5f &&
            std::fabs(midpoint.rotate[3] - 1.0f) < 1.0e-5f,
        "timeline interpolation must use the shortest quaternion arc");

    authoring_pointer_gate::State gate{};
    auto decision = authoring_pointer_gate::advance(
        gate, 10, true, true, true, true);
    require(!decision.forwardPrimaryDown, "newly routed press must wait for a mature lease");
    decision = authoring_pointer_gate::advance(
        gate, 11, true, true, false, true);
    require(!decision.forwardPrimaryDown, "mature lease must observe a neutral sample first");
    decision = authoring_pointer_gate::advance(
        gate, 12, true, true, true, true);
    require(decision.forwardPrimaryDown, "held press may route after lease maturity");
    decision = authoring_pointer_gate::advance(
        gate, 13, false, true, true, false);
    require(decision.clearLease && !decision.forwardPrimaryDown,
        "losing pointer routing must clear suppression and stop clicks");

    if (s_failures == 0) {
        std::puts("Authoring policy tests passed.");
    }
    return s_failures == 0 ? 0 : 1;
}
