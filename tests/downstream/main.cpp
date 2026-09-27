// Rack Registry - independent downstream consumer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is built outside the Rack Registry source tree, against an
// installed package, and it uses only the installed public headers and the
// imported target. It exercises the parts of the surface a later DCCP
// repository would compose: register a rack, place members, query occupancy,
// persist through the durable store and read the state back.

#include <filesystem>
#include <iostream>
#include <string>

#include <rack_registry/rack_registry.hpp>

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

int fail(const char* what, const RackError& error) {
  std::cerr << "downstream consumer failed at " << what << ": " << describe(error) << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path state_file =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "rack-registry-downstream.rrstate";
  std::error_code error;
  std::filesystem::remove(state_file, error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);

  std::cout << "rack_registry " << version_string() << " state format " << kStateFormatVersion
            << "\n";

  StoreOptions options;
  options.path = state_file;
  options.writer_id = WriterId::parse("wr:downstream").value();
  options.producer = SourceReference::parse("downstream-consumer/1.0.0").value();

  auto store = RackStore::open(options);
  if (!store) {
    return fail("open", store.error());
  }

  RegisterRackRequest registration;
  registration.structure.id = RackId::parse("rack:downstream-a01").value();
  registration.structure.unit_count = 42;
  registration.structure.profile.id = CompatibilityProfileId::parse("cp:general").value();
  registration.structure.profile.provides =
      TraitSet::parse("power.ac.208v,rail.depth.800mm,cooling.rear-intake").value();
  registration.structure.power_domains = {PowerDomainReference::parse("pdu-a/feed-1").value()};
  registration.structure.cooling_domains = {CoolingDomainReference::parse("crac-3/loop-b").value()};
  registration.identity.provenance.source = ProvenanceSource::Operator;
  registration.identity.provenance.actor = ActorId::parse("downstream").value();
  registration.identity.provenance.source_sequence = 1;
  registration.identity.provenance.observed_at_unix_ns = 1'800'000'000'000'000'000ull;

  const auto registered = store.value()->register_rack(registration);
  if (!registered) {
    return fail("register_rack", registered.error());
  }

  RackGeneration generation = registered.value().generation;
  MembershipGeneration membership = registered.value().membership_generation;
  for (int index = 0; index < 3; ++index) {
    InsertMemberRequest insert;
    insert.rack_id = registration.structure.id;
    insert.precondition.expected_generation = generation;
    insert.precondition.expected_membership_generation = membership;
    insert.member_id =
        RackMemberId::parse("rm:downstream-" + std::to_string(index)).value();
    insert.asset_id = AssetId::parse("asset:downstream-" + std::to_string(index)).value();
    insert.mount =
        MountSpan::full_units(RackUnitRange::single(RackUnitIndex::create(
                                                       static_cast<std::uint32_t>(index) + 1)
                                                       .value())
                                  .value())
            .value();
    insert.requirements.required = TraitSet::parse("power.ac.208v").value();
    insert.identity.provenance = registration.identity.provenance;
    insert.identity.request_id = RequestId::parse("downstream-" + std::to_string(index)).value();

    const auto receipt = store.value()->insert_member(insert);
    if (!receipt) {
      return fail("insert_member", receipt.error());
    }
    generation = receipt.value().generation;
    membership = receipt.value().membership_generation;

    // Retrying the identical request must be answered from the idempotency
    // table rather than applied twice.
    const auto replay = store.value()->insert_member(insert);
    if (!replay || !replay.value().replayed) {
      return fail("replayed insert_member", replay ? make_error(ErrorCode::InvalidStateEncoding,
                                                               "replay was not recognised")
                                                  : replay.error());
    }
  }

  const auto occupancy = store.value()->occupancy(registration.structure.id);
  if (!occupancy) {
    return fail("occupancy", occupancy.error());
  }
  for (const OccupancyRecord& record : occupancy.value()) {
    std::cout << "  " << record.mount.to_text() << " "
              << (record.units.has_value() ? record.units->to_text() : std::string("zero-u")) << " "
              << record.member_id.text() << " asset=" << record.asset_id.text() << "\n";
  }

  const auto free_spans = store.value()->free_spans(registration.structure.id);
  if (!free_spans) {
    return fail("free_spans", free_spans.error());
  }
  std::cout << "  free ranges " << free_spans.value().size() << ", first "
            << (free_spans.value().empty() ? std::string("none") : free_spans.value()[0].to_text())
            << "\n";

  const RegistryStats stats = store.value()->stats();
  std::cout << "  racks " << stats.rack_count << " members " << stats.member_count << "\n";

  if (!store.value()->close()) {
    return fail("close", store.value()->close().error());
  }

  // Reopen and prove the generation survived, then verify the file without
  // taking authority.
  auto reopened = RackStore::open(options);
  if (!reopened) {
    return fail("reopen", reopened.error());
  }
  if (reopened.value()->rack_count() != 1) {
    std::cerr << "downstream consumer failed: reopened store does not hold the rack\n";
    return 1;
  }
  const auto view = reopened.value()->rack(registration.structure.id);
  if (!view) {
    return fail("rack", view.error());
  }
  if (view.value().generation().value() != generation.value() ||
      view.value().member_count() != 3) {
    std::cerr << "downstream consumer failed: reopened generation does not match\n";
    return 1;
  }
  if (!reopened.value()->close()) {
    return fail("close", reopened.value()->close().error());
  }

  const auto info = RackStore::inspect(state_file);
  if (!info) {
    return fail("inspect", info.error());
  }
  std::cout << "  verified " << info.value().to_text() << "\n";

  std::filesystem::remove(state_file, error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".prev"), error);
  std::filesystem::remove(std::filesystem::path(state_file.string() + ".lock"), error);
  std::cout << "downstream consumer: ok\n";
  return 0;
}
