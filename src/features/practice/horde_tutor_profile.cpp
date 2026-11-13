#include "features/practice/horde_tutor_profile.h"

#include <stdexcept>

namespace bf::runtime {
namespace {
// Historical extracted HordeModeTutor/rel_prologue_01_WaveTest.lua, active
// lines20..48, and ConsoleMacro.ini. Current PE checks are separate evidence;
// this is not a fresh resource extraction or a host-provided spawn plan.
constexpr std::array kTutorSpawns{
    HordeTutorSpawn{1U, 0U, u".BigHpkephiPirate", u"BigHpKephiPirate", 1U,
                    u"move33", false},
    HordeTutorSpawn{2U, 0U, u".BigHpkephiPirate2", u"BigHpKephiPirate", 2U,
                    u"move32", false},
    HordeTutorSpawn{3U, 500U, u".retreatDaoPirate6", u"RetreatDaoPirate", 6U,
                    u"move0", false},
    HordeTutorSpawn{4U, 5000U, u".retreatKephiPirate", u"RetreatKephiPirate",
                    1U, u"move9", false},
    HordeTutorSpawn{5U, 6000U, u".retreatDaoPirate", u"RetreatDaoPirate", 1U,
                    u"move12", false},
    HordeTutorSpawn{6U, 3000U, u".daoPirate5", u"DaoPirate", 5U,
                    u"emotion_Dao3", false},
    HordeTutorSpawn{7U, 3500U, u".daoPirate4", u"DaoPirate", 4U,
                    u"emotion_Dao5", false},
    HordeTutorSpawn{8U, 4000U, u".daoPirate11", u"DaoPirate", 11U,
                    u"emotion_Dao6", false},
    HordeTutorSpawn{9U, 5500U, u".daoPirate12", u"DaoPirate", 12U,
                    u"emotion_Dao7", false},
    HordeTutorSpawn{10U, 6500U, u".daoPirate13", u"DaoPirate", 13U,
                    u"emotion_Dao8", false},
    HordeTutorSpawn{11U, 0U, u".rifleuniPirate5", u"RifleUniPirate", 5U,
                    u"emotion_Uni3", false},
    HordeTutorSpawn{12U, 6500U, u".kephiPirate2", u"KephiPirate", 2U,
                    u"emotion_Kephi0", false},
    HordeTutorSpawn{13U, 7000U, u".kephiPirate3", u"KephiPirate", 3U,
                    u"emotion_Kephi1", false},
    HordeTutorSpawn{14U, 7500U, u".kephiPirate4", u"KephiPirate", 4U,
                    u"emotion_Kephi2", false},
    HordeTutorSpawn{15U, 8000U, u".kephiPirate5", u"KephiPirate", 5U,
                    u"emotion_Kephi3", false},
    HordeTutorSpawn{16U, 1000U, u".RifleUniNPC", u"RifleUniNPC", 1U,
                    u"emotion_UniNPC0", true},
    HordeTutorSpawn{17U, 1500U, u".DizniNPC", u"DizniNPC", 1U,
                    u"emotion_DizniNPC0", true},
    HordeTutorSpawn{18U, 2000U, u".BazziNPC", u"BazziNPC", 1U,
                    u"emotion_BazziNPC0", true},
    HordeTutorSpawn{19U, 2500U, u".KephiNPC", u"KephiNPC", 1U,
                    u"emotion_KephiNPC0", true},
};
}  // namespace

std::span<const HordeTutorSpawn> HordeTutorSpawns() noexcept {
  return kTutorSpawns;
}

const std::array<float, 3U>& HordeTutorSpawnPosition(std::uint8_t group) {
  // DCD7E1 reads48-byte WP records at file128; DCD818 copies raw+8xyz
  // to point+4. DCFA10 and DBD715 use that FIRST point, not WP header+12.
  static constexpr std::array<std::array<float, 3U>, 19U> kPositions{{
      {1616.588989F, 265.988068F, 1327.147095F},
      {1618.071777F, 265.796021F, 1171.029053F},
      {1581.287964F, 265.793365F, 1205.455566F},
      {1598.167114F, 266.006439F, 1337.098633F},
      {1602.454956F, 265.796478F, 1191.051514F},
      {1758.974365F, 266.040283F, 1341.794434F},
      {1882.924561F, 265.862427F, 1494.717041F},
      {1905.939087F, 265.862427F, 1519.244141F},
      {2072.468994F, 273.691437F, 1471.044556F},
      {2088.033447F, 274.899353F, 1489.129395F},
      {1982.953491F, 265.906616F, 1397.451050F},
      {1783.713013F, 265.907776F, 1268.387329F},
      {1880.489624F, 265.828857F, 1540.999878F},
      {2013.412476F, 265.951141F, 1368.557617F},
      {2023.363037F, 265.869781F, 1512.918945F},
      {2159.256836F, 292.011902F, 1447.705444F},
      {2158.615234F, 292.011902F, 1423.761108F},
      {2178.807617F, 292.011902F, 1451.018921F},
      {2181.027344F, 292.011902F, 1424.226196F},
  }};
  if (group == 0U || group > kPositions.size())
    throw std::invalid_argument{"invalid HordeTutor spawn group"};
  return kPositions[group - 1U];
}
}  // namespace bf::runtime
