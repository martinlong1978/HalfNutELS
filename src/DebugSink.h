#ifndef ELS_DEBUGSINK_H
#define ELS_DEBUGSINK_H

#include <leadscrew.h>

// Uploads a finished motion-trace capture (lib/global_state/debugcapture.h) to
// the URL configured as `debugUrl` in WebSettings.
//
// CALL THIS FROM THE DISPLAY TASK, once per pass (src/main.cpp, displayLoop()).
// It is a poll, not a trigger: the capture stops itself when the buffer is
// full, and this is what notices - and then waits, for as long as it takes,
// until the machine is safe to interrupt.
//
// WHY IT WAITS. A network upload is not free on this board. WiFi's driver tasks
// live on core 0 alongside the SpindleTask, which runs a tight loop at priority
// 24 and never blocks, so the radio gets no CPU at all unless that loop is
// stopped - the same problem the OTA path solves by suspending motion outright
// (main.cpp's timerCallback swaps to commsManager.loop()). Uploading mid-cut
// would therefore either not work or stall step generation, which is precisely
// what saveLathePreferences() refuses flash writes under power to avoid.
//
// So the upload runs only when BOTH are true:
//   * the motion mode is MM_DISABLED / MM_UNSET (nothing is commanded), and
//   * the leadscrew's planner speed is a hard zero (the deceleration ramp has
//     finished - "commanded to stop" is not the same as "stopped").
// The operator does not have to do anything except stop the carriage; the
// upload then starts on its own, and the Diagnostics screen's title row says
// so at every step (armed / full / sending / sent).
//
// `leadscrew` supplies the at-rest test and may be null (it is, in AP config
// mode, where nothing is ever captured); a null leadscrew simply means never
// upload.
void debugCapturePoll(Leadscrew* leadscrew);

#endif  // ELS_DEBUGSINK_H
