import { useState } from "react";
import type { FormEvent } from "react";
import { HubClient, HubError } from "@talos/contracts/client";

const DEFAULT_URL = import.meta.env.VITE_HUB_URL ?? "http://localhost:8080";

export function Connect({ onConnected }: { onConnected: (c: HubClient, url: string) => void }) {
  const [url, setUrl] = useState(DEFAULT_URL);
  const [token, setToken] = useState("");
  const [error, setError] = useState<string>();
  const [busy, setBusy] = useState(false);

  async function submit(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError(undefined);
    const client = new HubClient({ baseUrl: url, token });
    try {
      await client.health();
      await client.state(); // proves the token works
      onConnected(client, url);
    } catch (err) {
      setError(
        err instanceof HubError
          ? `${err.status} ${err.code}: ${err.message}`
          : `Could not reach the hub at ${url}`,
      );
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className="connect">
      <h1>Talos</h1>
      <p className="muted">
        Connect to your property hub. The hub is the authority; this page is a client.
      </p>
      <form onSubmit={submit}>
        <label>
          Hub URL
          <input value={url} onChange={(e) => setUrl(e.target.value)} data-testid="hub-url" />
        </label>
        <label>
          Access token
          <input
            type="password"
            value={token}
            onChange={(e) => setToken(e.target.value)}
            autoComplete="current-password"
            data-testid="hub-token"
          />
        </label>
        <button type="submit" disabled={busy || !token} data-testid="connect">
          {busy ? "Connecting…" : "Connect"}
        </button>
        {error && (
          <p role="alert" className="error">
            {error}
          </p>
        )}
      </form>
    </main>
  );
}
