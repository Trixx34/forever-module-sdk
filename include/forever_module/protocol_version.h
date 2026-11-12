// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace forever_module {

// protocol_version is the gate against core's compiled-in wire protocol (doc/module-system/04-phase2-module-host-design.md; core's
// kModuleProtocolVersion must equal this in the same revision). From layout 25 a core accepts any module protocol from its kModuleProtocolBase up to
// its own (a newer core of the same layout base serves this module); a different base is refused. This SDK sends kProtocolVersion as before.
// Only bump this on breaking changes to the Tier 1 message schema or the Tier 2 shared-memory layout.
// Bump history: 20 -> 21 adds EmoteAction (ActionRequest fields 27 and 28, with LfgJoinAction), SpeakKind kParty/kGuild, the core-to-module
// GuildMemberLevelEvent and LfgQueueEvent, and ActionType kEmote/kLfgJoin; all additive, but the core and the SDK move together to "21".
// Bump history: 21 -> 22 adds the core-to-module GroupInviteDeclinedEvent (EventNotify field 22, hook GroupScript.OnInviteDeclined) and
// the Tier 2 layout 23 (NearbyUnit item_level and shared_objective, BotSnapshot in_capital and guild_guid).
// Bump history: 22 -> 23 adds GuildAction kCreate (SocialOp 3) and GuildAction.name (field 3): the bot creates a guild.
// 23 -> 24 adds GuildAction kInvite (SocialOp 4) and GuildAction.player_guid (field 4): the bot invites a character to its guild.
// Both are additive, but the core and the SDK move together, so an older core refuses the handshake instead of mis-reading the request.
// 24 -> 25 is the Tier 2 layout 24 (the bot's perception: NearbyUnit victim/threat/cast/type/flags/orientation/raid mark/own debuffs, BotSnapshot gcd_remaining_ms,
// movement_flags, transport_guid, banker, target auras, an action result ring, 16 cooldown slots). The Tier 1 messages do not change.
// 25 -> 26 is the Tier 2 layout 25 (the party view and enemy telegraphs: one shared PartyView per group with a PartyMember pool, 8 ground effects,
// Telegraph[4], ProbeResult[4] and kProbePoint, kStopCast, kTransport (reserved), NearbyUnit aggro_radius, the section table and the growth rule
// in Tier2RegionHeader). The Tier 1 messages do not change.
// 26 -> 27 adds the core-to-module BossKilledEvent (EventNotify field 23, hook PlayerScript.OnBossKill), and in layout 25 (no layout bump: the shapes keep their
// size) ActionResult::reason (was reserved), kQuest operation 5 and kVendor bit 2048. An append: kProtocolBase stays 26, a protocol 26 module is still served.
// 27 -> 28 adds, in layout 25 (no layout bump, append only), kResurrect spell_id 2 (take the spirit healer) and the meaning of BotAction::dest_x >= 1 on
// kCastSpell (cast spell_id exactly, no rank lookup). kProtocolBase stays 26.
inline constexpr char const* kProtocolVersion = "28";

// sdk_package_version is this SDK repo's own release tag (client lib code, build scripts, docs).
// Can bump independently of kProtocolVersion for bugfixes/improvements that don't change the wire
// contract. See doc/module-system/11-sdk-scope.md for the full versioning split rationale.
// Pre-1.0 scheme (forever-ops docs/96-versioning.md): 0.MINOR.PATCH, MINOR for a protocol or layout change or a new capability, PATCH for fixes. The
// same number is in CMakeLists.txt (project VERSION); the tier1_roundtrip test fails when the two differ.
inline constexpr char const* kSdkPackageVersion = "0.29.0";

} // namespace forever_module
