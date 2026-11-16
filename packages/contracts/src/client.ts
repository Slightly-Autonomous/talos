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
    const res = await this.fetchImpl(new URL(path, this.opts.baseUrl), {
      ...init,
      headers: {
        Authorization: `Bearer ${this.opts.token}`,
        "Content-Type": "application/json",
        ...(init.headers ?? {}),
      },
    });
    const body: unknown = await res.json();
    if (!res.ok) {
      const err = fromJson(ErrorResponseSchema, body as never, { ignoreUnknownFields: true });
      throw new HubError(res.status, err.code, err.message);
    }
    return fromJson(schema, body as never, { ignoreUnknownFields: true });
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

  // Async iterator over live journal entries (SSE via fetch, so the bearer
  // token stays in a header and never in the URL).
  async *events(after: bigint, signal?: AbortSignal): AsyncGenerator<JournalEntry> {
    const res = await this.fetchImpl(new URL(`/v1/events?after=${after}`, this.opts.baseUrl), {
      headers: { Authorization: `Bearer ${this.opts.token}` },
      signal,
    });
    if (!res.ok || !res.body)
      throw new HubError(res.status, "stream_failed", "event stream failed");
    const reader = res.body.getReader();
    const decoder = new TextDecoder();
    let buffer = "";
    for (;;) {
      const { done, value } = await reader.read();
      if (done) return;
      buffer += decoder.decode(value, { stream: true });
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
          yield fromJson(JournalEntrySchema, JSON.parse(data), { ignoreUnknownFields: true });
      }
    }
  }
}
