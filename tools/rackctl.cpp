// Rack Registry - rackctl inspection and operations tool.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// rackctl never bypasses the library. Every mutation goes through the same
// generation-fenced, lifecycle-gated API a normal consumer uses, and every
// mutation requires the caller to state the generation it expects. Read-only
// commands open the store without taking writer authority, so they can run
// while another process owns the store.

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cli_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)
using rackregistry::cli::Arguments;
using rackregistry::cli::JsonObject;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitRejected = 2;

constexpr std::string_view kDefaultStateFile = "rack-registry.rrstate";
constexpr std::string_view kDefaultWriter = "wr:rackctl";
constexpr std::string_view kDefaultActor = "rackctl";
constexpr std::string_view kDefaultProducer = "rackctl/1.0.0";

void print_usage() {
  std::cout <<
      R"(rackctl - Rack Registry inspection and operations

Usage: rackctl <command> [arguments] [options]

Read-only commands (take no writer authority):
  list                     List every rack in canonical order
  show <rack>              Describe one rack, its generations and its digest
  members <rack>           Enumerate members in a deterministic order
  occupancy <rack>         Mount occupancy in mount order
  free <rack>              Structural free ranges (descriptive, not capacity)
  shared <rack>            Shared-mount availability
  diff <rack>              Diff against the retained previous publication; with
                           --from N (and optionally --to-generation M) diff two
                           generations retained in this process
  verify                   Validate the state file without taking authority
  recover                  Report what opening the store recovered
  lock-status              Report the writer lock
  rejections               Show the retained rejection explanations
  version                  Print the library and state format versions

Mutating commands (all require explicit preconditions):
  register <rack>          Register a rack
      --units N --profile cp:x [--provides a,b] [--power p] [--cooling c] [--label L]
  set-structure <rack>     Replace the structural definition
      --units N --profile cp:x --expected-generation N [...]
  lifecycle <rack>         Apply one lifecycle transition
      --to <state> --expected-generation N [--expected-state S]
  insert <rack>            Insert a member
      --member rm:x --asset a:y --mount "U1-U2" --expected-generation N
      --expected-membership-generation M [--member-state installed|reserved]
      [--requires t1,t2] [--forbids t3]
  remove <rack>            Remove a member
      --member rm:x --expected-generation N --expected-membership-generation M
  move <rack>              Move a member
      --member rm:x --mount "U5" --expected-generation N
      --expected-membership-generation M
  replace <rack>           Replace the asset of a member
      --member rm:x --asset a:z --expected-generation N
      --expected-membership-generation M [--member-state S] [--requires ...]
  takeover                 Operator fencing: take writer authority unconditionally
      --writer wr:x [--at NANOSECONDS]
  export                   Write the canonical state image to --out

Common options:
  --state PATH             State file (default rack-registry.rrstate)
  --writer WRITER          Writer identity for mutating commands (default wr:rackctl)
  --actor ACTOR            Provenance actor (default rackctl)
  --at NANOSECONDS         Provenance timestamp; defaults to the current time
  --source SOURCE          Provenance source (default operator)
  --source-ref REF         Provenance source reference
  --source-sequence N      Provenance source sequence
  --note TEXT              Provenance note
  --request-id ID          Idempotency identity for a retryable mutation
  --producer REF           Producer identity recorded in published state
  --json                   Machine-readable output

Exit codes: 0 success, 1 usage error, 2 operation rejected.
)";
}

std::string state_path(const Arguments& args) {
  return args.get("state", kDefaultStateFile);
}

std::optional<SourceReference> producer_of(const Arguments& args) {
  const std::string text = args.get("producer", kDefaultProducer);
  if (text.empty()) {
    return std::nullopt;
  }
  return SourceReference::parse(text).value();
}

Result<ProvenanceRecord> provenance_of(const Arguments& args) {
  ProvenanceRecord provenance;
  const std::string source_text = args.get("source", "operator");
  const auto source = parse_provenance_source(source_text);
  if (!source) {
    return source.error();
  }
  provenance.source = source.value();
  const auto actor = ActorId::parse(args.get("actor", kDefaultActor));
  if (!actor) {
    return actor.error();
  }
  provenance.actor = actor.value();
  const std::string reference_text = args.get("source-ref");
  if (!reference_text.empty()) {
    const auto reference = SourceReference::parse(reference_text);
    if (!reference) {
      return reference.error();
    }
    provenance.source_reference = reference.value();
  }
  if (args.has("source-sequence")) {
    const auto sequence = rackregistry::cli::parse_u64(args.get("source-sequence"), "source-sequence");
    if (!sequence) {
      return sequence.error();
    }
    provenance.source_sequence = sequence.value();
  }
  const std::string at_text = args.get("at");
  provenance.observed_at_unix_ns =
      at_text.empty() ? rackregistry::cli::now_unix_ns()
                      : rackregistry::cli::parse_u64(at_text, "at").value_or(0);
  const auto note = Note::parse_or_none(args.get("note"));
  if (!note) {
    return note.error();
  }
  provenance.note = note.value();
  return provenance;
}

Result<std::optional<RequestId>> request_id_of(const Arguments& args) {
  const std::string text = args.get("request-id");
  if (text.empty()) {
    return std::optional<RequestId>{};
  }
  const auto id = RequestId::parse(text);
  if (!id) {
    return id.error();
  }
  return std::optional<RequestId>{id.value()};
}

Result<RackId> rack_id_of(const Arguments& args, std::size_t index) {
  if (args.positional().size() <= index) {
    return Arguments::usage_error("this command requires a rack identity");
  }
  return RackId::parse(args.positional()[index]);
}

Result<std::uint64_t> required_u64(const Arguments& args, std::string_view name) {
  if (!args.has(name)) {
    return Arguments::usage_error("--" + std::string(name) + " is required");
  }
  return rackregistry::cli::parse_u64(args.get(name), name);
}

Result<LifecycleState> parse_state(std::string_view text) { return parse_lifecycle_state(text); }

// Opens a store read-only. Never takes writer authority, so it is safe to run
// against a store another process is writing.
Result<std::unique_ptr<RackStore>> open_read(const Arguments& args, WriterId& writer_out) {
  StoreOptions options;
  options.path = state_path(args);
  options.read_only = true;
  options.producer = producer_of(args);
  options.create_if_missing = args.has("allow-missing");
  const auto writer = WriterId::parse(args.get("writer", kDefaultWriter));
  if (!writer) {
    return writer.error();
  }
  writer_out = writer.value();
  return RackStore::open(options);
}

Result<std::unique_ptr<RackStore>> open_write(const Arguments& args) {
  StoreOptions options;
  options.path = state_path(args);
  options.producer = producer_of(args);
  const auto writer = WriterId::parse(args.get("writer", kDefaultWriter));
  if (!writer) {
    return writer.error();
  }
  options.writer_id = writer.value();
  return RackStore::open(options);
}

int finish(const RackError& error) {
  rackregistry::cli::print_error(error);
  return error.code == ErrorCode::InvalidArgument || error.code == ErrorCode::EmptyValue ||
                 error.code == ErrorCode::MalformedIdentity ||
                 error.code == ErrorCode::InvalidCharacter ||
                 error.code == ErrorCode::InvalidRange ||
                 error.code == ErrorCode::InvalidEnumValue
             ? kExitUsage
             : kExitRejected;
}

int report_receipt(const Arguments& args, const MutationReceipt& receipt) {
  if (args.has("json")) {
    JsonObject object;
    object.add_string("operation", operation_kind_name(receipt.operation));
    object.add_string("rack", receipt.rack_id.text());
    if (receipt.member_id.has_value()) {
      object.add_string("member", receipt.member_id->text());
    }
    object.add_number("generation", receipt.generation.value());
    object.add_number("revision", receipt.revision.value());
    object.add_number("membership_generation", receipt.membership_generation.value());
    object.add_bool("replayed", receipt.replayed);
    object.add_string("state_digest", receipt.state_digest.to_hex());
    std::cout << object.render() << "\n";
  } else {
    std::cout << receipt.to_text() << "\n";
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Read-only commands
// ---------------------------------------------------------------------------

int command_list(const Arguments& args) {
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const std::vector<RackView> racks = store.value()->racks();
  if (args.has("json")) {
    std::string out = "[";
    for (std::size_t i = 0; i < racks.size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      JsonObject object;
      object.add_string("rack", racks[i].id().text());
      object.add_number("units", racks[i].unit_count());
      object.add_string("lifecycle", lifecycle_state_name(racks[i].lifecycle()));
      object.add_number("generation", racks[i].generation().value());
      object.add_number("revision", racks[i].revision().value());
      object.add_number("membership_generation",
                        racks[i].membership_generation().value());
      object.add_number("members", racks[i].member_count());
      object.add_string("state_digest", racks[i].state_digest().to_hex());
      out.append(object.render());
    }
    out.push_back(']');
    std::cout << out << "\n";
    return kExitOk;
  }
  if (racks.empty()) {
    std::cout << "no racks\n";
    return kExitOk;
  }
  for (const RackView& rack : racks) {
    std::cout << rack.id().text() << " units=" << rack.unit_count()
              << " lifecycle=" << lifecycle_state_name(rack.lifecycle())
              << " generation=" << rack.generation().value()
              << " revision=" << rack.revision().value()
              << " membership_generation=" << rack.membership_generation().value()
              << " members=" << rack.member_count() << "\n";
  }
  return kExitOk;
}

int command_show(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const auto view = store.value()->rack(rack_id.value());
  if (!view) {
    return finish(view.error());
  }
  const RackView& rack = view.value();
  if (args.has("json")) {
    JsonObject object;
    object.add_string("rack", rack.id().text());
    object.add_number("units", rack.unit_count());
    object.add_string("lifecycle", lifecycle_state_name(rack.lifecycle()));
    object.add_number("generation", rack.generation().value());
    object.add_number("revision", rack.revision().value());
    object.add_number("membership_generation", rack.membership_generation().value());
    object.add_string("profile", rack.profile().id.text());
    object.add_string("provides", rack.profile().provides.to_text());
    object.add_string("label", rack.label().text());
    object.add_number("members", rack.member_count());
    object.add_number("provenance_dropped", rack.provenance_dropped());
    object.add_number("idempotency_dropped", rack.idempotency_dropped());
    object.add_string("state_digest", rack.state_digest().to_hex());
    std::string power = "[";
    for (std::size_t i = 0; i < rack.power_domains().size(); ++i) {
      if (i != 0) {
        power.push_back(',');
      }
      power.append(rackregistry::cli::json_string(rack.power_domains()[i].text()));
    }
    power.push_back(']');
    object.add("power_domains", power);
    std::string cooling = "[";
    for (std::size_t i = 0; i < rack.cooling_domains().size(); ++i) {
      if (i != 0) {
        cooling.push_back(',');
      }
      cooling.append(rackregistry::cli::json_string(rack.cooling_domains()[i].text()));
    }
    cooling.push_back(']');
    object.add("cooling_domains", cooling);
    std::cout << object.render() << "\n";
    return kExitOk;
  }

  std::cout << "rack              " << rack.id().text() << "\n";
  std::cout << "units             " << rack.unit_count() << "\n";
  std::cout << "mount extent      " << rack.structure().slot_extent().to_text() << "\n";
  std::cout << "lifecycle         " << lifecycle_state_name(rack.lifecycle()) << "\n";
  std::cout << "generation        " << rack.generation().value() << "\n";
  std::cout << "revision          " << rack.revision().value() << "\n";
  std::cout << "membership gen    " << rack.membership_generation().value() << "\n";
  std::cout << "profile           " << rack.profile().id.text() << "\n";
  std::cout << "provides          " << rack.profile().provides.to_text() << "\n";
  if (!rack.label().empty()) {
    std::cout << "label             " << rack.label().text() << "\n";
  }
  for (const PowerDomainReference& reference : rack.power_domains()) {
    std::cout << "power domain      " << reference.text() << "\n";
  }
  for (const CoolingDomainReference& reference : rack.cooling_domains()) {
    std::cout << "cooling domain    " << reference.text() << "\n";
  }
  std::cout << "members           " << rack.member_count() << "\n";
  std::cout << "state digest      " << rack.state_digest().to_hex() << "\n";
  std::cout << "provenance\n";
  for (const ProvenanceRecord& provenance : rack.provenance()) {
    std::cout << "  " << provenance.to_text() << "\n";
  }
  if (rack.provenance_dropped() != 0) {
    std::cout << "  (" << rack.provenance_dropped() << " older records evicted)\n";
  }
  std::cout << "generation evidence\n";
  for (const GenerationEvidence& evidence : rack.generation_evidence()) {
    std::cout << "  generation " << evidence.generation.value() << " revision "
              << evidence.revision.value() << " membership "
              << evidence.membership_generation.value() << " lifecycle "
              << lifecycle_state_name(evidence.lifecycle) << " members " << evidence.member_count
              << " digest " << evidence.state_digest.to_hex() << "\n";
  }
  return kExitOk;
}

int command_members(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto order = parse_member_order(args.get("order", "mount"));
  if (!order) {
    return finish(order.error());
  }
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const auto members = store.value()->members(rack_id.value(), order.value());
  if (!members) {
    return finish(members.error());
  }
  if (args.has("json")) {
    std::string out = "[";
    for (std::size_t i = 0; i < members.value().size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      const MemberRecord& member = members.value()[i];
      JsonObject object;
      object.add_string("member", member.member_id.text());
      object.add_string("asset", member.asset_id.text());
      object.add_string("mount", member.mount.to_text());
      object.add_string("state", membership_state_name(member.state));
      object.add_string("required", member.requirements.required.to_text());
      object.add_string("forbids", member.requirements.forbids.to_text());
      object.add_number("mount_generation", member.mount_generation.value());
      object.add_number("asset_generation", member.asset_generation.value());
      object.add_number("created_at_membership_generation",
                        member.created_at_membership_generation.value());
      out.append(object.render());
    }
    out.push_back(']');
    std::cout << out << "\n";
    return kExitOk;
  }
  for (const MemberRecord& member : members.value()) {
    std::cout << member.member_id.text() << " asset=" << member.asset_id.text()
              << " mount=" << member.mount.to_text()
              << " state=" << membership_state_name(member.state);
    if (!member.requirements.required.empty()) {
      std::cout << " requires=" << member.requirements.required.to_text();
    }
    if (!member.requirements.forbids.empty()) {
      std::cout << " forbids=" << member.requirements.forbids.to_text();
    }
    std::cout << "\n";
  }
  if (members.value().empty()) {
    std::cout << "no members\n";
  }
  return kExitOk;
}

int command_occupancy(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const auto occupancy = store.value()->occupancy(rack_id.value());
  if (!occupancy) {
    return finish(occupancy.error());
  }
  if (args.has("json")) {
    std::string out = "[";
    for (std::size_t i = 0; i < occupancy.value().size(); ++i) {
      if (i != 0) {
        out.push_back(',');
      }
      const OccupancyRecord& entry = occupancy.value()[i];
      JsonObject object;
      object.add_string("member", entry.member_id.text());
      object.add_string("asset", entry.asset_id.text());
      object.add_string("mount", entry.mount.to_text());
      object.add_string("state", membership_state_name(entry.state));
      object.add_string("units", entry.units.has_value() ? entry.units->to_text() : "none");
      out.append(object.render());
    }
    out.push_back(']');
    std::cout << out << "\n";
    return kExitOk;
  }
  for (const OccupancyRecord& entry : occupancy.value()) {
    std::cout << entry.mount.to_text() << " "
              << (entry.units.has_value() ? entry.units->to_text() : std::string("zero-u"))
              << " " << entry.member_id.text() << " asset=" << entry.asset_id.text()
              << " state=" << membership_state_name(entry.state) << "\n";
  }
  if (occupancy.value().empty()) {
    std::cout << "no occupancy\n";
  }
  return kExitOk;
}

int command_free(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const auto free = store.value()->free_spans(rack_id.value());
  if (!free) {
    return finish(free.error());
  }
  std::cout << "descriptive only: these are unoccupied structural ranges, not capacity\n";
  for (const FreeSpan& span : free.value()) {
    std::cout << span.to_text() << "\n";
  }
  if (free.value().empty()) {
    std::cout << "no free ranges\n";
  }
  return kExitOk;
}

int command_shared(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const auto shared = store.value()->shared_availability(rack_id.value());
  if (!shared) {
    return finish(shared.error());
  }
  for (const SharedSpanAvailability& entry : shared.value()) {
    std::cout << entry.to_text() << "\n";
  }
  if (shared.value().empty()) {
    std::cout << "no shared mounts\n";
  }
  return kExitOk;
}

int command_diff(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  if (!args.has("from")) {
    // Without an explicit generation the comparison is against the retained
    // previous publication, which is the only comparison that survives a
    // restart.
    const auto diff = store.value()->diff_with_previous();
    if (!diff) {
      return finish(diff.error());
    }
    std::cout << diff.value().to_text() << "\n";
    return kExitOk;
  }

  const auto from = rackregistry::cli::parse_u64(args.get("from"), "from");
  if (!from) {
    return finish(from.error());
  }
  const auto from_generation = RackGeneration::create(from.value());
  if (!from_generation) {
    return finish(from_generation.error());
  }

  Result<RackDiff> diff = [&]() -> Result<RackDiff> {
    if (!args.has("to-generation")) {
      return store.value()->diff_generations(rack_id.value(), from_generation.value());
    }
    const auto to = rackregistry::cli::parse_u64(args.get("to-generation"), "to-generation");
    if (!to) {
      return to.error();
    }
    const auto to_generation = RackGeneration::create(to.value());
    if (!to_generation) {
      return to_generation.error();
    }
    return store.value()->diff_generations(rack_id.value(), from_generation.value(),
                                           to_generation.value());
  }();
  if (!diff) {
    // The in-process generation ring starts empty after a restart, so a request
    // for a generation that is not retained falls back to the retained previous
    // publication when that publication is exactly the requested generation.
    if (diff.error().code == ErrorCode::GenerationNotRetained) {
      const auto previous = store.value()->diff_with_previous();
      if (previous) {
        for (const RackDiff& candidate : previous.value().racks_changed) {
          if (candidate.rack_id == rack_id.value() &&
              candidate.from_generation == from_generation.value()) {
            std::cout << candidate.to_text() << "\n";
            std::cout << "(compared against the retained previous publication)\n";
            return kExitOk;
          }
        }
      }
    }
    return finish(diff.error());
  }
  std::cout << diff.value().to_text() << "\n";
  return kExitOk;
}

int command_verify(const Arguments& args) {
  const auto info = RackStore::inspect(state_path(args));
  if (!info) {
    return finish(info.error());
  }
  if (args.has("json")) {
    JsonObject object;
    object.add_number("format_version", info.value().format_version);
    object.add_number("snapshot_layout_version", info.value().snapshot_layout_version);
    object.add_number("mount_slots_per_rack_unit", info.value().mount_slots_per_rack_unit);
    object.add_number("epoch", info.value().epoch.value());
    object.add_number("sequence", info.value().sequence.value());
    object.add_number("racks", info.value().rack_count);
    object.add_number("members", info.value().member_count);
    object.add_number("bytes", info.value().byte_size);
    object.add_string("state_digest", info.value().payload_digest.to_hex());
    object.add_bool("verified", true);
    std::cout << object.render() << "\n";
    return kExitOk;
  }
  std::cout << "verified " << info.value().to_text() << "\n";
  return kExitOk;
}

int command_recover(const Arguments& args) {
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  std::cout << store.value()->recovery().to_text() << "\n";
  return kExitOk;
}

int command_lock_status(const Arguments& args) {
  const auto info = RackStore::query_writer_lock(state_path(args));
  if (!info) {
    return finish(info.error());
  }
  if (args.has("json")) {
    JsonObject object;
    object.add_string("state", writer_lock_state_name(info.value().state));
    object.add_string("writer", info.value().writer_id.text());
    object.add_string("incarnation", info.value().incarnation);
    object.add_number("pid", info.value().pid);
    object.add_number("epoch", info.value().epoch.value());
    object.add_number("acquired_at_unix_ns", info.value().acquired_at_unix_ns);
    object.add_string("taken_over_from", info.value().taken_over_from.text());
    object.add_number("taken_over_epoch", info.value().taken_over_epoch.value());
    object.add_string("detail", info.value().detail);
    std::cout << object.render() << "\n";
    return kExitOk;
  }
  std::cout << info.value().to_text() << "\n";
  return kExitOk;
}

int command_rejections(const Arguments& args) {
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const std::vector<RejectionRecord> rejections = store.value()->rejections();
  for (const RejectionRecord& rejection : rejections) {
    std::cout << rejection.to_text() << "\n";
  }
  if (rejections.empty()) {
    std::cout << "no rejections recorded in this process\n";
  }
  return kExitOk;
}

int command_export(const Arguments& args) {
  WriterId writer;
  auto store = open_read(args, writer);
  if (!store) {
    return finish(store.error());
  }
  const RackSnapshot snapshot = store.value()->snapshot();
  const std::vector<std::uint8_t> bytes = snapshot.to_bytes();
  const std::string out_path = args.get("out");
  if (out_path.empty()) {
    std::cout << "state_digest " << snapshot.state_digest().to_hex() << "\n";
    std::cout << "bytes " << bytes.size() << "\n";
    return kExitOk;
  }
  std::FILE* stream = nullptr;
#if defined(_WIN32)
  if (fopen_s(&stream, out_path.c_str(), "wb") != 0) {
    stream = nullptr;
  }
#else
  stream = std::fopen(out_path.c_str(), "wb");
#endif
  if (stream == nullptr) {
    return finish(make_error(ErrorCode::IoFailure, "cannot open " + out_path + " for writing",
                             ErrorDetail{.operation = "export", .subject = out_path}));
  }
  const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), stream);
  std::fclose(stream);
  if (written != bytes.size()) {
    return finish(make_error(ErrorCode::IoFailure, "writing " + out_path + " was incomplete",
                             ErrorDetail{.operation = "export", .subject = out_path}));
  }
  std::cout << "exported " << bytes.size() << " bytes to " << out_path << "\n";
  std::cout << "state_digest " << snapshot.state_digest().to_hex() << "\n";
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Mutating commands
// ---------------------------------------------------------------------------

int command_register(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto units = required_u64(args, "units");
  if (!units) {
    return finish(units.error());
  }
  const auto unit_count = validate_unit_count(units.value());
  if (!unit_count) {
    return finish(unit_count.error());
  }
  const auto profile_id = CompatibilityProfileId::parse(args.get("profile", "cp:default"));
  if (!profile_id) {
    return finish(profile_id.error());
  }
  const auto provides = rackregistry::cli::parse_traits(rackregistry::cli::split_csv(args.get("provides")));
  if (!provides) {
    return finish(provides.error());
  }
  const auto power = rackregistry::cli::parse_power_domains(
      rackregistry::cli::split_csv(args.get("power")));
  if (!power) {
    return finish(power.error());
  }
  const auto cooling = rackregistry::cli::parse_cooling_domains(
      rackregistry::cli::split_csv(args.get("cooling")));
  if (!cooling) {
    return finish(cooling.error());
  }
  const auto label = DisplayLabel::parse_or_none(args.get("label"));
  if (!label) {
    return finish(label.error());
  }
  const auto provenance = provenance_of(args);
  if (!provenance) {
    return finish(provenance.error());
  }
  const auto request_id = request_id_of(args);
  if (!request_id) {
    return finish(request_id.error());
  }

  RegisterRackRequest request;
  request.structure.id = rack_id.value();
  request.structure.unit_count = unit_count.value();
  request.structure.profile.id = profile_id.value();
  request.structure.profile.provides = provides.value();
  request.structure.power_domains = power.value();
  request.structure.cooling_domains = cooling.value();
  request.structure.label = label.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }
  const auto receipt = store.value()->register_rack(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_set_structure(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto units = required_u64(args, "units");
  const auto generation = required_u64(args, "expected-generation");
  if (!units || !generation) {
    return finish(units ? generation.error() : units.error());
  }
  const auto unit_count = validate_unit_count(units.value());
  if (!unit_count) {
    return finish(unit_count.error());
  }
  const auto expected = RackGeneration::create(generation.value());
  if (!expected) {
    return finish(expected.error());
  }
  const auto profile_id = CompatibilityProfileId::parse(args.get("profile", "cp:default"));
  if (!profile_id) {
    return finish(profile_id.error());
  }
  const auto provides = rackregistry::cli::parse_traits(rackregistry::cli::split_csv(args.get("provides")));
  const auto power = rackregistry::cli::parse_power_domains(
      rackregistry::cli::split_csv(args.get("power")));
  const auto cooling = rackregistry::cli::parse_cooling_domains(
      rackregistry::cli::split_csv(args.get("cooling")));
  const auto label = DisplayLabel::parse_or_none(args.get("label"));
  if (!provides || !power || !cooling || !label) {
    return finish(!provides ? provides.error()
                            : (!power ? power.error() : (!cooling ? cooling.error() : label.error())));
  }
  const auto provenance = provenance_of(args);
  const auto request_id = request_id_of(args);
  if (!provenance || !request_id) {
    return finish(!provenance ? provenance.error() : request_id.error());
  }

  SetRackStructureRequest request;
  request.rack_id = rack_id.value();
  request.precondition.expected_generation = expected.value();
  request.unit_count = unit_count.value();
  request.profile.id = profile_id.value();
  request.profile.provides = provides.value();
  request.power_domains = power.value();
  request.cooling_domains = cooling.value();
  request.label = label.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }
  const auto receipt = store.value()->set_rack_structure(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_lifecycle(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto generation = required_u64(args, "expected-generation");
  if (!generation) {
    return finish(generation.error());
  }
  const auto expected = RackGeneration::create(generation.value());
  if (!expected) {
    return finish(expected.error());
  }
  const auto target = parse_state(args.get("to"));
  if (!target) {
    return finish(target.error());
  }
  const auto provenance = provenance_of(args);
  const auto request_id = request_id_of(args);
  if (!provenance || !request_id) {
    return finish(!provenance ? provenance.error() : request_id.error());
  }

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }

  LifecycleState expected_state = LifecycleState::Defined;
  if (args.has("expected-state")) {
    const auto parsed = parse_state(args.get("expected-state"));
    if (!parsed) {
      return finish(parsed.error());
    }
    expected_state = parsed.value();
  } else {
    const auto view = store.value()->rack(rack_id.value());
    if (!view) {
      return finish(view.error());
    }
    expected_state = view.value().lifecycle();
  }

  TransitionLifecycleRequest request;
  request.rack_id = rack_id.value();
  request.precondition.expected_generation = expected.value();
  request.precondition.expected_state = expected_state;
  request.target = target.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  const auto receipt = store.value()->transition_lifecycle(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_insert(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto generation = required_u64(args, "expected-generation");
  const auto membership = required_u64(args, "expected-membership-generation");
  if (!generation || !membership) {
    return finish(generation ? membership.error() : generation.error());
  }
  const auto expected = RackGeneration::create(generation.value());
  const auto expected_membership = MembershipGeneration::create(membership.value());
  if (!expected || !expected_membership) {
    return finish(expected ? expected_membership.error() : expected.error());
  }
  if (!args.has("member")) {
    return finish(Arguments::usage_error("--member is required"));
  }
  const auto member_id = RackMemberId::parse(args.get("member"));
  if (!member_id) {
    return finish(member_id.error());
  }
  if (!args.has("asset")) {
    return finish(Arguments::usage_error("--asset is required"));
  }
  const auto asset = AssetId::parse(args.get("asset"));
  if (!asset) {
    return finish(asset.error());
  }
  const auto mount = rackregistry::cli::parse_mount(args.get("mount", "U1"));
  if (!mount) {
    return finish(mount.error());
  }
  const auto state = parse_membership_state(args.get("member-state", "installed"));
  if (!state) {
    return finish(state.error());
  }
  const auto required_traits =
      rackregistry::cli::parse_traits(rackregistry::cli::split_csv(args.get("requires")));
  const auto forbidden_traits =
      rackregistry::cli::parse_traits(rackregistry::cli::split_csv(args.get("forbids")));
  if (!required_traits || !forbidden_traits) {
    return finish(!required_traits ? required_traits.error() : forbidden_traits.error());
  }
  const auto provenance = provenance_of(args);
  const auto request_id = request_id_of(args);
  if (!provenance || !request_id) {
    return finish(!provenance ? provenance.error() : request_id.error());
  }

  InsertMemberRequest request;
  request.rack_id = rack_id.value();
  request.precondition.expected_generation = expected.value();
  request.precondition.expected_membership_generation = expected_membership.value();
  request.member_id = member_id.value();
  request.asset_id = asset.value();
  request.mount = mount.value();
  request.state = state.value();
  request.requirements.required = required_traits.value();
  request.requirements.forbids = forbidden_traits.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }
  const auto receipt = store.value()->insert_member(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_remove(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto generation = required_u64(args, "expected-generation");
  const auto membership = required_u64(args, "expected-membership-generation");
  if (!generation || !membership) {
    return finish(generation ? membership.error() : generation.error());
  }
  if (!args.has("member")) {
    return finish(Arguments::usage_error("--member is required"));
  }
  const auto member_id = RackMemberId::parse(args.get("member"));
  if (!member_id) {
    return finish(member_id.error());
  }
  const auto provenance = provenance_of(args);
  const auto request_id = request_id_of(args);
  if (!provenance || !request_id) {
    return finish(!provenance ? provenance.error() : request_id.error());
  }

  RemoveMemberRequest request;
  request.rack_id = rack_id.value();
  request.precondition.expected_generation = RackGeneration::create(generation.value()).value();
  request.precondition.expected_membership_generation =
      MembershipGeneration::create(membership.value()).value();
  request.member_id = member_id.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }
  const auto receipt = store.value()->remove_member(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_move(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto generation = required_u64(args, "expected-generation");
  const auto membership = required_u64(args, "expected-membership-generation");
  if (!generation || !membership) {
    return finish(generation ? membership.error() : generation.error());
  }
  if (!args.has("member")) {
    return finish(Arguments::usage_error("--member is required"));
  }
  if (!args.has("mount")) {
    return finish(Arguments::usage_error("--mount is required"));
  }
  const auto member_id = RackMemberId::parse(args.get("member"));
  const auto mount = rackregistry::cli::parse_mount(args.get("mount"));
  if (!member_id || !mount) {
    return finish(!member_id ? member_id.error() : mount.error());
  }
  const auto provenance = provenance_of(args);
  const auto request_id = request_id_of(args);
  if (!provenance || !request_id) {
    return finish(!provenance ? provenance.error() : request_id.error());
  }

  MoveMemberRequest request;
  request.rack_id = rack_id.value();
  request.precondition.expected_generation = RackGeneration::create(generation.value()).value();
  request.precondition.expected_membership_generation =
      MembershipGeneration::create(membership.value()).value();
  request.member_id = member_id.value();
  request.target_mount = mount.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }
  const auto receipt = store.value()->move_member(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_replace(const Arguments& args) {
  const auto rack_id = rack_id_of(args, 0);
  if (!rack_id) {
    return finish(rack_id.error());
  }
  const auto generation = required_u64(args, "expected-generation");
  const auto membership = required_u64(args, "expected-membership-generation");
  if (!generation || !membership) {
    return finish(generation ? membership.error() : generation.error());
  }
  if (!args.has("member") || !args.has("asset")) {
    return finish(Arguments::usage_error("--member and --asset are required"));
  }
  const auto member_id = RackMemberId::parse(args.get("member"));
  const auto asset = AssetId::parse(args.get("asset"));
  if (!member_id || !asset) {
    return finish(!member_id ? member_id.error() : asset.error());
  }
  const auto state = parse_membership_state(args.get("member-state", "installed"));
  if (!state) {
    return finish(state.error());
  }
  const auto required_traits =
      rackregistry::cli::parse_traits(rackregistry::cli::split_csv(args.get("requires")));
  const auto forbidden_traits =
      rackregistry::cli::parse_traits(rackregistry::cli::split_csv(args.get("forbids")));
  if (!required_traits || !forbidden_traits) {
    return finish(!required_traits ? required_traits.error() : forbidden_traits.error());
  }
  const auto provenance = provenance_of(args);
  const auto request_id = request_id_of(args);
  if (!provenance || !request_id) {
    return finish(!provenance ? provenance.error() : request_id.error());
  }

  ReplaceMemberRequest request;
  request.rack_id = rack_id.value();
  request.precondition.expected_generation = RackGeneration::create(generation.value()).value();
  request.precondition.expected_membership_generation =
      MembershipGeneration::create(membership.value()).value();
  request.member_id = member_id.value();
  request.replacement_asset = asset.value();
  request.target_state = state.value();
  request.requirements.required = required_traits.value();
  request.requirements.forbids = forbidden_traits.value();
  request.identity.provenance = provenance.value();
  request.identity.request_id = request_id.value();

  auto store = open_write(args);
  if (!store) {
    return finish(store.error());
  }
  const auto receipt = store.value()->replace_member(request);
  if (!receipt) {
    return finish(receipt.error());
  }
  return report_receipt(args, receipt.value());
}

int command_takeover(const Arguments& args) {
  if (!args.has("writer")) {
    return finish(Arguments::usage_error("--writer is required"));
  }
  const auto writer = WriterId::parse(args.get("writer"));
  if (!writer) {
    return finish(writer.error());
  }
  const std::string at_text = args.get("at");
  const std::uint64_t at =
      at_text.empty() ? rackregistry::cli::now_unix_ns()
                      : rackregistry::cli::parse_u64(at_text, "at").value_or(0);
  const auto info = RackStore::force_takeover(state_path(args), writer.value(), at);
  if (!info) {
    return finish(info.error());
  }
  std::cout << "took over writer authority: " << info.value().to_text() << "\n";
  return kExitOk;
}

int command_version() {
  std::cout << "rack registry " << version_string() << "\n";
  std::cout << "state format version " << kStateFormatVersion << "\n";
  std::cout << "snapshot layout version " << kSnapshotLayoutVersion << "\n";
  std::cout << "mount slots per rack unit " << kMountSlotsPerRackUnit << "\n";
  return kExitOk;
}

}  // namespace

namespace {

// Splits the argument list into the command and its options. The command is the
// first token that is not an option and not the value of an option, so global
// options may appear before or after the command name.
struct SplitArguments {
  std::string command{};
  Arguments rest{std::vector<std::string>{}};
};

SplitArguments split_arguments(std::vector<std::string> tokens) {
  SplitArguments split;
  std::vector<std::string> remaining;
  const std::vector<std::string>& flags = rackregistry::cli::all_flag_names();
  bool have_command = false;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::string& token = tokens[index];
    const bool is_option = token.size() >= 2 && token[0] == '-' && token[1] == '-';
    if (!have_command && !is_option) {
      split.command = token;
      have_command = true;
      continue;
    }
    remaining.push_back(token);
    if (!is_option || token.find('=') != std::string::npos) {
      continue;
    }
    const std::string name = token.substr(2);
    const bool takes_value = std::find(flags.begin(), flags.end(), name) == flags.end();
    if (takes_value && index + 1 < tokens.size()) {
      // The next token is this option's value, whatever it looks like.
      remaining.push_back(tokens[++index]);
    }
  }
  split.rest = Arguments(std::move(remaining));
  return split;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; ++i) {
    tokens.emplace_back(argv[i]);
  }
  if (tokens.empty()) {
    print_usage();
    return kExitUsage;
  }

  SplitArguments split = split_arguments(std::move(tokens));
  if (split.command.empty()) {
    print_usage();
    return kExitUsage;
  }
  const std::string command = split.command;
  Arguments& args = split.rest;

  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return kExitOk;
  }

  const Status parsed =
      args.parse(rackregistry::cli::all_option_names(), rackregistry::cli::all_flag_names());
  if (!parsed) {
    rackregistry::cli::print_error(parsed.error());
    return kExitUsage;
  }

  if (command == "version") {
    return command_version();
  }
  if (command == "list") {
    return command_list(args);
  }
  if (command == "show") {
    return command_show(args);
  }
  if (command == "members") {
    return command_members(args);
  }
  if (command == "occupancy") {
    return command_occupancy(args);
  }
  if (command == "free") {
    return command_free(args);
  }
  if (command == "shared") {
    return command_shared(args);
  }
  if (command == "diff") {
    return command_diff(args);
  }
  if (command == "verify") {
    return command_verify(args);
  }
  if (command == "recover") {
    return command_recover(args);
  }
  if (command == "lock-status") {
    return command_lock_status(args);
  }
  if (command == "rejections") {
    return command_rejections(args);
  }
  if (command == "export") {
    return command_export(args);
  }
  if (command == "register" || command == "init") {
    return command_register(args);
  }
  if (command == "set-structure") {
    return command_set_structure(args);
  }
  if (command == "lifecycle") {
    return command_lifecycle(args);
  }
  if (command == "insert") {
    return command_insert(args);
  }
  if (command == "remove") {
    return command_remove(args);
  }
  if (command == "move") {
    return command_move(args);
  }
  if (command == "replace") {
    return command_replace(args);
  }
  if (command == "takeover") {
    return command_takeover(args);
  }

  std::cerr << "error: unknown command '" << command << "'\n";
  print_usage();
  return kExitUsage;
}
