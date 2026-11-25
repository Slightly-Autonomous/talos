import { expect, test } from "vitest";
import { genConfig } from "./gen-config.ts";

const cams = {
  patio: {
    deviceId: "cam_patio_video",
    token: "t-secret",
    rtspUrl: "rtsp://talos:{FRIGATE_RTSP_PASSWORD}@cam-patio.local:8554/cam",
  },
};

test("generates recording with pre-roll, detection and the camera, without secrets", () => {
  const y = genConfig(cams);
  expect(y).toContain("  patio:\n");
  expect(y).toContain('- path: "rtsp://talos:{FRIGATE_RTSP_PASSWORD}@cam-patio.local:8554/cam"');
  expect(y).toContain("roles: [detect, record]");
  expect(y).toContain("pre_capture: 5");
  expect(y).toContain("type: cpu");
  expect(y).not.toContain("t-secret");
});

test("a second detect stream, and the Hailo detector", () => {
  const y = genConfig(cams, {
    detector: "hailo8l",
    detectUrls: { patio: "rtsp://talos:{FRIGATE_RTSP_PASSWORD}@cam-patio.local:8554/detect" },
  });
  expect(y).toContain("roles: [record]");
  expect(y).toContain("roles: [detect]");
  expect(y).toContain("width: 640");
  expect(y).toContain("type: hailo8l");
});

test("refuses passwords in the file, bad names and non-RTSP sources", () => {
  expect(() =>
    genConfig({ patio: { ...cams.patio, rtspUrl: "rtsp://talos:hunter2@cam:8554/cam" } }),
  ).toThrow(/FRIGATE_RTSP_PASSWORD/);
  expect(() => genConfig({ "Patio Cam": cams.patio })).toThrow(/camera name/);
  expect(() => genConfig({ patio: { ...cams.patio, rtspUrl: "http://cam/x" } })).toThrow(/rtsp/);
  expect(() => genConfig({})).toThrow(/no cameras/);
});
