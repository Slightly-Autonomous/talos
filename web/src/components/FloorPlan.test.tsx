import { create } from "@bufbuild/protobuf";
import { render, screen } from "@testing-library/react";
import { Freshness, StateResponseSchema } from "@talos/contracts/talos/api/v1/api_pb";
import { PropertySchema } from "@talos/contracts/talos/v1/property_pb";
import { FloorPlan } from "./FloorPlan";

const property = create(PropertySchema, {
  id: "house-01",
  zones: [
    {
      id: "hall",
      polygon: [
        { x: 0, y: 0 },
        { x: 4, y: 4 },
      ],
    },
  ],
  devices: [{ id: "node-12", zoneId: "hall" }],
  assets: [
    {
      id: "front_door",
      zoneId: "hall",
      segment: { a: { x: 0, y: 0 }, b: { x: 1, y: 0 } },
      attached: [{ deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" }],
    },
  ],
});

const contact = (freshness: Freshness) =>
  create(StateResponseSchema, {
    capabilities: [
      {
        ref: { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" },
        value: { kind: { case: "contact", value: { open: false } } },
        freshness,
      },
    ],
  });

test("a stale closed door is drawn unknown with its last value, never closed", () => {
  render(<FloorPlan property={property} state={contact(Freshness.STALE)} nowMs={0} />);
  const g = screen.getByTestId("portal-front_door");
  expect(g.getAttribute("data-open")).toBe("unknown");
  expect(g.getAttribute("data-last-open")).toBe("false");
  const line = g.querySelector("line")!;
  expect(line.getAttribute("class")).toContain("portal-unknown");
  expect(line.getAttribute("class")).not.toContain("portal-closed");
  expect(line.textContent).toBe("front_door: unknown (last: closed)");
});

test("a fresh closed door is drawn closed", () => {
  render(<FloorPlan property={property} state={contact(Freshness.FRESH)} nowMs={0} />);
  const g = screen.getByTestId("portal-front_door");
  expect(g.getAttribute("data-open")).toBe("false");
  expect(g.querySelector("line")!.getAttribute("class")).toContain("portal-closed");
});
