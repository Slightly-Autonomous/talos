// Loads property, state and the journal from the hub, then follows the live
// event stream. The hub is the authority: the UI never derives state it
// cannot get from the API, and refetches /v1/state after every new entry.
import { createContext, useCallback, useContext, useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import { HubClient } from "@talos/contracts/client";
import type { StateResponse } from "@talos/contracts/talos/api/v1/api_pb";
import type { JournalEntry } from "@talos/contracts/talos/v1/journal_pb";
import type { Property } from "@talos/contracts/talos/v1/property_pb";

export type ConnectionStatus = "connecting" | "live" | "reconnecting";

export interface HubContextValue {
  client: HubClient;
  property: Property | undefined;
  state: StateResponse | undefined;
  entries: JournalEntry[];
  status: ConnectionStatus;
  error: string | undefined;
  refresh: () => Promise<void>;
}

const HubContext = createContext<HubContextValue | null>(null);

export function useHub(): HubContextValue {
  const ctx = useContext(HubContext);
  if (!ctx) throw new Error("useHub outside HubProvider");
  return ctx;
}

async function loadJournal(client: HubClient, after: bigint): Promise<JournalEntry[]> {
  const all: JournalEntry[] = [];
  let cursor = after;
  for (;;) {
    const page = await client.journal(cursor, 500);
    all.push(...page.entries);
    cursor = page.nextAfter;
    if (!page.hasMore) return all;
  }
}

export function HubProvider({ client, children }: { client: HubClient; children: ReactNode }) {
  const [property, setProperty] = useState<Property>();
  const [state, setState] = useState<StateResponse>();
  const [entries, setEntries] = useState<JournalEntry[]>([]);
  const [status, setStatus] = useState<ConnectionStatus>("connecting");
  const [error, setError] = useState<string>();
  const lastSeq = useRef(0n);
  const refreshTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  const refresh = useCallback(async () => {
    setState(await client.state());
  }, [client]);

  const scheduleRefresh = useCallback(() => {
    clearTimeout(refreshTimer.current);
    refreshTimer.current = setTimeout(() => void refresh().catch(() => {}), 100);
  }, [refresh]);

  useEffect(() => {
    const abort = new AbortController();
    let stopped = false;

    const run = async () => {
      setProperty(await client.property());
      while (!stopped) {
        try {
          const fresh = await loadJournal(client, lastSeq.current);
          if (fresh.length) {
            lastSeq.current = fresh[fresh.length - 1]!.journalSeq;
            setEntries((prev) => [...prev, ...fresh]);
          }
          await refresh();
          setStatus("live");
          setError(undefined);
          for await (const entry of client.events(lastSeq.current, abort.signal)) {
            if (entry.journalSeq <= lastSeq.current) continue;
            lastSeq.current = entry.journalSeq;
            setEntries((prev) => [...prev, entry]);
            scheduleRefresh();
          }
        } catch (e) {
          if (stopped) return;
          setStatus("reconnecting");
          setError(e instanceof Error ? e.message : String(e));
          await new Promise((r) => setTimeout(r, 2000));
        }
      }
    };
    void run().catch((e) => setError(String(e)));
    // Freshness is time-dependent: re-read state periodically.
    const poll = setInterval(() => void refresh().catch(() => {}), 5000);
    return () => {
      stopped = true;
      abort.abort();
      clearInterval(poll);
      clearTimeout(refreshTimer.current);
    };
  }, [client, refresh, scheduleRefresh]);

  return (
    <HubContext.Provider value={{ client, property, state, entries, status, error, refresh }}>
      {children}
    </HubContext.Provider>
  );
}
