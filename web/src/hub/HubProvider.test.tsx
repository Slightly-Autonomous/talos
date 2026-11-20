import { create } from "@bufbuild/protobuf";
import { render, screen, waitFor } from "@testing-library/react";
import { HubError, type HubClient } from "@talos/contracts/client";
import {
  Freshness,
  JournalPageSchema,
  StateResponseSchema,
} from "@talos/contracts/talos/api/v1/api_pb";
import { PropertySchema } from "@talos/contracts/talos/v1/property_pb";
import { HubProvider, useHub } from "./HubProvider";

function Probe() {
  const { status, state, stateStale } = useHub();
  return (
    <div>
      <span data-testid="status">{status}</span>
      <span data-testid="stale">{String(stateStale)}</span>
      <span data-testid="freshness">{String(state?.capabilities[0]?.freshness)}</span>
    </div>
  );
}

test("an idle stream drops 'live', and failed state reads render capabilities unknown", async () => {
  let stateCalls = 0;
  const client = {
    config: () => Promise.reject(new Error("no config api")),
    property: () => Promise.resolve(create(PropertySchema, { id: "house-01" })),
    journal: () => Promise.resolve(create(JournalPageSchema, {})),
    state: () => {
      stateCalls += 1;
      if (stateCalls > 1) return Promise.reject(new HubError(0, "unreachable", "down"));
      return Promise.resolve(
        create(StateResponseSchema, {
          capabilities: [
            {
              ref: { deviceId: "node-12", capabilityId: "contact.open_state" },
              value: { kind: { case: "contact", value: { open: false } } },
              freshness: Freshness.FRESH,
            },
          ],
        }),
      );
    },
    // The stream connects, then times out with no keepalive.
    // eslint-disable-next-line require-yield
    events: async function* () {
      await new Promise((r) => setTimeout(r, 50));
      throw new HubError(0, "stream_idle", "no data from event stream");
    },
  } as unknown as HubClient;

  render(
    <HubProvider client={client}>
      <Probe />
    </HubProvider>,
  );
  await waitFor(() => expect(screen.getByTestId("status").textContent).toBe("live"));
  expect(screen.getByTestId("freshness").textContent).toBe(String(Freshness.FRESH));
  await waitFor(() => expect(screen.getByTestId("status").textContent).toBe("reconnecting"));
  // The retry's state read fails: the hub is unreachable, so the last
  // snapshot is aged to unknown instead of staying "fresh".
  await waitFor(() => expect(screen.getByTestId("stale").textContent).toBe("true"), {
    timeout: 4000,
  });
  expect(screen.getByTestId("freshness").textContent).toBe(String(Freshness.UNKNOWN));
  expect(screen.getByTestId("status").textContent).toBe("reconnecting");
});
