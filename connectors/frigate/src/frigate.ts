// Frigate NVR connector (ADR 0015): turns Frigate detection events into
// camera.object_detected observations for mapped Talos camera devices,
// posted through the hub's device API with each camera's own token. Video
// stays in Frigate; Talos only records detection metadata (spec §33).
//
// Field names follow Frigate's /api/events; verify against your Frigate
// version (newer versions move scores under `data`).

export interface CameraMapping {
  deviceId: string;
  token: string;
  slot?: number;
}

export interface FrigateOptions {
  hubUrl: string;
  frigateUrl: string;
  propertyId: string;
  cameras: Record<string, CameraMapping>; // Frigate camera name -> Talos device
  fetch?: typeof fetch;
}

interface FrigateEvent {
  id: string;
  camera: string;
  label: string;
  start_time: number;
  top_score?: number | null;
  data?: { top_score?: number; score?: number };
}

export class FrigateConnector {
  private readonly fetchImpl: typeof fetch;
  private readonly seen = new Set<string>();
  private readonly seq = new Map<string, number>();
  private readonly bootEpoch = Math.floor(Date.now() / 1000);
  private after = 0;

  constructor(private readonly o: FrigateOptions) {
    this.fetchImpl = o.fetch ?? globalThis.fetch.bind(globalThis);
  }

  async pollOnce(): Promise<number> {
    const res = await this.fetchImpl(
      `${this.o.frigateUrl}/api/events?after=${this.after}&limit=50`,
    );
    if (!res.ok) throw new Error(`frigate ${res.status}`);
    const events = (await res.json()) as FrigateEvent[];
    let posted = 0;
    for (const e of [...events].sort((a, b) => a.start_time - b.start_time)) {
      this.after = Math.max(this.after, Math.floor(e.start_time));
      if (this.seen.has(e.id)) continue;
      const cam = this.o.cameras[e.camera];
      if (!cam) continue; // unmapped camera: ignore
      this.seen.add(e.id);
      const score = e.top_score ?? e.data?.top_score ?? e.data?.score ?? 0;
      const seq = (this.seq.get(cam.deviceId) ?? 0) + 1;
      this.seq.set(cam.deviceId, seq);
      const source: Record<string, unknown> = {
        deviceId: cam.deviceId,
        capabilityId: "camera.detection",
      };
      if (cam.slot !== undefined) source.slot = cam.slot;
      const r = await this.fetchImpl(new URL("/v1/devices/observations", this.o.hubUrl), {
        method: "POST",
        headers: { Authorization: `Bearer ${cam.token}`, "Content-Type": "application/json" },
        body: JSON.stringify({
          schemaVersion: 1,
          propertyId: this.o.propertyId,
          source,
          eventType: "camera.object_detected",
          eventId: `${cam.deviceId}:frigate:${e.id}`,
          position: { bootEpoch: this.bootEpoch, sequence: String(seq) },
          observedAtUnixMs: String(Math.round(e.start_time * 1000)),
          payload: {
            objectDetected: {
              label: e.label,
              confidencePermille: Math.round(Math.min(1, Math.max(0, score)) * 1000),
            },
          },
        }),
      });
      if (r.ok) posted++;
    }
    return posted;
  }
}
