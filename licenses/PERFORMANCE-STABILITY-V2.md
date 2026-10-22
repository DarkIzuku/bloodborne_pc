# Focused renderer adaptations

* visy/bloodborne_pc_windows_port (GPL-2.0-or-later renderer code):
  https://github.com/visy/bloodborne_pc_windows_port/commit/b8db8c029f73e05364d27be30b33d9c49dbc3996
  https://github.com/visy/bloodborne_pc_windows_port/commit/cf4c369cf2e9ba34517e0b3be2f8f764576076ed
  https://github.com/visy/bloodborne_pc_windows_port/commit/6685a2e1c6b945c32e0d7ec29807bc0448ce81dd
  These identify original shadPS4 coroutine and depth-ordering work and the staging
  read discovered through Mrsuss60's port. Existing copyright notices are retained.
* Supermedo/bloodborne_pc texture-pressure fix, adapted to our collector rather
  than importing its memory arena, counters or thresholds:
  https://github.com/Supermedo/bloodborne_pc/commit/f91543a22ab9294f5c8b94c78f06eb22d63731aa

New collector-policy adaptations retain this project's GPL-3.0-or-later notice.
No SDK, game assets or derived game files are included by these changes.
