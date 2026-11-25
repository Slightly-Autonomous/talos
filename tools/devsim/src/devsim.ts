// Virtual devices for the dev property (house-01). Each one is a real client
// of the hub's device API with its own device token: it posts observations,
// fetches its commands, executes them against a tiny local world model and
// ACKs. Nothing bypasses hub ingress, rules or policy.

export type Fault = "none" | "silent_ack" | "no_response" | "jam";
// "sn002": a TL-SN-002 self-contained node (ADR 0019) reporting its built-in door
// contact (slot 0) and enclosure loop on 2 x AA lithium cells, with a USER button.
// "vision": a TL-VN-001 Vision Node agent (ADR 0020): stream status, lid, heartbeats.
// "recorder": Frigate's view of that camera: detections with media ids, bookmarks and clips.
export type Kind = "pir" | "contact" | "speaker" | "lock" | "sn002" | "vision" | "recorder";

export interface DeviceDef {
  id: string;
  token: string;
  kind: Kind;
  slot?: number;
  // The node has an enclosure tamper loop (capability tamper.enclosure, no slot).
  tamper?: boolean;
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
  // Device clock, for the command dispatch deadline. Defaults to Date.now.
  nowMs?: () => number;
  // A bookmark's clip is reported after this long (a real one waits for the bookmark to end).
  clipDelayMs?: number;
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
  // Dispatch deadline (int64 as a JSON string). A device never acts after it.
  expiresAtUnixMs?: string | number;
  operation?: string;
  args?: {
    audioSpeak?: { text: string };
    setLocked?: { locked?: boolean };
    bookmark?: { durationS?: number };
  };
}

const CAPABILITY: Record<Kind, string> = {
  pir: "motion.detect",
  contact: "contact.open_state",
  speaker: "audio.speak",
  lock: "access.lock",
  sn002: "contact.open_state",
  vision: "camera.stream",
  recorder: "camera.detection",
};

// A tiny placeholder snapshot (160x90 JPEG) the fake recorder serves for every media id.
export const SNAPSHOT_JPEG = Buffer.from(
  "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",
  "base64",
);

// Battery a heartbeat reports: TL-SN-002's AA pair under load (fresh ~3.0 V), else 1S Li-ion.
const HEARTBEAT_MV: Partial<Record<Kind, number>> = { sn002: 3000 };

const isContact = (k: Kind) => k === "contact" || k === "sn002";

export class DevSim {
  private readonly devices = new Map<string, Device>();
  private readonly fetchImpl: typeof fetch;
  private timers: ReturnType<typeof setInterval>[] = [];
  private holdTimers = new Map<string, ReturnType<typeof setTimeout>>();
  readonly log: string[] = [];
  // Media ids the fake recorder has reported (served by the control server, ADR 0020).
  readonly media = new Set<string>();
  private mediaSeq = 0;

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
    const base = this.opts.hubUrl.endsWith("/") ? this.opts.hubUrl : `${this.opts.hubUrl}/`;
    const res = await this.fetchImpl(new URL(path.replace(/^\/+/, ""), base), {
      method: body === undefined ? "GET" : "POST",
      headers: { Authorization: `Bearer ${d.token}`, "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await res.text();
    if (!res.ok) throw new Error(`${path} ${res.status} ${text.slice(0, 200)}`);
    return text ? (JSON.parse(text) as unknown) : {};
  }

  private async observe(
    d: Device,
    eventType: string,
    payload: unknown,
    capability = CAPABILITY[d.kind],
  ) {
    d.seq += 1;
    const source: Record<string, unknown> = { deviceId: d.id, capabilityId: capability };
    const deviceLevel = capability === "device.health" || capability === "tamper.enclosure";
    if (d.slot !== undefined && !deviceLevel) source.slot = d.slot;
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
    const d = this.dev(id);
    await this.observe(
      d,
      "device.heartbeat",
      { heartbeat: { batteryMv: HEARTBEAT_MV[d.kind] ?? 3900 } },
      "device.health",
    );
  }

  // TL-SN-002 USER button, short press: the test report is a heartbeat now (the
  // firmware's kUserButton diagnostic itself only travels over CoAP).
  async userButton(id: string) {
    const d = this.dev(id);
    if (d.kind !== "sn002") throw new Error(`${id} has no USER button`);
    this.log.push(`${id} USER pressed`);
    await this.heartbeat(id);
  }

  async boot() {
    for (const d of this.devices.values()) {
      if (isContact(d.kind)) {
        // Boot state report, not a transition: rules must not fire on it.
        d.state.open = false;
        await this.observe(d, "contact.state", { contact: { open: false } });
      }
      if (d.tamper) {
        // Boot state report (rules don't fire on it), as the firmware sends after a cold boot.
        d.state.lidOpen = false;
        await this.observe(d, "tamper.state", { tamper: { open: false } }, "tamper.enclosure");
      }
      if (d.kind === "lock") {
        d.state.locked = false;
        await this.observe(d, "lock.state", { lock: { locked: false } });
      }
      if (d.kind === "vision") {
        d.state.streaming = true;
        await this.streamReport(d);
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
    if (!isContact(d.kind)) throw new Error(`${id} is not a contact`);
    d.state.open = open;
    await this.observe(d, open ? "contact.opened" : "contact.closed", { contact: { open } });
  }

  // The node's enclosure lid opened (or the loop was cut) / closed again.
  async tamper(id: string, open: boolean) {
    const d = this.dev(id);
    if (!d.tamper) throw new Error(`${id} has no tamper loop`);
    d.state.lidOpen = open;
    await this.observe(
      d,
      open ? "tamper.opened" : "tamper.closed",
      { tamper: { open } },
      "tamper.enclosure",
    );
  }

  private async streamReport(d: Device) {
    const up = d.state.streaming === true;
    await this.observe(d, "camera.stream_status", {
      streamStatus: up
        ? { up, width: 1920, height: 1080, fpsMilli: 15000, bitrateKbps: 4000, socMilliC: 51000 }
        : { up, socMilliC: 51000 },
    });
  }

  // The Vision Node's camera stops or starts producing video.
  async stream(id: string, up: boolean) {
    const d = this.dev(id);
    if (d.kind !== "vision") throw new Error(`${id} is not a Vision Node`);
    d.state.streaming = up;
    await this.streamReport(d);
  }

  // Frigate detected something on this camera: a detection with a media id the hub can serve.
  async detect(id: string, label = "person") {
    const d = this.dev(id);
    if (d.kind !== "recorder") throw new Error(`${id} is not a recorder view`);
    const mediaId = `devsim-${Date.now()}-${++this.mediaSeq}`;
    this.media.add(mediaId);
    await this.observe(d, "camera.object_detected", {
      objectDetected: {
        label,
        confidencePermille: 880,
        media: {
          store: "frigate",
          mediaId,
          startUnixMs: String(Date.now()),
          hasClip: false,
          hasSnapshot: true,
        },
      },
    });
    return mediaId;
  }

  setFault(id: string, fault: Fault) {
    this.dev(id).fault = fault;
  }

  // Fetch and execute pending commands for every actuator once.
  async pollOnce() {
    for (const d of this.devices.values()) {
      if (d.kind !== "speaker" && d.kind !== "lock" && d.kind !== "recorder") continue;
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
    // TTL gates physical effect: past the deadline the device refuses (ADR 0004).
    const expires = Number(cmd.expiresAtUnixMs ?? 0);
    if (expires > 0 && (this.opts.nowMs ?? Date.now)() >= expires) {
      this.log.push(`${d.id} refused expired ${cmd.id}`);
      await ack(false, "expired");
      return;
    }
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
    if (d.kind === "recorder" && cmd.operation === "camera.bookmark") {
      await ack(true, "frigate event created");
      await delay(this.opts.clipDelayMs ?? 2000);
      const mediaId = `devsim-bm-${++this.mediaSeq}`;
      this.media.add(mediaId);
      await this.observe(
        d,
        "camera.clip_recorded",
        {
          clipRecorded: {
            commandId: cmd.id,
            media: { store: "frigate", mediaId, hasClip: true, hasSnapshot: false },
          },
        },
        "camera.recording",
      );
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
        for (const d of this.devices.values()) {
          void this.heartbeat(d.id).catch(() => {});
          if (d.kind === "vision") void this.streamReport(d).catch(() => {});
        }
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
  // TL-SN-002 (ADR 0019): built-in magnetic contact in slot 0, lid + wall loop, AA cells.
  { id: "node-12", token: "dev-node-12-token", kind: "sn002", slot: 0, tamper: true },
  { id: "patio_speaker", token: "dev-patio-speaker-token", kind: "speaker" },
  { id: "lock-3", token: "dev-lock-3-token", kind: "lock" },
  // TL-VN-001 Vision Node (ADR 0020) and Frigate's view of its camera.
  { id: "cam_patio", token: "dev-cam-patio-token", kind: "vision", tamper: true },
  { id: "cam_patio_video", token: "dev-cam-patio-video-token", kind: "recorder" },
];
