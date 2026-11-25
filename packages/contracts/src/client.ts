// Typed client for the hub HTTP API v1 (ADR 0010). Works against a local hub
// or a cloud-hosted one: only the base URL and token change. Used by the web
// app and connectors; never talks to storage directly.
import { fromJson, toJson, type DescMessage, type MessageShape } from "@bufbuild/protobuf";
import {
  CommandRequestSchema,
  CommandResponseSchema,
  ConfigResponseSchema,
  DiagnosticsResponseSchema,
  HealthListResponseSchema,
  PutConfigRequestSchema,
  RulePreviewRequestSchema,
  RulePreviewResponseSchema,
  CommandViewSchema,
  ErrorResponseSchema,
  HealthResponseSchema,
  IngestResponseSchema,
  JournalPageSchema,
  SetArmModeResponseSchema,
  StateResponseSchema,
  type CommandRequest,
} from "./gen/talos/api/v1/api_pb.ts";
import { JournalEntrySchema, type JournalEntry } from "./gen/talos/v1/journal_pb.ts";
import { PropertySchema } from "./gen/talos/v1/property_pb.ts";
import type { ArmMode, Rule } from "./gen/talos/v1/rule_pb.ts";
import type { ConfigDocument } from "./gen/talos/server/v1/config_pb.ts";
import { create } from "@bufbuild/protobuf";
import { ArmModeSchema } from "./gen/talos/v1/rule_pb.ts";
import { enumToJson } from "@bufbuild/protobuf";

export class HubError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
  ) {
    super(message);
  }
}

export interface HubClientOptions {
  baseUrl: string;
  token: string;
  fetch?: typeof fetch;
}

// The hub sends an SSE keepalive every 15 s. With no bytes for this long the
// stream is treated as dead (half-open TCP, stalled proxy) and fails, so the
// caller can show "not live" and reconnect.
export const EVENT_STREAM_IDLE_TIMEOUT_MS = 40_000;

export interface EventStreamOptions {
  idleTimeoutMs?: number;
}

// Resolves an API path against the base URL without dropping a base-path
// prefix: base "https://x/talos" + "/v1/state" -> "https://x/talos/v1/state".
export function hubUrl(baseUrl: string, path: string): URL {
  const base = baseUrl.endsWith("/") ? baseUrl : `${baseUrl}/`;
  return new URL(path.replace(/^\/+/, ""), base);
}

function parseJson(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return undefined;
  }
}

export class HubClient {
  private readonly fetchImpl: typeof fetch;

  constructor(private readonly opts: HubClientOptions) {
    this.fetchImpl = opts.fetch ?? globalThis.fetch.bind(globalThis);
  }

  private async call<D extends DescMessage>(
    schema: D,
    path: string,
    init: RequestInit = {},
  ): Promise<MessageShape<D>> {
    const res = await this.fetchImpl(hubUrl(this.opts.baseUrl, path), {
      ...init,
      headers: {
        Authorization: `Bearer ${this.opts.token}`,
        "Content-Type": "application/json",
        ...(init.headers ?? {}),
      },
    });
    // Check the status before decoding: a proxy error page or an empty 404 is
    // not JSON, and must still surface as a HubError.
    const text = await res.text();
    const body = parseJson(text);
    if (!res.ok) {
      if (body !== null && typeof body === "object" && !Array.isArray(body)) {
        try {
          const err = fromJson(ErrorResponseSchema, body as never, { ignoreUnknownFields: true });
          if (err.code || err.message) throw new HubError(res.status, err.code, err.message);
        } catch (e) {
          if (e instanceof HubError) throw e;
        }
      }
      throw new HubError(
        res.status,
        `http_${res.status}`,
        res.statusText || text.slice(0, 200) || `HTTP ${res.status}`,
      );
    }
    if (body === undefined) throw new HubError(res.status, "bad_response", "response is not JSON");
    return fromJson(schema, body as never, { ignoreUnknownFields: true });
  }

  // Camera media through the hub (ADR 0020): a snapshot or clip the hub
  // fetched from the recorder, for a media id this device reported. People
  // only; the hub refuses agents.
  async media(
    deviceId: string,
    mediaId: string,
    kind: "snapshot.jpg" | "clip.mp4",
  ): Promise<Blob> {
    const path = `/v1/media/${encodeURIComponent(deviceId)}/${encodeURIComponent(mediaId)}/${kind}`;
    const res = await this.fetchImpl(hubUrl(this.opts.baseUrl, path), {
      headers: { Authorization: `Bearer ${this.opts.token}` },
    });
    if (!res.ok) {
      const body = parseJson(await res.text()) as { code?: string; message?: string } | undefined;
      throw new HubError(res.status, body?.code ?? `http_${res.status}`, body?.message ?? res.statusText);
    }
    return res.blob();
  }

  health() {
    return this.call(HealthResponseSchema, "/v1/health");
  }
  property() {
    return this.call(PropertySchema, "/v1/property");
  }
  state() {
    return this.call(StateResponseSchema, "/v1/state");
  }
  stateAt(unixMs: number) {
    return this.call(StateResponseSchema, `/v1/state_at?t=${unixMs}`);
  }
  journal(after: bigint, limit = 100) {
    return this.call(JournalPageSchema, `/v1/journal?after=${after}&limit=${limit}`);
  }
  setArmMode(mode: ArmMode) {
    return this.call(SetArmModeResponseSchema, "/v1/arm", {
      method: "POST",
      body: JSON.stringify({ mode: enumToJson(ArmModeSchema, mode) }),
    });
  }
  requestCommand(req: CommandRequest) {
    return this.call(CommandResponseSchema, "/v1/commands", {
      method: "POST",
      body: JSON.stringify(toJson(CommandRequestSchema, req)),
    });
  }
  // Current config, or the revision in force at `atUnixMs` (ADR 0012).
  config(atUnixMs?: number) {
    return this.call(
      ConfigResponseSchema,
      atUnixMs === undefined ? "/v1/config" : `/v1/config?at=${atUnixMs}`,
    );
  }
  putConfig(baseRevision: number, document: ConfigDocument) {
    return this.call(ConfigResponseSchema, "/v1/config", {
      method: "PUT",
      body: JSON.stringify(
        toJson(PutConfigRequestSchema, create(PutConfigRequestSchema, { baseRevision, document })),
      ),
    });
  }
  previewRule(rule: Rule, fromUnixMs: number, toUnixMs = 0) {
    return this.call(RulePreviewResponseSchema, "/v1/rules/preview", {
      method: "POST",
      body: JSON.stringify(
        toJson(
          RulePreviewRequestSchema,
          create(RulePreviewRequestSchema, {
            rule,
            fromUnixMs: BigInt(fromUnixMs),
            toUnixMs: BigInt(toUnixMs),
          }),
        ),
      ),
    });
  }
  deviceHealth() {
    return this.call(HealthListResponseSchema, "/v1/health/devices");
  }
  diagnostics(deviceId: string) {
    return this.call(
      DiagnosticsResponseSchema,
      `/v1/diagnostics?device_id=${encodeURIComponent(deviceId)}`,
    );
  }
  command(id: string) {
    return this.call(CommandViewSchema, `/v1/commands/${encodeURIComponent(id)}`);
  }
  // Owner only: forget a device's source positions (journaled SourceReset)
  // after a poisoned boot epoch or a reflash with a rotated key (ADR 0008).
  resetDeviceSequence(deviceId: string) {
    return this.call(
      IngestResponseSchema,
      `/v1/devices/${encodeURIComponent(deviceId)}/sequence/reset`,
      { method: "POST", body: "" },
    );
  }

  // Async iterator over live journal entries (SSE via fetch, so the bearer
  // token stays in a header and never in the URL). Fails with HubError
  // "stream_idle" when nothing (not even a keepalive) arrives within
  // `idleTimeoutMs`, including while waiting for the response headers.
  async *events(
    after: bigint,
    signal?: AbortSignal,
    opts: EventStreamOptions = {},
  ): AsyncGenerator<JournalEntry> {
    const idleMs = opts.idleTimeoutMs ?? EVENT_STREAM_IDLE_TIMEOUT_MS;
    const ctrl = new AbortController();
    const onAbort = () => ctrl.abort(signal?.reason);
    if (signal?.aborted) ctrl.abort(signal.reason);
    signal?.addEventListener("abort", onAbort);
    let idle = false;
    let reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const arm = () => {
      clearTimeout(timer);
      timer = setTimeout(() => {
        idle = true;
        ctrl.abort();
        void reader?.cancel().catch(() => {});
      }, idleMs);
    };
    const idleError = () =>
      new HubError(0, "stream_idle", `no data from event stream for ${idleMs} ms`);
    try {
      arm();
      let res: Response;
      try {
        res = await this.fetchImpl(hubUrl(this.opts.baseUrl, `/v1/events?after=${after}`), {
          headers: { Authorization: `Bearer ${this.opts.token}` },
          signal: ctrl.signal,
        });
      } catch (e) {
        if (idle) throw idleError();
        throw e;
      }
      clearTimeout(timer);
      if (!res.ok || !res.body)
        throw new HubError(res.status, "stream_failed", "event stream failed");
      reader = res.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";
      for (;;) {
        arm();
        let chunk: ReadableStreamReadResult<Uint8Array>;
        try {
          chunk = await reader.read();
        } catch (e) {
          if (idle) throw idleError();
          throw e;
        }
        clearTimeout(timer);
        if (idle) throw idleError();
        if (chunk.done) return;
        buffer += decoder.decode(chunk.value, { stream: true });
        let split: number;
        while ((split = buffer.indexOf("\n\n")) >= 0) {
          const frame = buffer.slice(0, split);
          buffer = buffer.slice(split + 2);
          const data = frame
            .split("\n")
            .filter((l) => l.startsWith("data: "))
            .map((l) => l.slice(6))
            .join("\n");
          if (data)
            yield fromJson(JournalEntrySchema, JSON.parse(data), {
              ignoreUnknownFields: true,
            });
        }
      }
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener("abort", onAbort);
    }
  }
}
