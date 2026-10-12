#pragma once

namespace forever_module {

// protocol_version is the exact-match gate against core's compiled-in wire protocol
// (doc/module-system/02-phase0-decisions.md §3). Only bump this on breaking changes to the
// Tier 1 message schema or the Tier 2 shared-memory layout.
inline constexpr char const* kProtocolVersion = "1";

// sdk_package_version is this SDK repo's own release tag (client lib code, build scripts, docs).
// Can bump independently of kProtocolVersion for bugfixes/improvements that don't change the wire
// contract. See doc/module-system/11-sdk-scope.md for the full versioning split rationale.
inline constexpr char const* kSdkPackageVersion = "0.1.0-skeleton";

} // namespace forever_module
