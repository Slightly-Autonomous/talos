// Notification device adapter (ADR 0015): a hub device that executes
// `notify` commands by calling a webhook (ntfy, Home Assistant, Slack-style
// JSON). It talks to the hub only through the device API with its own
// device token, exactly like a hardware actuator. Notify is ack-only:
// a 2xx from the webhook is acknowledged, never "verified".

export interface NotifierOptions {
  hubUrl: string;
  deviceToken: string;
  webhookUrl: string;
  format: "ntfy" | "json";
  fetch?: typeof fetch;
}

interface Command {
  id: string;
  operation?: string;
  args?: { notify?: { recipientRole?: string; text?: string } };
}

export class Notifier {
  private readonly fetchImpl: typeof fetch;
  private readonly done = new Set<string>();

  constructor(private readonly o: NotifierOptions) {
    this.fetchImpl = o.fetch ?? globalThis.fetch.bind(globalThis);
  }

  private hub(path: string, body?: unknown) {
    return this.fetchImpl(new URL(path, this.o.hubUrl), {
      method: body === undefined ? "GET" : "POST",
      headers: {
        Authorization: `Bearer ${this.o.deviceToken}`,
        "Content-Type": "application/json",
      },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  }

  private async deliver(text: string, commandId: string): Promise<{ ok: boolean; detail: string }> {
    try {
      const res =
        this.o.format === "ntfy"
          ? await this.fetchImpl(this.o.webhookUrl, {
              method: "POST",
              headers: { Title: "Talos", Tags: "house", "X-Talos-Command": commandId },
              body: text,
            })
          : await this.fetchImpl(this.o.webhookUrl, {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ source: "talos", command_id: commandId, text }),
            });
      return { ok: res.ok, detail: `webhook ${res.status}` };
    } catch (e) {
      return { ok: false, detail: `webhook error: ${String(e)}` };
    }
  }

  // Fetch pending commands once and execute them.
  async processOnce(): Promise<number> {
    const res = await this.hub("/v1/devices/commands");
    if (!res.ok) throw new Error(`hub ${res.status}`);
    const { commands = [] } = (await res.json()) as { commands?: Command[] };
    let handled = 0;
    for (const c of commands) {
      if (this.done.has(c.id)) continue; // device-side idempotency
      this.done.add(c.id);
      const text = c.operation === "notify" ? c.args?.notify?.text : undefined;
      const result = text
        ? await this.deliver(text, c.id)
        : { ok: false, detail: "unsupported command" };
      await this.hub("/v1/devices/acks", {
        commandId: c.id,
        accepted: result.ok,
        detail: result.detail,
      });
      handled++;
    }
    return handled;
  }
}
