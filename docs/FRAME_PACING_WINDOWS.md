# Windows frame pacing

The launcher offers 30, 60, 90, 120 and Unlimited (both quick controls and Performance).
Settings survive saving and reopening. A different simulation mode takes effect next launch.
The established 30/60/90 FPS++ patch selection is retained.

At startup, the supported 1.09 identity and the original front-end predicate's
instructions are checked. The predicate reads menu flow +0xb11, independently of
inventory, shops and System menus. Title/front-end frames are limited to at most
60 FPS. Loading without a valid world player also retains that ceiling; on an
unknown image/state the safe ceiling remains. Returning to title restores it.
No memory is written to determine menu state, and PC Controls may be disabled.

On Windows, 120 FPS and Unlimited require FIFO (VSync). The launcher selects and
locks this choice in either FPS selector, migrates older saved choices, and saves
FIFO. Switching back to 30/60/90 enables all present choices again. The Windows
preparation script and runtime also enforce FIFO for old configurations, inherited
environment variables and direct launches. FIFO Relaxed is not used for these modes.
Unlimited follows the current monitor's refresh; 120 FPS follows the lower of
120 and that refresh. Refresh is read on the SDL window thread, retaining fractional
rates and updating on display/mode changes. The selected Vulkan present mode is
logged after fallback only when Frame Statistics is enabled. Frame throughput in a log is not a measurement of which
frames Mailbox displayed, and shader compilation/loading spikes remain distinct
from this presentation policy.
The present limiter also covers repeated/blank frames. It retains the scheduled
deadline phase across ordinary Windows wake delays, avoiding accumulated drift
below the selected rate. A delay of half a frame or more starts a new phase with
a full frame period; overdue frames never trigger a burst of catch-up work.
The immediate-flip vblank loop retains this phase as well, so a producer waiting
on guest vblank events does not inherit a lower cadence from repeated late wakes.
Frame queue ownership/backpressure and GPU synchronization
remain unchanged. A continuously nonempty flip queue still returns at every
vblank deadline: condition-variable predicates otherwise bypass an expired
wait_until deadline and can starve guest vblank events in uncapped mode.
Logs report display refresh and effective cap changes.

120 derives Kyo's 90 FPS++ timing values as in Awakenchan's Windows port. New
high-FPS modes also use byte-verified measured physics/AI/ragdoll/grass steps
from bbhost. Unlimited retains this fixed-preset frame-job design with no minimum
frame target, rather than restoring the different legacy Uncap FPS++ patch
reported to deadlock model jobs after Amelia's introduction. Frame-time-consuming
character code uses the existing measured FPS++ dt slot. The newer cloth treatment
preserves native damping/integration. Only these two new modes receive those edits.
User game files and save formats are unchanged; external XML patches still apply
later and can override built-in changes.

## Cinematics

Read-only inspection of `sprj_opening.mp4` in the supported local dump reports
H.264, 1920x1080, 30/1 FPS. Its audio and presentation timestamps are preserved:
this update does not interpolate video or speed it up to 60/120. A 48 FPS overlay
reading does not establish the video's source frame rate. No verified universal
48 FPS cinematic patch was found. In-engine cinematics use the selected gameplay
pace and the checked timing/job corrections above; they require gameplay testing.

## Verification

Automated checks cover 100000 paced frames, repeated late timer wakes at 30, 60,
90, 120 and fractional monitor rates, stalls, transitions, inventory
exclusion, fractional display refresh, invalid state/identity, patch prerequisites,
120/uncapped targets and measured-step addresses, launcher persistence and launch
environment. The new timing instructions also match the actual supported 1.09 ELF.
Complete Windows and existing graphics/mod regressions are run before packaging.
Long gameplay, boss introductions, deaths and monitor switching require testing
in the game; synthetic tests do not establish gameplay stability at every rate.
