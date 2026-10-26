#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace forever_module {

// The talent specs a module can name in SpendBotTalents (protocol 18). The numbering is part of the wire contract: SpendBotTalentsAction.spec carries
// the value, and the core (Bots/BotTalentSpec.h) keeps the same table; the core's interop test compares the two. kNone (0) means "no spec": the core
// then looks the bot's profile up by its role only, as it did before protocol 18.
//
// Key is the lowercase word a profile file line uses after the class id (`<classId> <key> <nodeId>:<ranks>,...`): the tank, healer and dps role
// keys keep working, and a spec key (below) names that spec's profile. A key is unique inside its class, not across classes (prot is both the
// warrior's and the paladin's, holy the paladin's and the priest's, resto the shaman's and the druid's).
enum class TalentSpec : std::uint32_t {
    kNone = 0,
    kWarriorArms = 1,
    kWarriorFury = 2,
    kWarriorProt = 3,
    kPaladinHoly = 4,
    kPaladinProt = 5,
    kPaladinRet = 6,
    kHunterBm = 7,
    kHunterMm = 8,
    kHunterSv = 9,
    kRogueAssassination = 10,
    kRogueCombat = 11,
    kRogueSubtlety = 12,
    kPriestDisc = 13,
    kPriestHoly = 14,
    kPriestShadow = 15,
    kShamanEle = 16,
    kShamanEnh = 17,
    kShamanResto = 18,
    kMageArcane = 19,
    kMageFire = 20,
    kMageFrost = 21,
    kWarlockAffliction = 22,
    kWarlockDemonology = 23,
    kWarlockDestruction = 24,
    kDruidBalance = 25,
    kDruidFeralTank = 26,
    kDruidFeralCat = 27,
    kDruidResto = 28,
};

inline constexpr std::uint32_t kLastTalentSpec = 28;

struct TalentSpecInfo {
    TalentSpec Spec;
    std::uint32_t ClassId; // the core's class id: 1 warrior, 2 paladin, 3 hunter, 4 rogue, 5 priest, 7 shaman, 8 mage, 9 warlock, 11 druid
    char const* Key;
};

inline constexpr std::array<TalentSpecInfo, kLastTalentSpec> kTalentSpecs = {{
    { TalentSpec::kWarriorArms, 1, "arms" },
    { TalentSpec::kWarriorFury, 1, "fury" },
    { TalentSpec::kWarriorProt, 1, "prot" },
    { TalentSpec::kPaladinHoly, 2, "holy" },
    { TalentSpec::kPaladinProt, 2, "prot" },
    { TalentSpec::kPaladinRet, 2, "ret" },
    { TalentSpec::kHunterBm, 3, "bm" },
    { TalentSpec::kHunterMm, 3, "mm" },
    { TalentSpec::kHunterSv, 3, "sv" },
    { TalentSpec::kRogueAssassination, 4, "assassination" },
    { TalentSpec::kRogueCombat, 4, "combat" },
    { TalentSpec::kRogueSubtlety, 4, "subtlety" },
    { TalentSpec::kPriestDisc, 5, "disc" },
    { TalentSpec::kPriestHoly, 5, "holy" },
    { TalentSpec::kPriestShadow, 5, "shadow" },
    { TalentSpec::kShamanEle, 7, "ele" },
    { TalentSpec::kShamanEnh, 7, "enh" },
    { TalentSpec::kShamanResto, 7, "resto" },
    { TalentSpec::kMageArcane, 8, "arcane" },
    { TalentSpec::kMageFire, 8, "fire" },
    { TalentSpec::kMageFrost, 8, "frost" },
    { TalentSpec::kWarlockAffliction, 9, "affliction" },
    { TalentSpec::kWarlockDemonology, 9, "demonology" },
    { TalentSpec::kWarlockDestruction, 9, "destruction" },
    { TalentSpec::kDruidBalance, 11, "balance" },
    { TalentSpec::kDruidFeralTank, 11, "feral_tank" },
    { TalentSpec::kDruidFeralCat, 11, "feral_cat" },
    { TalentSpec::kDruidResto, 11, "resto" },
}};

// The table row of a spec, or nullptr for kNone or a value past kLastTalentSpec.
inline constexpr TalentSpecInfo const* GetTalentSpecInfo(TalentSpec spec)
{
    std::uint32_t const value = static_cast<std::uint32_t>(spec);
    return value >= 1 && value <= kLastTalentSpec ? &kTalentSpecs[value - 1] : nullptr;
}

// The spec a class's profile key names, or kNone when the key is not one of that class's spec keys (tank, healer and dps are roles, not specs).
inline constexpr TalentSpec TalentSpecFromKey(std::uint32_t classId, std::string_view key)
{
    for (TalentSpecInfo const& info : kTalentSpecs)
        if (info.ClassId == classId && key == info.Key)
            return info.Spec;
    return TalentSpec::kNone;
}

} // namespace forever_module
