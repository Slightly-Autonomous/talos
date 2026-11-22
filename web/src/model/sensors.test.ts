import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, test } from "vitest";
import { BUILT_IN_BOARDS, deviceSensors, sensorsLabel } from "./view";

describe("device sensors (ADR 0019)", () => {
  test("TL-SN-002 lists its built-in sensors, never slots", () => {
    const s = deviceSensors({ boardId: "sensor_node_v2", slotModules: [] })!;
    expect(s.kind).toBe("built-in");
    expect(s.items.map((i) => i.module)).toEqual([
      "tmag5273a2_magnetic",
      "vibration_lis2dw12_v1",
      "pir223d_motion",
      "environment_sht40_v1",
    ]);
    expect(sensorsLabel(s)).toBe(
      "built-in: door contact (magnetic), vibration, motion (PIR), temperature / humidity",
    );
    expect(sensorsLabel(s)).not.toContain("slot");
  });

  test("modular boards list occupied slots; devices without any list nothing", () => {
    const s = deviceSensors({
      boardId: "universal_controller_v1",
      slotModules: ["pir_ekmb_v1", "", "smart_camera_s3_v1"],
    })!;
    expect(s.kind).toBe("slots");
    expect(sensorsLabel(s)).toBe("slots: 0 motion (PIR), 2 camera");
    expect(deviceSensors({ boardId: undefined, slotModules: [] })).toBeUndefined();
  });

  test("the built-in table matches the board manifest", () => {
    const yaml = readFileSync(
      resolve(process.cwd(), "../contracts/manifests/boards/sensor_node_v2.yaml"),
      "utf8",
    );
    const fromManifest = [...yaml.matchAll(/built_in_module:\s*([a-z0-9_]+)/g)].map((m) => m[1]);
    expect(fromManifest).toEqual([...BUILT_IN_BOARDS.sensor_node_v2!]);
  });
});
