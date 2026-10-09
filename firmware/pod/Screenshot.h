#pragma once

// Screen capture over the USB serial port, for checking layouts without a camera.
// Send "shot" + newline to the pod; it answers with
//   SHOT_BEGIN <width> <height>
//   SHOT:<base64>   (repeated; run-length pairs of [uint16 count, uint16 RGB565 colour], little-endian)
//   SHOT_END
// tools/pod_screenshot.py turns that into a PNG.
// Also: "msg <line>" shows a line as if the phone sent it (phone ignored until "test off").
void Screenshot_Poll(void);

// True while a screenshot is being sent: other code should not print to the serial port then
extern volatile bool screenshot_sending;

// "perf" toggles a report of screen redraw times every 2 s
extern bool perf_report;
