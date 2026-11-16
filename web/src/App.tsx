import { useState } from "react";
import type { HubClient } from "@talos/contracts/client";
import { HubProvider, useHub } from "./hub/HubProvider";
import { Connect } from "./pages/Connect";
import { Engineering } from "./pages/Engineering";
import { Live } from "./pages/Live";
import { Replay } from "./pages/Replay";
import { Simulation } from "./pages/Simulation";
import { Configure } from "./pages/Configure";

const TABS = ["Live", "Replay", "Configure", "Engineering", "Simulation"] as const;
type Tab = (typeof TABS)[number];

export function App() {
  const [conn, setConn] = useState<{ client: HubClient; url: string }>();
  if (!conn) return <Connect onConnected={(client, url) => setConn({ client, url })} />;
  return (
    <HubProvider client={conn.client}>
      <Shell url={conn.url} onDisconnect={() => setConn(undefined)} />
    </HubProvider>
  );
}

function Shell({ url, onDisconnect }: { url: string; onDisconnect: () => void }) {
  const [tab, setTab] = useState<Tab>("Live");
  const { status, error } = useHub();
  return (
    <div className="app">
      <header className="topbar">
        <strong>Talos</strong>
        <nav role="tablist">
          {TABS.map((t) => (
            <button
              key={t}
              role="tab"
              aria-selected={tab === t}
              className={tab === t ? "tab active" : "tab"}
              onClick={() => setTab(t)}
            >
              {t}
            </button>
          ))}
        </nav>
        <span className={`conn conn-${status}`} title={error} data-testid="connection">
          {status === "live" ? "Live" : status === "connecting" ? "Connecting…" : "Reconnecting…"} ·{" "}
          {url}
        </span>
        <button className="link" onClick={onDisconnect}>
          Disconnect
        </button>
      </header>
      <main>
        {tab === "Live" && <Live />}
        {tab === "Replay" && <Replay />}
        {tab === "Configure" && <Configure />}
        {tab === "Engineering" && <Engineering />}
        {tab === "Simulation" && <Simulation />}
      </main>
    </div>
  );
}
