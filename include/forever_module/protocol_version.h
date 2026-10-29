#pragma once

namespace forever_module {

// protocol_version is the exact-match gate against core's compiled-in wire protocol
// (doc/module-system/04-phase2-module-host-design.md; core's kModuleProtocolVersion must equal this).
// Only bump this on breaking changes to the Tier 1 message schema or the Tier 2 shared-memory layout.
inline constexpr char const* kProtocolVersion = "19";

// sdk_package_version is this SDK repo's own release tag (client lib code, build scripts, docs).
// Can bump independently of kProtocolVersion for bugfixes/improvements that don't change the wire
// contract. See doc/module-system/11-sdk-scope.md for the full versioning split rationale.
// Pre-1.0 scheme (forever-ops docs/96-versioning.md): 0.MINOR.PATCH, MINOR for a protocol or layout change or a new capability, PATCH for fixes. The
// same number is in CMakeLists.txt (project VERSION); the tier1_roundtrip test fails when the two differ.
inline constexpr char const* kSdkPackageVersion = "0.23.0";

} // namespace forever_module
