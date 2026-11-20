// Loads property, state and the journal from the hub, then follows the live
// event stream. The hub is the authority: the UI never derives state it
// cannot get from the API, and refetches /v1/state after every new entry.
// Freshness is aged locally (ageState): if state reads stop succeeding, every
// capability turns unknown instead of keeping its last "fresh" verdict.
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import type { ReactNode } from "react";
import { HubClient } from "@talos/contracts/client";
import type { ConfigResponse, StateResponse } from "@talos/contracts/talos/api/v1/api_pb";
import type { JournalEntry } from "@talos/contracts/talos/v1/journal_pb";
import type { Property } from "@talos/contracts/talos/v1/property_pb";
import { ageState } from "../model/view";

export type ConnectionStatus = "connecting" | "live" | "reconnecting";

export interface HubContextValue {
  client: HubClient;
  property: Property | undefined;
  config: ConfigResponse | undefined;
  // Locally aged: capabilities are UNKNOWN when the snapshot is too old or
  // the hub is unreachable.
  state: StateResponse | undefined;
  // True while `state` is being shown aged (unknown) for either reason.
  stateStale: boolean;
  entries: JournalEntry[];
  status: ConnectionStatus;
  error: string | undefined;
  refresh: () => Promise<void>;
  reloadConfig: () => Promise<void>;
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
  const [config, setConfig] = useState<ConfigResponse>();
  const [snapshot, setSnapshot] = useState<{ state: StateResponse; fetchedAtMs: number }>();
  const [reachable, setReachable] = useState(true);
  const [nowMs, setNowMs] = useState(() => Date.now());
  const [entries, setEntries] = useState<JournalEntry[]>([]);
  const [status, setStatus] = useState<ConnectionStatus>("connecting");
  const [error, setError] = useState<string>();
  const lastSeq = useRef(0n);
  const refreshTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  const refresh = useCallback(async () => {
    try {
      const s = await client.state();
      setSnapshot({ state: s, fetchedAtMs: Date.now() });
      setReachable(true);
    } catch (e) {
      setReachable(false);
      throw e;
    }
  }, [client]);

  useEffect(() => {
    const t = setInterval(() => setNowMs(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);

  const state = useMemo(
    () => ageState(snapshot?.state, snapshot?.fetchedAtMs, nowMs, reachable),
    [snapshot, nowMs, reachable],
  );
  const stateStale = !!snapshot && state !== snapshot.state;

  const reloadConfig = useCallback(async () => {
    try {
      const c = await client.config();
      setConfig(c);
      if (c.document?.property) setProperty(c.document.property);
    } catch {
      setProperty(await client.property()); // hub without versioned config
    }
  }, [client]);

  const scheduleRefresh = useCallback(() => {
    clearTimeout(refreshTimer.current);
    refreshTimer.current = setTimeout(() => void refresh().catch(() => {}), 100);
  }, [refresh]);

  useEffect(() => {
    const abort = new AbortController();
    let stopped = false;

    const run = async () => {
      await reloadConfig();
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
            if (entry.event.case === "configRevisionApplied") void reloadConfig();
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
  }, [client, refresh, scheduleRefresh, reloadConfig]);

  return (
    <HubContext.Provider
      value={{
        client,
        property,
        config,
        state,
        stateStale,
        entries,
        status,
        error,
        refresh,
        reloadConfig,
      }}
    >
      {children}
    </HubContext.Provider>
  );
}
