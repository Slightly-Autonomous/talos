// Virtual devices for the dev property (house-01). Each one is a real client
// of the hub's device API with its own device token: it posts observations,
// fetches its commands, executes them against a tiny local world model and
// ACKs. Nothing bypasses hub ingress, rules or policy.

export type Fault = "none" | "silent_ack" | "no_response" | "jam";
export type Kind = "pir" | "contact" | "speaker" | "lock";

export interface DeviceDef {
  id: string;
  token: string;
  kind: Kind;
  slot?: number;
}

export interface DevSimOptions {
  hubUrl: string;
  devices: DeviceDef[];
  fetch?: typeof fetch;
  holdMs?: number; // PIR hold before "cleared"
  speakerLatencyMs?: number;
  lockTravelMs?: number;
  heartbeatMs?: number;
  pollMs?: number;
  // The contact device whose door the lock bolts.
  doorContactDevice?: string;
}

interface Device extends DeviceDef {
  bootEpoch: number;
  seq: number;
  fault: Fault;
  executed: Set<string>;
  state: Record<string, unknown>;
}

interface Command {
  id: string;
  args?: { audioSpeak?: { text: string }; setLocked?: { locked?: boolean } };
}

const CAPABILITY: Record<Kind, string> = {
  pir: "motion.detect",
  contact: "contact.open_state",
  speaker: "audio.speak",
  lock: "access.lock",
};

export class DevSim {
  private readonly devices = new Map<string, Device>();
  private readonly fetchImpl: typeof fetch;
  private timers: ReturnType<typeof setInterval>[] = [];
  private holdTimers = new Map<string, ReturnType<typeof setTimeout>>();
  readonly log: string[] = [];

  constructor(private readonly opts: DevSimOptions) {
    this.fetchImpl = opts.fetch ?? globalThis.fetch.bind(globalThis);
    // Seconds since epoch: monotonic across simulator restarts, so the hub
    // never mistakes a restarted device's sequence numbers for duplicates.
    const boot = Math.floor(Date.now() / 1000);
    for (const d of opts.devices) {
      this.devices.set(d.id, {
        ...d,
        bootEpoch: boot,
        seq: 0,
        fault: "none",
        executed: new Set(),
        state: {},
      });
    }
  }

  private dev(id: string): Device {
    const d = this.devices.get(id);
    if (!d) throw new Error(`unknown device ${id}`);
    return d;
  }

  private async hub(d: Device, path: string, body?: unknown): Promise<unknown> {
    const res = await this.fetchImpl(new URL(path, this.opts.hubUrl), {
      method: body === undefined ? "GET" : "POST",
      headers: { Authorization: `Bearer ${d.token}`, "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const json: unknown = await res.json();
    if (!res.ok) throw new Error(`${path} ${res.status} ${JSON.stringify(json)}`);
    return json;
  }

  private async observe(
    d: Device,
    eventType: string,
    payload: unknown,
    capability = CAPABILITY[d.kind],
  ) {
    d.seq += 1;
    const source: Record<string, unknown> = { deviceId: d.id, capabilityId: capability };
    if (d.slot !== undefined && capability !== "device.health") source.slot = d.slot;
    await this.hub(d, "/v1/devices/observations", {
      schemaVersion: 1,
      propertyId: "house-01",
      source,
      eventType,
      eventId: `${d.id}:${d.bootEpoch}:${d.seq}`,
      position: { bootEpoch: d.bootEpoch, sequence: String(d.seq) },
      observedAtUnixMs: String(Date.now()),
      payload,
    });
    this.log.push(`${d.id} ${eventType}`);
  }

  async heartbeat(id: string) {
    await this.observe(
      this.dev(id),
      "device.heartbeat",
      { heartbeat: { batteryMv: 3900 } },
      "device.health",
    );
  }

  async boot() {
    for (const d of this.devices.values()) {
      if (d.kind === "contact") {
        // Boot state report, not a transition: rules must not fire on it.
        d.state.open = false;
        await this.observe(d, "contact.state", { contact: { open: false } });
      }
      if (d.kind === "lock") {
        d.state.locked = false;
        await this.observe(d, "lock.state", { lock: { locked: false } });
      }
      await this.heartbeat(d.id);
    }
  }

  async motion(id: string) {
    const d = this.dev(id);
    if (d.kind !== "pir") throw new Error(`${id} is not a PIR`);
    const wasActive = d.state.active === true;
    d.state.active = true;
    if (!wasActive) await this.observe(d, "motion.detected", { motion: { detected: true } });
    clearTimeout(this.holdTimers.get(id));
    this.holdTimers.set(
      id,
      setTimeout(() => {
        d.state.active = false;
        void this.observe(d, "motion.cleared", { motion: { detected: false } }).catch(() => {});
      }, this.opts.holdMs ?? 5000),
    );
  }

  async contact(id: string, open: boolean) {
    const d = this.dev(id);
    if (d.kind !== "contact") throw new Error(`${id} is not a contact`);
    d.state.open = open;
    await this.observe(d, open ? "contact.opened" : "contact.closed", { contact: { open } });
  }

  setFault(id: string, fault: Fault) {
    this.dev(id).fault = fault;
  }

  // Fetch and execute pending commands for every actuator once.
  async pollOnce() {
    for (const d of this.devices.values()) {
      if (d.kind !== "speaker" && d.kind !== "lock") continue;
      const pending = (await this.hub(d, "/v1/devices/commands")) as { commands?: Command[] };
      for (const cmd of pending.commands ?? []) await this.execute(d, cmd);
    }
  }

  private async execute(d: Device, cmd: Command) {
    if (d.fault === "no_response") return;
    if (d.executed.has(cmd.id)) return; // device-side idempotency
    d.executed.add(cmd.id);
    const ack = (accepted: boolean, detail: string) =>
      this.hub(d, "/v1/devices/acks", { commandId: cmd.id, accepted, detail });
    const delay = (ms: number) => new Promise((r) => setTimeout(r, ms));

    if (d.kind === "speaker" && cmd.args?.audioSpeak) {
      await delay(this.opts.speakerLatencyMs ?? 300);
      if (d.fault !== "silent_ack") {
        await this.observe(d, "audio.playback_started", { audioPlayback: { playing: true } });
        this.log.push(`${d.id} played "${cmd.args.audioSpeak.text}"`);
      }
      await ack(true, d.fault === "silent_ack" ? "ok" : "playing");
      return;
    }
    if (d.kind === "lock" && cmd.args?.setLocked) {
      const want = cmd.args.setLocked.locked === true;
      await ack(true, "moving");
      await delay(this.opts.lockTravelMs ?? 1000);
      const door = this.opts.doorContactDevice
        ? this.devices.get(this.opts.doorContactDevice)
        : undefined;
      const doorOpen = door?.state.open === true;
      if (d.fault !== "jam" && d.fault !== "silent_ack" && !(want && doorOpen))
        d.state.locked = want;
      await this.observe(d, "lock.state", { lock: { locked: d.state.locked === true } });
      return;
    }
    await ack(false, "unsupported command");
  }

  start() {
    const poll = this.opts.pollMs ?? 250;
    let polling = false;
    this.timers.push(
      setInterval(() => {
        if (polling) return;
        polling = true;
        void this.pollOnce()
          .catch((e) => this.log.push(`poll error: ${String(e)}`))
          .finally(() => (polling = false));
      }, poll),
      setInterval(() => {
        for (const id of this.devices.keys()) void this.heartbeat(id).catch(() => {});
      }, this.opts.heartbeatMs ?? 30_000),
    );
  }

  stop() {
    this.timers.forEach(clearInterval);
    this.holdTimers.forEach(clearTimeout);
  }

  status() {
    return {
      hub: this.opts.hubUrl,
      devices: Object.fromEntries(
        [...this.devices.values()].map((d) => [
          d.id,
          { kind: d.kind, fault: d.fault, state: d.state, seq: d.seq },
        ]),
      ),
      log: this.log.slice(-20),
    };
  }
}

export const DEV_DEVICES: DeviceDef[] = [
  { id: "node-07", token: "dev-node-07-token", kind: "pir", slot: 0 },
  { id: "node-12", token: "dev-node-12-token", kind: "contact", slot: 0 },
  { id: "patio_speaker", token: "dev-patio-speaker-token", kind: "speaker" },
  { id: "lock-3", token: "dev-lock-3-token", kind: "lock" },
];
