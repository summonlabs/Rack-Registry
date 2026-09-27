// Rack Registry - benchmarks.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every measurement below times a *completed* operation. The durability
// benchmarks include the full publication sequence, which means serialization,
// the temporary write, the device flush, the read-back verification and the
// atomic replace. Nothing here measures submission or enqueue latency, and
// nothing here claims to represent a production facility workload: the shapes
// are synthetic and the scale is reported with every result.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "rack_registry/rack_registry.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

constexpr std::uint64_t kBaseTimeNs = 1'800'000'000'000'000'000ull;

struct Timing {
  double total_ms = 0.0;
  std::uint64_t operations = 0;

  [[nodiscard]] double per_operation_us() const {
    return operations == 0 ? 0.0 : (total_ms * 1000.0) / static_cast<double>(operations);
  }
  [[nodiscard]] double operations_per_second() const {
    return total_ms <= 0.0 ? 0.0 : (static_cast<double>(operations) * 1000.0) / total_ms;
  }
};

class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  [[nodiscard]] double milliseconds() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

ProvenanceRecord provenance(std::uint64_t sequence) {
  ProvenanceRecord record;
  record.source = ProvenanceSource::System;
  record.actor = ActorId::parse("benchmark").value();
  record.source_sequence = sequence;
  record.observed_at_unix_ns = kBaseTimeNs + sequence;
  return record;
}

void report(std::string_view name, const Timing& timing, std::string_view scale) {
  std::cout << "bench " << name << " scale=\"" << scale << "\""
            << " operations=" << timing.operations << " total_ms=" << timing.total_ms
            << " per_operation_us=" << timing.per_operation_us()
            << " operations_per_second=" << timing.operations_per_second() << "\n";
}

struct Shape {
  std::size_t racks = 0;
  std::size_t members_per_rack = 0;
  std::uint32_t units = 0;
};

// Builds a registry with the requested shape. Membership is placed as half-unit
// spans so that a rack of `units` rack units can host 2 * units members, which
// keeps the synthetic shape internally consistent.
Shape build_registry(RackRegistry& registry, std::size_t racks, std::size_t members_per_rack,
                     std::uint32_t units) {
  Shape shape;
  shape.racks = racks;
  shape.members_per_rack = members_per_rack;
  shape.units = units;

  for (std::size_t r = 0; r < racks; ++r) {
    const std::string body = "r" + std::to_string(r);
    RegisterRackRequest request;
    request.structure.id = RackId::parse("rack:" + body).value();
    request.structure.unit_count = units;
    request.structure.profile.id = CompatibilityProfileId::parse("cp:bench").value();
    request.structure.profile.provides =
        TraitSet::parse("power.ac.208v,rail.depth.800mm").value();
    request.structure.power_domains = {PowerDomainReference::parse("pdu-" + body).value()};
    request.structure.cooling_domains = {CoolingDomainReference::parse("crac-" + body).value()};
    request.identity.provenance = provenance(1);
    const auto registered = registry.register_rack(request);
    if (!registered) {
      std::cerr << "benchmark setup failed: " << describe(registered.error()) << "\n";
      return Shape{};
    }
    RackGeneration generation = registered.value().generation;
    MembershipGeneration membership = registered.value().membership_generation;
    for (std::size_t m = 0; m < members_per_rack; ++m) {
      InsertMemberRequest insert;
      insert.rack_id = request.structure.id;
      insert.precondition.expected_generation = generation;
      insert.precondition.expected_membership_generation = membership;
      insert.member_id = RackMemberId::parse("rm:m" + std::to_string(m)).value();
      insert.asset_id =
          AssetId::parse("asset:" + body + "-" + std::to_string(m)).value();
      const std::uint32_t unit = static_cast<std::uint32_t>(m / 2) + 1;
      insert.mount = MountSpan::full(
                         SlotRange::half_unit(RackUnitIndex::create(unit).value(),
                                              (m % 2 == 0) ? HalfSlot::Lower : HalfSlot::Upper)
                             .value())
                         .value();
      insert.requirements.required = TraitSet::parse("power.ac.208v").value();
      insert.identity.provenance = provenance(2);
      const auto receipt = registry.insert_member(insert);
      if (!receipt) {
        std::cerr << "benchmark setup failed: " << describe(receipt.error()) << "\n";
        return Shape{};
      }
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
    }
  }
  return shape;
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

void bench_occupancy_validation(std::size_t racks, std::size_t members_per_rack) {
  RackRegistry registry;
  const std::uint32_t units = static_cast<std::uint32_t>(members_per_rack / 2 + 1);
  const Shape shape = build_registry(registry, racks, members_per_rack, units);
  if (shape.racks == 0) {
    return;
  }

  // A rejected placement forces the registry to walk the occupancy of the rack,
  // which is exactly the validation cost being measured. The measured operation
  // is the complete rejected mutation.
  Timing timing;
  Stopwatch watch;
  for (std::size_t r = 0; r < racks; ++r) {
    const RackId rack_id = RackId::parse("rack:r" + std::to_string(r)).value();
    const auto view = registry.rack(rack_id);
    if (!view) {
      continue;
    }
    for (int repeat = 0; repeat < 4; ++repeat) {
      InsertMemberRequest insert;
      insert.rack_id = rack_id;
      insert.precondition.expected_generation = view.value().generation();
      insert.precondition.expected_membership_generation = view.value().membership_generation();
      insert.member_id = RackMemberId::parse("rm:probe").value();
      insert.asset_id = AssetId::parse("asset:probe").value();
      insert.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 1).value()).value();
      insert.identity.provenance = provenance(3);
      const auto receipt = registry.insert_member(insert);
      if (receipt) {
        std::cerr << "benchmark probe unexpectedly succeeded\n";
      }
      ++timing.operations;
    }
  }
  timing.total_ms = watch.milliseconds();
  report("completed_occupancy_validation", timing,
         std::to_string(racks) + " racks x " + std::to_string(members_per_rack) +
             " members, rejected insert validated against the full membership");
  std::cout << "bench integrity occupancy_registry_digest=" << registry.state_digest().to_hex()
            << " racks=" << registry.rack_count() << " members=" << registry.stats().member_count
            << "\n";
}

void bench_membership_mutation(std::size_t racks, std::size_t members_per_rack) {
  // The setup pass fills the lower half of each rack, so the measured inserts
  // use full rack units above it and every one of them is accepted.
  const std::uint32_t units = static_cast<std::uint32_t>(2 * members_per_rack + 2);
  RackRegistry registry;
  const Shape shape = build_registry(registry, racks, members_per_rack, units);
  if (shape.racks == 0) {
    return;
  }

  Timing timing;
  Stopwatch watch;
  for (std::size_t r = 0; r < racks; ++r) {
    const std::string body = "r" + std::to_string(r);
    const RackId rack_id = RackId::parse("rack:" + body).value();
    const auto view = registry.rack(rack_id);
    if (!view) {
      continue;
    }
    RackGeneration generation = view.value().generation();
    MembershipGeneration membership = view.value().membership_generation();
    for (std::size_t m = 0; m < members_per_rack; ++m) {
      InsertMemberRequest insert;
      insert.rack_id = rack_id;
      insert.precondition.expected_generation = generation;
      insert.precondition.expected_membership_generation = membership;
      insert.member_id = RackMemberId::parse("rm:x" + std::to_string(m)).value();
      insert.asset_id = AssetId::parse("asset:x" + body + "-" + std::to_string(m)).value();
      insert.mount = MountSpan::full_units(
                         RackUnitRange::single(RackUnitIndex::create(
                                                   static_cast<std::uint32_t>(m) +
                                                   static_cast<std::uint32_t>(members_per_rack) + 1)
                                                   .value())
                             .value())
                         .value();
      insert.identity.provenance = provenance(4);
      const auto receipt = registry.insert_member(insert);
      if (!receipt) {
        std::cerr << "benchmark insert failed: " << describe(receipt.error()) << "\n";
        break;
      }
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
      ++timing.operations;
    }
  }
  timing.total_ms = watch.milliseconds();
  report("completed_membership_mutation", timing,
         std::to_string(racks) + " racks x " + std::to_string(members_per_rack) +
             " accepted inserts, in-memory only");
  std::cout << "bench integrity membership_registry_digest=" << registry.state_digest().to_hex()
            << " racks=" << registry.rack_count() << " members=" << registry.stats().member_count
            << "\n";
}

void bench_enumeration(std::size_t racks, std::size_t members_per_rack) {
  RackRegistry registry;
  const std::uint32_t units = static_cast<std::uint32_t>(members_per_rack / 2 + 1);
  const Shape shape = build_registry(registry, racks, members_per_rack, units);
  if (shape.racks == 0) {
    return;
  }

  Timing timing;
  Stopwatch watch;
  std::size_t observed = 0;
  for (int repeat = 0; repeat < 20; ++repeat) {
    for (std::size_t r = 0; r < racks; ++r) {
      const RackId rack_id = RackId::parse("rack:r" + std::to_string(r)).value();
      const auto members = registry.members(rack_id, MemberOrder::MountOrder);
      if (!members) {
        continue;
      }
      observed += members.value().size();
      ++timing.operations;
    }
  }
  timing.total_ms = watch.milliseconds();
  report("completed_deterministic_enumeration", timing,
         std::to_string(racks) + " racks x " + std::to_string(members_per_rack) +
             " members x 20 repetitions, mount-ordered");
  std::cout << "bench integrity enumeration_members_observed=" << observed << "\n";
}

void bench_generation_diff(std::size_t racks, std::size_t members_per_rack) {
  const std::uint32_t units = static_cast<std::uint32_t>(members_per_rack + 2);
  RackRegistry registry;
  const Shape shape = build_registry(registry, racks, members_per_rack, units);
  if (shape.racks == 0) {
    return;
  }

  Timing timing;
  Stopwatch watch;
  std::size_t changes = 0;
  for (std::size_t r = 0; r < racks; ++r) {
    const RackId rack_id = RackId::parse("rack:r" + std::to_string(r)).value();
    const auto view = registry.rack(rack_id);
    if (!view) {
      continue;
    }
    const std::uint64_t current = view.value().generation().value();
    if (current < 2) {
      continue;
    }
    const auto from = RackGeneration::create(current - 1);
    const auto diff = registry.diff_generations(rack_id, from.value());
    if (!diff) {
      continue;
    }
    changes += diff.value().member_changes.size();
    ++timing.operations;
  }
  timing.total_ms = watch.milliseconds();
  report("completed_generation_diff", timing,
         std::to_string(racks) + " racks x " + std::to_string(members_per_rack) +
             " members, adjacent retained generations");
  std::cout << "bench integrity generation_diff_member_changes=" << changes << "\n";
}

void bench_durable_publication(const std::filesystem::path& directory, std::size_t racks,
                               std::size_t members_per_rack) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path state_file = directory / "benchmark.rrstate";
  std::filesystem::remove(state_file, error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);

  StoreOptions options;
  options.path = state_file;
  options.writer_id = WriterId::parse("wr:benchmark").value();
  options.producer = SourceReference::parse("bench_rack_registry/1.0.0").value();

  auto store = RackStore::open(options);
  if (!store) {
    std::cerr << "benchmark store open failed: " << describe(store.error()) << "\n";
    return;
  }

  const std::uint32_t units = static_cast<std::uint32_t>(members_per_rack + 2);
  Timing registration_timing;
  Timing insert_timing;
  const std::size_t inserts_per_rack = members_per_rack > 4 ? 4 : members_per_rack;

  for (std::size_t r = 0; r < racks; ++r) {
    const std::string body = "d" + std::to_string(r);
    RegisterRackRequest request;
    request.structure.id = RackId::parse("rack:" + body).value();
    request.structure.unit_count = units;
    request.structure.profile.id = CompatibilityProfileId::parse("cp:bench").value();
    request.structure.profile.provides = TraitSet::parse("power.ac.208v").value();
    request.identity.provenance = provenance(5);
    Stopwatch watch;
    const auto registered = store.value()->register_rack(request);
    registration_timing.total_ms += watch.milliseconds();
    if (!registered) {
      std::cerr << "benchmark durable register failed: " << describe(registered.error()) << "\n";
      break;
    }
    ++registration_timing.operations;
    RackGeneration generation = registered.value().generation;
    MembershipGeneration membership = registered.value().membership_generation;
    for (std::size_t m = 0; m < inserts_per_rack; ++m) {
      InsertMemberRequest insert;
      insert.rack_id = request.structure.id;
      insert.precondition.expected_generation = generation;
      insert.precondition.expected_membership_generation = membership;
      insert.member_id = RackMemberId::parse("rm:d" + std::to_string(m)).value();
      insert.asset_id = AssetId::parse("asset:" + body + "-" + std::to_string(m)).value();
      insert.mount = MountSpan::full_units(
                         RackUnitRange::single(RackUnitIndex::create(
                                                   static_cast<std::uint32_t>(m) + 1)
                                                   .value())
                             .value())
                         .value();
      insert.identity.provenance = provenance(6);
      Stopwatch insert_watch;
      const auto receipt = store.value()->insert_member(insert);
      insert_timing.total_ms += insert_watch.milliseconds();
      if (!receipt) {
        std::cerr << "benchmark durable insert failed: " << describe(receipt.error()) << "\n";
        break;
      }
      generation = receipt.value().generation;
      membership = receipt.value().membership_generation;
      ++insert_timing.operations;
    }
  }

  const auto info = RackStore::inspect(state_file);
  report("completed_durable_registration", registration_timing,
         std::to_string(racks) + " racks, each including flush, read-back verification and "
                                 "atomic publication");
  report("completed_durable_membership_mutation", insert_timing,
         std::to_string(racks) + " racks x " + std::to_string(inserts_per_rack) +
             " accepted inserts, each durably published");
  if (info) {
    std::cout << "bench integrity durable_state=" << info.value().to_text() << "\n";
  } else {
    std::cerr << "bench integrity durable_state=unreadable: " << describe(info.error()) << "\n";
  }

  const auto recovery = store.value()->recovery();
  std::cout << "bench recovery " << recovery.to_text() << "\n";
  (void)store.value()->close();

  std::filesystem::remove(state_file, error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);
  std::filesystem::remove_all(directory, error);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t racks = 64;
  std::size_t members_per_rack = 32;
  std::filesystem::path scratch;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--racks" && i + 1 < argc) {
      racks = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (token == "--members" && i + 1 < argc) {
      members_per_rack = static_cast<std::size_t>(std::stoull(argv[++i]));
    } else if (token == "--scratch" && i + 1 < argc) {
      scratch = argv[++i];
    }
  }
  if (scratch.empty()) {
    std::error_code error;
    scratch = std::filesystem::temp_directory_path(error) / "rack-registry-benchmark";
  }

  std::cout << "rack_registry_benchmark version=" << version_string() << " racks=" << racks
            << " members_per_rack=" << members_per_rack << " scratch=\"" << scratch.string()
            << "\"\n";

  bench_occupancy_validation(racks, members_per_rack);
  bench_membership_mutation(racks, members_per_rack);
  bench_enumeration(racks, members_per_rack);
  bench_generation_diff(racks, members_per_rack);
  bench_durable_publication(scratch, racks, members_per_rack);
  return 0;
}
