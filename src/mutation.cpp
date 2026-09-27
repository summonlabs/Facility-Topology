// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/facility_topology/mutation.hpp"

#include <string>
#include <type_traits>
#include <utility>

#include "graph_data.hpp"

namespace dccp::facility_topology {
namespace {

std::string describe(const Mutation& mutation) {
  return std::visit(
      [](const auto& command) -> std::string {
        using Command = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Command, AddNode>) {
          return command.node.id.str();
        } else if constexpr (std::is_same_v<Command, RemoveNode>) {
          return command.id.str();
        } else if constexpr (std::is_same_v<Command, MoveNode>) {
          return command.id.str();
        } else if constexpr (std::is_same_v<Command, AddContainment>) {
          return command.edge.child.str();
        } else if constexpr (std::is_same_v<Command, RemoveContainment>) {
          return command.child.str();
        } else if constexpr (std::is_same_v<Command, SetNodeLabel>) {
          return command.id.str();
        } else if constexpr (std::is_same_v<Command, AddAdjacency>) {
          return command.edge.first.str();
        } else if constexpr (std::is_same_v<Command, RemoveAdjacency>) {
          return command.first.str();
        } else if constexpr (std::is_same_v<Command, DeclareDomain>) {
          return command.declaration.id.str();
        } else if constexpr (std::is_same_v<Command, RetireDomain>) {
          return command.id.str();
        } else if constexpr (std::is_same_v<Command, AddAssociation>) {
          return command.association.node.str();
        } else {
          return command.node.str();
        }
      },
      mutation);
}

void record_failure(MutationOutcome& outcome, std::size_t index, const Mutation& mutation, const Error& error) {
  MutationDisposition disposition;
  disposition.index = index;
  disposition.code = error.code();
  disposition.explanation = error.message();
  disposition.subject = error.subject().empty() ? describe(mutation) : error.subject();
  outcome.dispositions.push_back(std::move(disposition));
  if (outcome.first_failure == ErrorCode::Ok) {
    outcome.first_failure = error.code();
  }
}

}  // namespace

std::string_view mutation_kind_token(const Mutation& mutation) noexcept {
  return std::visit(
      [](const auto& command) -> std::string_view {
        using Command = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Command, AddNode>) {
          return "add-node";
        } else if constexpr (std::is_same_v<Command, RemoveNode>) {
          return "remove-node";
        } else if constexpr (std::is_same_v<Command, MoveNode>) {
          return "move-node";
        } else if constexpr (std::is_same_v<Command, AddContainment>) {
          return "add-containment";
        } else if constexpr (std::is_same_v<Command, RemoveContainment>) {
          return "remove-containment";
        } else if constexpr (std::is_same_v<Command, SetNodeLabel>) {
          return "set-node-label";
        } else if constexpr (std::is_same_v<Command, AddAdjacency>) {
          return "add-adjacency";
        } else if constexpr (std::is_same_v<Command, RemoveAdjacency>) {
          return "remove-adjacency";
        } else if constexpr (std::is_same_v<Command, DeclareDomain>) {
          return "declare-domain";
        } else if constexpr (std::is_same_v<Command, RetireDomain>) {
          return "retire-domain";
        } else if constexpr (std::is_same_v<Command, AddAssociation>) {
          return "add-association";
        } else {
          return "remove-association";
        }
      },
      mutation);
}

Result<void> validate_batch(const MutationBatch& batch, const TopologyLimits& limits) {
  std::string explanation;
  if (!validate_limits(limits, explanation)) {
    return Error(ErrorCode::InvalidArgument, "invalid topology limits: " + explanation);
  }
  if (batch.actor.empty()) {
    return Error(ErrorCode::MissingField, "mutation batch actor must not be empty");
  }
  FT_TRYV(validate_source(batch.source, limits));
  FT_TRYV(validate_reason(batch.reason, limits));
  if (!batch.recorded_at.empty() && !is_canonical_utc_timestamp(batch.recorded_at)) {
    return Error(ErrorCode::MalformedRecord, "batch timestamp must be canonical UTC")
        .with_subject(batch.recorded_at);
  }
  if (batch.mutations.size() > limits.max_mutations_per_batch) {
    return Error(ErrorCode::LimitExceeded, "batch exceeds the configured maximum number of mutations")
        .with_subject(std::to_string(batch.mutations.size()));
  }
  return ok();
}

std::string MutationOutcome::to_string() const {
  std::size_t accepted = 0;
  for (const MutationDisposition& disposition : dispositions) {
    if (disposition.accepted()) {
      ++accepted;
    }
  }
  std::string out = "code=";
  out.append(error_code_name(code));
  out.append(" published=");
  out.append(published ? "yes" : "no");
  out.append(" base=");
  out.append(text::format_u64(base_generation.value()));
  out.append(" head=");
  out.append(text::format_u64(new_generation.value()));
  out.append(" applied=");
  out.append(std::to_string(accepted));
  out.push_back('/');
  out.append(std::to_string(dispositions.size()));
  if (replayed) {
    out.append(" replayed=yes");
  }
  return out;
}

std::string MutationOutcome::explain() const {
  std::string out = to_string();
  out.push_back('\n');
  out.append("  ");
  out.append(explanation);
  for (const MutationDisposition& disposition : dispositions) {
    out.push_back('\n');
    out.append("  [");
    out.append(std::to_string(disposition.index));
    out.append("] ");
    out.append(error_code_name(disposition.code));
    if (!disposition.subject.empty()) {
      out.push_back(' ');
      out.append(disposition.subject);
    }
    out.append(": ");
    out.append(disposition.explanation);
  }
  return out;
}

BatchApplication apply_batch_explained(const TopologySnapshot& base, const MutationBatch& batch,
                                       const TopologyLimits& limits) {
  BatchApplication application;
  MutationOutcome& outcome = application.outcome;
  outcome.base_generation = batch.base_generation;

  const Result<void> shape = validate_batch(batch, limits);
  if (!shape.has_value()) {
    outcome.code = shape.error().code();
    outcome.explanation = shape.error().message();
    return application;
  }
  if (!base.valid()) {
    outcome.code = ErrorCode::NotInitialized;
    outcome.explanation = "no base generation was supplied";
    return application;
  }
  if (base.generation() != batch.base_generation) {
    outcome.code = ErrorCode::StaleBaseGeneration;
    outcome.explanation = "batch base generation " + text::format_u64(batch.base_generation.value()) +
                          " is not the current generation " + text::format_u64(base.generation().value());
    return application;
  }

  const Result<TopologyGeneration> candidate_generation = base.generation().next();
  if (!candidate_generation.has_value()) {
    outcome.code = candidate_generation.error().code();
    outcome.explanation = candidate_generation.error().message();
    return application;
  }

  Result<TopologyBuilder> builder = TopologyBuilder::from_snapshot(base);
  if (!builder.has_value()) {
    outcome.code = builder.error().code();
    outcome.explanation = builder.error().message();
    return application;
  }
  builder->set_limits(limits);

  outcome.dispositions.reserve(batch.mutations.size());
  for (std::size_t index = 0; index < batch.mutations.size(); ++index) {
    const Mutation& mutation = batch.mutations[index];
    const Result<void> applied = std::visit(
        [&builder](const auto& command) -> Result<void> {
          using Command = std::decay_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, AddNode>) {
            return builder->add_node(command.node);
          } else if constexpr (std::is_same_v<Command, RemoveNode>) {
            return builder->remove_node(command.id);
          } else if constexpr (std::is_same_v<Command, MoveNode>) {
            return builder->move_node(command.id, command.new_parent, command.provenance);
          } else if constexpr (std::is_same_v<Command, AddContainment>) {
            return builder->add_containment(command.edge);
          } else if constexpr (std::is_same_v<Command, RemoveContainment>) {
            return builder->remove_containment(command.parent, command.child);
          } else if constexpr (std::is_same_v<Command, SetNodeLabel>) {
            return builder->set_node_label(command.id, command.label, command.provenance);
          } else if constexpr (std::is_same_v<Command, AddAdjacency>) {
            return builder->add_adjacency(command.edge);
          } else if constexpr (std::is_same_v<Command, RemoveAdjacency>) {
            return builder->remove_adjacency(command.first, command.second, command.kind);
          } else if constexpr (std::is_same_v<Command, DeclareDomain>) {
            return builder->declare_domain(command.declaration);
          } else if constexpr (std::is_same_v<Command, RetireDomain>) {
            return builder->retire_domain(command.id);
          } else if constexpr (std::is_same_v<Command, AddAssociation>) {
            return builder->add_association(command.association);
          } else {
            return builder->remove_association(command.node, command.domain);
          }
        },
        mutation);

    MutationDisposition disposition;
    disposition.index = index;
    if (applied.has_value()) {
      disposition.code = ErrorCode::Ok;
      disposition.explanation = "applied";
      disposition.subject = describe(mutation);
      outcome.dispositions.push_back(std::move(disposition));
      continue;
    }
    record_failure(outcome, index, mutation, applied.error());
    outcome.code = ErrorCode::BatchRejected;
    outcome.explanation = "rejected at mutation [" + std::to_string(index) + "] " +
                          std::string(mutation_kind_token(mutation)) + ": " + applied.error().to_string();
    return application;
  }

  // Whole-graph validation runs on the fully mutated candidate. A failure here
  // has no single owning mutation, so it is reported with the index one past
  // the last mutation.
  const ValidationReport report = builder->validate();
  if (!report.valid) {
    const ValidationIssue& issue = report.issues.front();
    MutationDisposition disposition;
    disposition.index = batch.mutations.size();
    disposition.code = issue.code;
    disposition.explanation = issue.explanation;
    disposition.subject = issue.subject;
    outcome.dispositions.push_back(std::move(disposition));
    outcome.code = ErrorCode::BatchRejected;
    outcome.first_failure = issue.code;
    outcome.explanation = "the candidate generation failed whole-graph validation: " + report.issues.front().explanation;
    return application;
  }

  Result<TopologySnapshot> snapshot = builder->build(*candidate_generation);
  if (!snapshot.has_value()) {
    outcome.code = snapshot.error().code();
    outcome.explanation = snapshot.error().message();
    return application;
  }
  outcome.published = true;
  outcome.code = ErrorCode::Ok;
  outcome.new_generation = *candidate_generation;
  outcome.content_digest = snapshot->content_digest();
  outcome.explanation = "candidate generation " + text::format_u64(candidate_generation->value()) +
                        " produced from base generation " + text::format_u64(base.generation().value()) + " (" +
                        std::to_string(batch.mutations.size()) + " mutations applied)";
  application.candidate = std::move(*snapshot);
  return application;
}

Result<TopologySnapshot> apply_batch(const TopologySnapshot& base, const MutationBatch& batch,
                                     const TopologyLimits& limits) {
  BatchApplication application = apply_batch_explained(base, batch, limits);
  if (!application.accepted()) {
    const ErrorCode code =
        (application.outcome.first_failure != ErrorCode::Ok) ? application.outcome.first_failure : application.outcome.code;
    return Error(code, application.outcome.explanation);
  }
  return std::move(application.candidate);
}

}  // namespace dccp::facility_topology
