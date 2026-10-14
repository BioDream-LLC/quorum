// A casebook that ignores `<channel>_unavailable` LOSES the safe action when the sensor dies mid-alarm.
//
// cbc warns "nothing reasons about 'X_unavailable', so this casebook cannot tell a dead X sensor from one
// reporting nothing". That warning had never been shown to matter. This proves it does.
//
// The binding layer's contract is correct and deliberate: an unbelievable channel asserts its `unavailable`
// observation and NONE of its thresholds, so the mask "never says quiet when it means blind". The consequence is
// that a LATCHED alarm disappears when its channel goes stale -- which is safe only if the casebook reasons about
// the `unavailable` observation that replaced it.
//
// Measured here: tank at 90% (trip is 85.0), lvl_high latched, then 19 s of silence.
//   healthy -> stop_transfer
//   stale   -> carry_on          <- the alarm went with the sensor
//
// tank.cb, which DOES reason about _unavailable, holds stop_transfer across the same transition. That contrast is
// what makes this evidence instead of an anecdote.
#include <cstdio>
#include <string>
#include "blind_generated.hpp"
namespace gen  = quorum::casebook::gen::blind;
namespace bind = quorum::binding;
int main() {
    auto run = [](std::uint32_t at_ms, const char* what) -> const char* {
        gen::CaseBinding b;
        if (gen::make_binding(b) != bind::Status::Ok) { std::printf("binding failed\n"); return "?"; }
        (void)b.sample(0, 90000, 1000);   // level 90.0% -> latches lvl_high
        const std::uint32_t m = b.mask(at_ms);
        gen::CaseFramework fw; gen::CaseSolver sv; gen::CasePolicy pol;
        (void)gen::make_policy(pol);
        if (gen::instantiate(fw, m) != quorum::solver::Status::Ok) { std::printf("inst failed\n"); return "?"; }
        const quorum::arbiter::Verdict v = quorum::arbiter::decide(fw, sv, pol);
        const char* act = gen::name_of(v.action.get());
        std::printf("  %-34s mask=0x%04X  action=%s\n", what, m, act);
        return act;
    };
    const char* a = run(1000,  "healthy, lvl_high latched");
    const char* b = run(20000, "STALE 19 s later, still latched");
    int failures = 0;
    if (std::string(a) != "stop_transfer") {
        std::printf("FAIL  a latched alarm on a healthy channel must stop the transfer, got '%s'\n", a);
        ++failures;
    }
    if (std::string(b) != "carry_on") {
        std::printf("FAIL  this fixture exists to DEMONSTRATE lost safety; expected 'carry_on' once the channel\n"
                    "      went stale, got '%s'. If the engine now holds the safe action without the casebook\n"
                    "      reasoning about _unavailable, that is a real improvement -- but the warning's\n"
                    "      justification has changed and both the warning text and this fixture need rewriting.\n", b);
        ++failures;
    }
    if (failures == 0) {
        std::printf("  OK: ignoring _unavailable loses the safe action when the sensor dies mid-alarm\n");
    }
    return failures == 0 ? 0 : 1;
}
