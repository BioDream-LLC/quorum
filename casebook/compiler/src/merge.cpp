// Pass 3b: merging casebooks into a palette.
//
// The product thesis is that a customer buys the pump casebook, then later the tank casebook, and they coexist on
// one device without re-certifying the first. That is an assertion until it is tested, and this pass is what makes
// it testable: it unions lowered casebooks mechanically, so no hand-editing can accidentally make the result look
// better than it is.
//
// ASPIC+ predicts the union is safe. Two of the five rationality postulates say so directly:
//
//   non-interference   -- syntactically disjoint knowledge bases do not influence each other's conclusions
//   crash-resistance   -- no contaminating knowledge base can render the others irrelevant
//
// Both hold for ASPIC-minus under GROUNDED semantics, which is the only semantics this product ships on device.
// That makes this a genuine prediction rather than a hope, and a failure here would mean the lowering is wrong.

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "quorum/cbc/cbc.hpp"

namespace quorum::cbc {

Palette merge(const std::vector<Lowered>& members, Diagnostics& diag) {
    Palette out;

    if (members.empty()) {
        diag.error(0, "a palette needs at least one casebook");
        return out;
    }

    // Observations must remain a contiguous prefix of the combined framework, because the presence mask depends on
    // it. So the layout is: every member's observations first, in member order, then every member's body. A
    // member's arguments are therefore NOT contiguous -- it owns one slice of the prefix and one slice of the body.
    std::int32_t total_observations = 0;
    for (const Lowered& m : members) { total_observations += m.observation_count; }

    if (total_observations > 32) {
        diag.error(0, "a palette of " + std::to_string(total_observations) +
                          " observations exceeds the 32-bit presence mask; split the device");
    }

    out.members.resize(members.size());

    // Prefix every name. Always, not just on collision: the disjointness is the hypothesis under test, and making
    // it conditional would leave the result depending on whether two authors happened to pick the same word.
    auto qualify = [](const std::string& book, const std::string& name) { return book + "." + name; };

    // ── the observation prefix ──────────────────────────────────────────────
    std::int32_t offset = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const Lowered& m = members[i];
        PaletteMember& pm = out.members[i];
        pm.casebook = m.casebook;
        pm.observation_offset = offset;
        pm.observation_count = m.observation_count;
        pm.map.assign(static_cast<std::size_t>(m.count()), -1);

        for (std::int32_t a = 0; a < m.observation_count; ++a) {
            LoweredArgument arg = m.arguments[static_cast<std::size_t>(a)];
            arg.name = qualify(m.casebook, arg.name);
            if (!arg.relates_to.empty()) { arg.relates_to = qualify(m.casebook, arg.relates_to); }
            pm.map[static_cast<std::size_t>(a)] = out.all.count();
            out.all.arguments.push_back(std::move(arg));
        }
        offset += m.observation_count;
    }
    out.all.observation_count = total_observations;

    // ── each member's body ──────────────────────────────────────────────────
    for (std::size_t i = 0; i < members.size(); ++i) {
        const Lowered& m = members[i];
        PaletteMember& pm = out.members[i];
        for (std::int32_t a = m.observation_count; a < m.count(); ++a) {
            LoweredArgument arg = m.arguments[static_cast<std::size_t>(a)];
            arg.name = qualify(m.casebook, arg.name);
            if (!arg.relates_to.empty()) { arg.relates_to = qualify(m.casebook, arg.relates_to); }
            pm.map[static_cast<std::size_t>(a)] = out.all.count();
            out.all.arguments.push_back(std::move(arg));
        }
    }

    // ── edges, actions, closed-world ────────────────────────────────────────
    for (std::size_t i = 0; i < members.size(); ++i) {
        const Lowered& m = members[i];
        PaletteMember& pm = out.members[i];

        for (const LoweredEdge& e : m.edges) {
            LoweredEdge ne;
            ne.from = pm.map[static_cast<std::size_t>(e.from)];
            ne.to = pm.map[static_cast<std::size_t>(e.to)];
            ne.reason = e.reason;
            out.all.edges.push_back(std::move(ne));
        }

        for (const std::int32_t a : m.actions) {
            const std::int32_t combined = pm.map[static_cast<std::size_t>(a)];
            pm.actions.push_back(combined);
            out.all.actions.push_back(combined);
        }
        if (m.idle_action >= 0) { pm.idle_action = pm.map[static_cast<std::size_t>(m.idle_action)]; }
        if (m.safe_action >= 0) { pm.safe_action = pm.map[static_cast<std::size_t>(m.safe_action)]; }
        if (m.closed_world >= 0) { pm.closed_world = pm.map[static_cast<std::size_t>(m.closed_world)]; }

        for (const auto& group : m.exclusive_groups) {
            std::vector<std::int32_t> ng;
            for (const std::int32_t g : group) { ng.push_back(pm.map[static_cast<std::size_t>(g)]); }
            out.all.exclusive_groups.push_back(std::move(ng));
        }
    }

    // No cross-member edge exists, by construction: every edge above was rewritten through exactly one member's
    // map. The combined `idle_action` and `safe_action` are deliberately left at -1 -- a palette has no single idle
    // or safe action, and pretending otherwise is how a merged policy would go wrong.
    out.all.casebook = "palette";
    out.all.version = 1;
    out.all.closed_world = -1;
    out.all.max_args = out.all.count();
    out.all.max_attacks = static_cast<std::int32_t>(out.all.edges.size()) + out.all.observation_count;

    return out;
}

}  // namespace quorum::cbc
