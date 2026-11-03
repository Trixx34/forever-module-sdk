// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace forever_module {

// protocol_version is the exact-match gate against core's compiled-in wire protocol
// (doc/module-system/04-phase2-module-host-design.md; core's kModuleProtocolVersion must equal this).
// Only bump this on breaking changes to the Tier 1 message schema or the Tier 2 shared-memory layout.
// Bump history: 20 -> 21 adds EmoteAction (ActionRequest fields 27 and 28, with LfgJoinAction), SpeakKind kParty/kGuild, the core-to-module
// GuildMemberLevelEvent and LfgQueueEvent, and ActionType kEmote/kLfgJoin; all additive, but the core and the SDK move together to "21".
// Bump history: 21 -> 22 adds the core-to-module GroupInviteDeclinedEvent (EventNotify field 22, hook GroupScript.OnInviteDeclined) and
// the Tier 2 layout 23 (NearbyUnit item_level and shared_objective, BotSnapshot in_capital and guild_guid).
inline constexpr char const* kProtocolVersion = "22";

// sdk_package_version is this SDK repo's own release tag (client lib code, build scripts, docs).
// Can bump independently of kProtocolVersion for bugfixes/improvements that don't change the wire
// contract. See doc/module-system/11-sdk-scope.md for the full versioning split rationale.
// Pre-1.0 scheme (forever-ops docs/96-versioning.md): 0.MINOR.PATCH, MINOR for a protocol or layout change or a new capability, PATCH for fixes. The
// same number is in CMakeLists.txt (project VERSION); the tier1_roundtrip test fails when the two differ.
inline constexpr char const* kSdkPackageVersion = "0.25.0";

} // namespace forever_module
