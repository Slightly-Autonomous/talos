// Frigate NVR connector (ADR 0015, ADR 0020). For each mapped camera it is the
// one producer of that camera's "<cam>_video" device:
// - detections: Frigate events become camera.object_detected observations
//   with a media reference (Frigate event id, clip and snapshot flags);
// - health: a device.heartbeat while Frigate reports frames from the camera,
//   so a broken RTSP feed or a dead Frigate reads as offline, never as fine;
// - bookmarks: camera.bookmark commands become Frigate manual events, ACKed
//   when created; a camera.clip_recorded observation with the command id
//   follows once Frigate has the clip, and only that verifies the command.
// Video stays in Frigate; the hub serves media through its own checks.
//
// Field names follow Frigate 0.17's HTTP API (/api/events, /api/stats,
// /api/events/<camera>/<label>/create). Use Frigate's internal,
// unauthenticated port (5000) on a private network only.

export interface CameraMapping {
  deviceId: string; // the "<cam>_video" device
  token: string; // that device's hub token
  slot?: number;
  rtspUrl?: string; // for gen-config only
}

export interface FrigateOptions {
  hubUrl: string;
  frigateUrl: string;
  propertyId: string;
  cameras: Record<string, CameraMapping>; // Frigate camera name -> Talos device
  fetch?: typeof fetch;
  now?: () => number; // unix ms
  heartbeatMs?: number;
}

interface FrigateEvent {
  id: string;
  camera: string;
  label: string;
  start_time: number;
  end_time?: number | null;
  has_clip?: boolean;
  has_snapshot?: boolean;
  top_score?: number | null;
  data?: { top_score?: number; score?: number };
}

interface HubCommand {
  id: string;
  operation: string;
  args?: { bookmark?: { durationS?: number; label?: string } };
}

interface PendingClip {
  commandId: string;
  eventId: string;
  camera: string;
  giveUpAt: number;
}

export const MEDIA_ID = /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,127}$/;

function mediaRef(e: FrigateEvent) {
  return {
    store: "frigate",
    mediaId: e.id,
    startUnixMs: String(Math.round(e.start_time * 1000)),
    endUnixMs: String(e.end_time ? Math.round(e.end_time * 1000) : 0),
    hasClip: Boolean(e.has_clip),
    hasSnapshot: Boolean(e.has_snapshot),
  };
}

export class FrigateConnector {
  private readonly fetchImpl: typeof fetch;
  private readonly now: () => number;
  private readonly seen = new Set<string>();
  private readonly seq = new Map<string, number>();
  private readonly bootEpoch: number;
  private readonly lastBeat = new Map<string, number>();
  private readonly handled = new Set<string>();
  private readonly ownEvents = new Set<string>(); // Frigate events our bookmarks created
  private pending: PendingClip[] = [];
  private after = 0;

  constructor(private readonly o: FrigateOptions) {
    this.fetchImpl = o.fetch ?? globalThis.fetch.bind(globalThis);
    this.now = o.now ?? Date.now;
    this.bootEpoch = Math.floor(this.now() / 1000);
  }

  private async post(cam: CameraMapping, body: Record<string, unknown>): Promise<boolean> {
    const seq = (this.seq.get(cam.deviceId) ?? 0) + 1;
    this.seq.set(cam.deviceId, seq);
    const r = await this.fetchImpl(new URL("/v1/devices/observations", this.o.hubUrl), {
      method: "POST",
      headers: { Authorization: `Bearer ${cam.token}`, "Content-Type": "application/json" },
      body: JSON.stringify({
        schemaVersion: 1,
        propertyId: this.o.propertyId,
        position: { bootEpoch: this.bootEpoch, sequence: String(seq) },
        ...body,
      }),
    });
    return r.ok;
  }

  private source(cam: CameraMapping, capabilityId: string) {
    const s: Record<string, unknown> = { deviceId: cam.deviceId, capabilityId };
    if (cam.slot !== undefined && capabilityId === "camera.detection") s.slot = cam.slot;
    return s;
  }

  private async frigate(path: string, init?: RequestInit): Promise<Response> {
    return this.fetchImpl(`${this.o.frigateUrl}${path}`, init);
  }

  // Detections since the last poll; returns how many were posted.
  async pollOnce(): Promise<number> {
    const res = await this.frigate(`/api/events?after=${this.after}&limit=50`);
    if (!res.ok) throw new Error(`frigate ${res.status}`);
    const events = (await res.json()) as FrigateEvent[];
    let posted = 0;
    for (const e of [...events].sort((a, b) => a.start_time - b.start_time)) {
      this.after = Math.max(this.after, Math.floor(e.start_time));
      if (this.seen.has(e.id)) continue;
      const cam = this.o.cameras[e.camera];
      if (!cam) continue; // unmapped camera: ignore
      this.seen.add(e.id);
      if (this.ownEvents.has(e.id)) continue; // our own bookmarks: reported as clips
      const score = e.top_score ?? e.data?.top_score ?? e.data?.score ?? 0;
      const objectDetected: Record<string, unknown> = {
        label: e.label,
        confidencePermille: Math.round(Math.min(1, Math.max(0, score)) * 1000),
      };
      // An id the hub would refuse is reported without media, never dropped.
      if (MEDIA_ID.test(e.id)) objectDetected.media = mediaRef(e);
      const ok = await this.post(cam, {
        source: this.source(cam, "camera.detection"),
        eventType: "camera.object_detected",
        eventId: `${cam.deviceId}:frigate:${e.id}`,
        observedAtUnixMs: String(Math.round(e.start_time * 1000)),
        payload: { objectDetected },
      });
      if (ok) posted++;
    }
    return posted;
  }

  // A heartbeat per camera while Frigate reports frames from it.
  async heartbeatOnce(): Promise<number> {
    const every = this.o.heartbeatMs ?? 60_000;
    const res = await this.frigate("/api/stats");
    if (!res.ok) return 0;
    const stats = (await res.json()) as { cameras?: Record<string, { camera_fps?: number }> };
    let sent = 0;
    for (const [name, cam] of Object.entries(this.o.cameras)) {
      const fps = stats.cameras?.[name]?.camera_fps ?? 0;
      if (fps <= 0) continue; // no frames: let the hub see silence
      const last = this.lastBeat.get(name);
      if (last !== undefined && this.now() - last < every) continue;
      const seq = (this.seq.get(cam.deviceId) ?? 0) + 1;
      if (
        await this.post(cam, {
          source: this.source(cam, "device.health"),
          eventType: "device.heartbeat",
          eventId: `${cam.deviceId}:hb:${this.bootEpoch}:${seq}`,
          payload: { heartbeat: { batteryMv: 0 } },
        })
      ) {
        this.lastBeat.set(name, this.now());
        sent++;
      }
    }
    return sent;
  }

  private async ack(cam: CameraMapping, commandId: string, accepted: boolean, detail: string) {
    await this.fetchImpl(new URL("/v1/devices/acks", this.o.hubUrl), {
      method: "POST",
      headers: { Authorization: `Bearer ${cam.token}`, "Content-Type": "application/json" },
      body: JSON.stringify({ commandId, accepted, detail: detail.slice(0, 256) }),
    });
  }

  // Executes camera.bookmark commands for every mapped camera.
  async commandsOnce(): Promise<number> {
    let handled = 0;
    for (const [name, cam] of Object.entries(this.o.cameras)) {
      const res = await this.fetchImpl(new URL("/v1/devices/commands", this.o.hubUrl), {
        headers: { Authorization: `Bearer ${cam.token}` },
      });
      if (!res.ok) continue;
      const { commands = [] } = (await res.json()) as { commands?: HubCommand[] };
      for (const c of commands) {
        if (this.handled.has(c.id)) continue; // device-side idempotency
        this.handled.add(c.id);
        handled++;
        if (c.operation !== "camera.bookmark" || !c.args?.bookmark) {
          await this.ack(cam, c.id, false, "unsupported command");
          continue;
        }
        const duration = c.args.bookmark.durationS ?? 30;
        try {
          const r = await this.frigate(
            `/api/events/${encodeURIComponent(name)}/${encodeURIComponent(c.args.bookmark.label ?? "talos")}/create`,
            {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({
                duration,
                include_recording: true,
                sub_label: c.id.slice(0, 100),
              }),
            },
          );
          const body = (await r.json().catch(() => ({}))) as {
            success?: boolean;
            event_id?: string;
          };
          if (!r.ok || !body.event_id || !MEDIA_ID.test(body.event_id)) {
            await this.ack(cam, c.id, false, `frigate ${r.status}`);
            continue;
          }
          this.ownEvents.add(body.event_id);
          await this.ack(cam, c.id, true, `frigate event ${body.event_id}`);
          this.pending.push({
            commandId: c.id,
            eventId: body.event_id,
            camera: name,
            giveUpAt: this.now() + (duration + 120) * 1000,
          });
        } catch (e) {
          await this.ack(cam, c.id, false, `frigate unreachable: ${String(e)}`);
        }
      }
    }
    return handled;
  }

  // Reports clips for bookmarks once Frigate has them.
  async clipsOnce(): Promise<number> {
    let reported = 0;
    const keep: PendingClip[] = [];
    for (const p of this.pending) {
      const cam = this.o.cameras[p.camera];
      if (!cam) continue;
      let e: FrigateEvent | undefined;
      try {
        const r = await this.frigate(`/api/events/${p.eventId}`);
        if (r.ok) e = (await r.json()) as FrigateEvent;
      } catch {
        // Frigate busy or restarting: try again next round.
      }
      if (e && e.end_time && e.has_clip) {
        const ok = await this.post(cam, {
          source: this.source(cam, "camera.recording"),
          eventType: "camera.clip_recorded",
          eventId: `${cam.deviceId}:clip:${p.eventId}`,
          payload: { clipRecorded: { commandId: p.commandId, media: mediaRef(e) } },
        });
        if (ok) {
          reported++;
          continue;
        }
      }
      // No clip in time: stop asking; the hub's window closes the command
      // as unknown_result or completed_unverified, never verified.
      if (this.now() < p.giveUpAt) keep.push(p);
    }
    this.pending = keep;
    return reported;
  }

  // One round of every duty; one failing (Frigate down) never skips the
  // others, so bookmarks are still ACKed as rejected.
  async tick(): Promise<string[]> {
    const errors: string[] = [];
    for (const step of [
      () => this.pollOnce(),
      () => this.heartbeatOnce(),
      () => this.commandsOnce(),
      () => this.clipsOnce(),
    ]) {
      await step().catch((e: unknown) => errors.push(String(e)));
    }
    return errors;
  }

  get pendingClips(): number {
    return this.pending.length;
  }
}
