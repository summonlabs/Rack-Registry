// Rack Registry - umbrella header.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rack Registry is the canonical rack-composition runtime of the Data Center
// Control Plane, Tranche 1. It owns rack identities, physical unit extent,
// membership and occupancy inside a rack, power and cooling domain
// associations, compatibility metadata, rack lifecycle, and generation-bound
// membership authority.
//
// It does not own facility topology, general physical addressing, asset
// internal lifecycle, capacity planning, electrical or cooling control,
// placement optimization, or reservations of future capacity.

#pragma once

#include "rack_registry/compatibility.hpp"
#include "rack_registry/diff.hpp"
#include "rack_registry/digest.hpp"
#include "rack_registry/errors.hpp"
#include "rack_registry/ids.hpp"
#include "rack_registry/lifecycle.hpp"
#include "rack_registry/limits.hpp"
#include "rack_registry/member.hpp"
#include "rack_registry/mount.hpp"
#include "rack_registry/persistence.hpp"
#include "rack_registry/provenance.hpp"
#include "rack_registry/rack.hpp"
#include "rack_registry/registry.hpp"
#include "rack_registry/requests.hpp"
#include "rack_registry/result.hpp"
#include "rack_registry/text.hpp"
#include "rack_registry/version.hpp"
