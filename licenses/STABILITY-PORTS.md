# Stability ports, October 2026

These are focused adaptations, not merges of either reference renderer.

* AwesomeObserver/bloodborne_pc, branch `windows`: deferred recording ownership,
  bounded crash diagnostics, independent NGX model hints and executable cache.
  https://github.com/AwesomeObserver/bloodborne_pc/tree/windows
* Gerasimos Kanellatos (GerasimosKan), public PRs #1 and #2: native Quit Game,
  local trophies and asset-tool dependency setup. Adapted without the unrelated
  launcher redesign. Original copyright and GPL notices are retained.
  https://github.com/DarkIzuku/bloodborne_pc/pull/1
  https://github.com/DarkIzuku/bloodborne_pc/pull/2

* Awakenchan/BloodBorneButPc, `bf594531a6de33ef607fe9661073c09e3a7fa9ff`,
  `engine/bbhost-changes.patch`: game render scratch pools, tiled image writeback,
  collector lifetime observations, memory notification/remapping and allocation checks.
  https://github.com/Awakenchan/BloodBorneButPc
* Supermedo/bloodborne_pc, `21f8497082e73a2c3a21c87c69e08a236fb8f217`;
  null vertex-table correction `2c4ddeba16568d460a20fef63dd32b86e706905c`.
  https://github.com/Supermedo/bloodborne_pc
* Red-zone store discovery/table and trampoline approach: d4rksp4rt4n's
  Supermedo pull request #2 and the subsequent reference tests above.
  https://github.com/Supermedo/bloodborne_pc/pull/2

Existing shadPS4 code retains its GPL-2.0-or-later notices. New adaptations are
GPL-3.0-or-later, compatible with that later-version grant; see GPL-3.0.txt.
The byte-checked patch table contains isolated instruction sites, not a game
executable. No original game files or saves are distributed.

* Gerasimos Kanellatos (GerasimosKan), public PRs #3, #4 and #5, October 22:
  trophies/dependency parity checked, device-aware procedural prompts, Windows
  demand commit, mod patch selection and atomic settings. Adapted rather than
  merging unrelated histories; see docs/PR_CONTRIBUTIONS_2026_10_22.md.
  https://github.com/DarkIzuku/bloodborne_pc/pull/3
  https://github.com/DarkIzuku/bloodborne_pc/pull/4
  https://github.com/DarkIzuku/bloodborne_pc/pull/5
