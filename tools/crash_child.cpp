// Rack Registry - crash injection child process.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program performs one real mutation against a real store and terminates
// the process without unwinding at a named durable step, so that the parent
// test can reopen the store and prove that recovery is conservative. It is
// built only when the test suite is enabled and is not installed.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cli_support.hpp"

namespace {

using namespace rackregistry;  // NOLINT(google-build-using-namespace)

int usage() {
  std::cerr << "usage: rr_crash_child <state-file> <writer> <stage> <rack-id> [--units N]\n"
               "                     [--member rm:x] [--asset a:y] [--mount M]\n"
               "                     [--generation N] [--membership-generation N]\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> tokens;
  for (int i = 1; i < argc; ++i) {
    tokens.emplace_back(argv[i]);
  }
  if (tokens.size() < 4) {
    return usage();
  }

  const std::string state_path = tokens[0];
  const std::string writer_text = tokens[1];
  const std::string stage_text = tokens[2];
  const std::string rack_text = tokens[3];

  std::vector<std::string> option_tokens(tokens.begin() + 4, tokens.end());
  rackregistry::cli::Arguments args(std::move(option_tokens));
  const Status parsed = args.parse({"units", "member", "asset", "mount", "generation",
                                    "membership-generation", "operation"},
                                   {});
  if (!parsed) {
    rackregistry::cli::print_error(parsed.error());
    return 1;
  }

  const auto stage = parse_write_stage(stage_text);
  if (!stage) {
    rackregistry::cli::print_error(stage.error());
    return 1;
  }
  const auto writer = WriterId::parse(writer_text);
  if (!writer) {
    rackregistry::cli::print_error(writer.error());
    return 1;
  }
  const auto rack_id = RackId::parse(rack_text);
  if (!rack_id) {
    rackregistry::cli::print_error(rack_id.error());
    return 1;
  }

  StoreOptions options;
  options.path = state_path;
  options.writer_id = writer.value();
  options.producer = SourceReference::parse("rr_crash_child/1.0.0").value();
  options.fault_hook = [wanted = stage.value()](WriteStage current) {
    if (current == wanted) {
      std::cout << "crashing at " << write_stage_name(current) << std::endl;
      std::cout.flush();
      std::abort();
    }
  };

  auto store = RackStore::open(options);
  if (!store) {
    rackregistry::cli::print_error(store.error());
    return 2;
  }

  const std::string operation = args.get("operation", "register");
  ProvenanceRecord provenance;
  provenance.source = ProvenanceSource::System;
  provenance.actor = ActorId::parse("crash-child").value();
  provenance.observed_at_unix_ns = 1000;

  if (operation == "register") {
    RegisterRackRequest request;
    request.structure.id = rack_id.value();
    request.structure.unit_count =
        static_cast<std::uint32_t>(rackregistry::cli::parse_u64(args.get("units", "4"), "units")
                                       .value_or(4));
    request.structure.profile.id = CompatibilityProfileId::parse("cp:crash").value();
    request.identity.provenance = provenance;
    const auto receipt = store.value()->register_rack(request);
    if (!receipt) {
      rackregistry::cli::print_error(receipt.error());
      return 2;
    }
    std::cout << receipt.value().to_text() << std::endl;
    return 0;
  }

  if (operation == "insert") {
    InsertMemberRequest request;
    request.rack_id = rack_id.value();
    request.precondition.expected_generation =
        RackGeneration::create(rackregistry::cli::parse_u64(args.get("generation", "1"),
                                                            "generation")
                                   .value_or(1))
            .value();
    request.precondition.expected_membership_generation =
        MembershipGeneration::create(rackregistry::cli::parse_u64(
                                         args.get("membership-generation", "0"),
                                         "membership-generation")
                                         .value_or(0))
            .value();
    request.member_id = RackMemberId::parse(args.get("member", "rm:crash")).value();
    request.asset_id = AssetId::parse(args.get("asset", "asset:crash")).value();
    request.mount = rackregistry::cli::parse_mount(args.get("mount", "U1")).value();
    request.identity.provenance = provenance;
    const auto receipt = store.value()->insert_member(request);
    if (!receipt) {
      rackregistry::cli::print_error(receipt.error());
      return 2;
    }
    std::cout << receipt.value().to_text() << std::endl;
    return 0;
  }

  std::cerr << "unknown operation\n";
  return 1;
}
