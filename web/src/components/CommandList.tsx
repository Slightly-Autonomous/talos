import type { StateResponse } from "@talos/contracts/talos/api/v1/api_pb";
import { commandStatusName, outcomeBadge, sortedCommands } from "../model/view";

export function CommandList({ state }: { state: StateResponse | undefined }) {
  const commands = sortedCommands(state);
  if (commands.length === 0) return <p className="muted">No commands yet.</p>;
  return (
    <ul className="commands" data-testid="commands">
      {commands.slice(0, 20).map((c) => {
        const badge = outcomeBadge(c.outcome);
        const last = c.history[c.history.length - 1];
        return (
          <li key={c.command?.id} data-testid={`command-${c.command?.operation}`}>
            <div>
              <strong>{c.command?.operation}</strong> → {c.command?.target?.deviceId}
              <span className={`badge badge-${badge.tone}`} data-testid="outcome">
                {badge.label}
              </span>
            </div>
            <div className="muted small">
              {commandStatusName(c.status)}
              {last?.reason ? ` · ${last.reason}` : ""}
              {last?.late ? " · late evidence" : ""}
            </div>
          </li>
        );
      })}
    </ul>
  );
}
