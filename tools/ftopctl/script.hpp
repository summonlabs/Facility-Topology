// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Mutation script parsing for the ftopctl inspection tool.
//
// A script is a line-oriented, deterministic representation of a mutation
// batch. It is untrusted input: every field is validated by the library before
// a single mutation is applied.

#ifndef FACILITY_TOPOLOGY_TOOLS_FTOPCTL_SCRIPT_HPP
#define FACILITY_TOPOLOGY_TOOLS_FTOPCTL_SCRIPT_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/facility_topology/mutation.hpp"

namespace ftopctl {

struct ScriptLine {
  std::size_t number = 0;
  std::string text;
};

/// Parses a mutation script.
///
/// Grammar (one command per line, `#` starts a comment, blank lines ignored):
///
///   add-node <id> <kind> [<"label">] [<zone-scope>]
///   remove-node <id>
///   move-node <id> <new-parent>
///   add-containment <parent> <child> [physical|logical]
///   remove-containment <parent> <child>
///   set-label <id> <"label">
///   add-adjacency <first> <second> [shared-boundary|service-aisle|structural-neighbor]
///   remove-adjacency <first> <second> [shared-boundary|service-aisle|structural-neighbor]
///   declare-domain <id> <power|cooling> [<"label">]
///   retire-domain <id>
///   add-association <node> <domain> <power|cooling>
///   remove-association <node> <domain>
///
/// Labels are quoted strings with `\"` and `\\` escapes. Anything else is
/// rejected with the line number.
dccp::facility_topology::Result<std::vector<dccp::facility_topology::Mutation>> parse_script(
    std::string_view script, const dccp::facility_topology::TopologyLimits& limits);

}  // namespace ftopctl

#endif  // FACILITY_TOPOLOGY_TOOLS_FTOPCTL_SCRIPT_HPP
