// Configuration editors (ADR 0012). Edits are drafts until saved; saving
// creates a new hub revision (owner only) and history keeps the old layout.
import { useEffect, useMemo, useState } from "react";
import { clone, create } from "@bufbuild/protobuf";
import { HubError } from "@talos/contracts/client";
import type { RulePreviewResponse } from "@talos/contracts/talos/api/v1/api_pb";
import {
  PointSchema,
  PropertySchema,
  ZoneKind,
  type Property,
} from "@talos/contracts/talos/v1/property_pb";
import { ArmMode } from "@talos/contracts/talos/v1/rule_pb";
import { LayoutCanvas } from "../components/LayoutCanvas";
import { useHub } from "../hub/HubProvider";
import {
  draftFromRule,
  emptyDraft,
  layoutDiff,
  ruleFromDraft,
  snap,
  withProperty,
  withRules,
  zoneAt,
  type RuleDraft,
} from "../model/config";

export function Configure() {
  const [section, setSection] = useState<"layout" | "rules">("layout");
  const { config } = useHub();
  if (!config?.document) {
    return <p className="muted">This hub does not expose versioned configuration.</p>;
  }
  return (
    <div>
      <div className="subtabs">
        <button
          className={section === "layout" ? "tab active" : "tab"}
          onClick={() => setSection("layout")}
        >
          Layout
        </button>
        <button
          className={section === "rules" ? "tab active" : "tab"}
          onClick={() => setSection("rules")}
        >
          Rules
        </button>
        <span className="muted small">
          Revision {config.revision} · saved by {config.principalId}
        </span>
      </div>
      {section === "layout" ? <LayoutEditor /> : <RuleEditor />}
    </div>
  );
}

function useSave() {
  const { client, config, reloadConfig } = useHub();
  const [message, setMessage] = useState<{ tone: "good" | "bad"; text: string }>();
  async function save(document: NonNullable<typeof config>["document"]) {
    if (!config || !document) return false;
    setMessage(undefined);
    try {
      const r = await client.putConfig(config.revision, document);
      await reloadConfig();
      setMessage({ tone: "good", text: `Saved as revision ${r.revision}` });
      return true;
    } catch (e) {
      setMessage({
        tone: "bad",
        text:
          e instanceof HubError
            ? e.status === 409
              ? `Someone else changed the configuration (${e.message}). Reload and reapply your edits.`
              : `${e.status} ${e.code}: ${e.message}`
            : String(e),
      });
      return false;
    }
  }
  return { save, message };
}

type Tool = "zone" | "device" | "door";

function LayoutEditor() {
  const { config } = useHub();
  const saved = config!.document!.property!;
  const [draft, setDraft] = useState<Property>(() => clone(PropertySchema, saved));
  const [tool, setTool] = useState<Tool>("zone");
  const [zoneId, setZoneId] = useState(saved.zones[0]?.id ?? "");
  const [deviceId, setDeviceId] = useState(saved.devices[0]?.id ?? "");
  const [assetId, setAssetId] = useState(saved.assets[0]?.id ?? "");
  const [pendingDoorPoint, setPendingDoorPoint] = useState<{ x: number; y: number }>();
  const { save, message } = useSave();

  useEffect(() => setDraft(clone(PropertySchema, saved)), [saved]);
  const diff = useMemo(() => layoutDiff(saved, draft), [saved, draft]);
  const zone = draft.zones.find((z) => z.id === zoneId);

  const update = (fn: (p: Property) => void) =>
    setDraft((prev) => {
      const next = clone(PropertySchema, prev);
      fn(next);
      return next;
    });

  function onCanvas(raw: { x: number; y: number }) {
    const p = { x: snap(raw.x), y: snap(raw.y) };
    if (tool === "zone" && zoneId) {
      update((d) => d.zones.find((z) => z.id === zoneId)?.polygon.push(create(PointSchema, p)));
    } else if (tool === "device" && deviceId) {
      update((d) => {
        const dev = d.devices.find((x) => x.id === deviceId);
        if (!dev) return;
        dev.position = { $typeName: "talos.v1.Point", ...p };
        const z = zoneAt(d, p);
        if (z) dev.zoneId = z;
      });
    } else if (tool === "door" && assetId) {
      if (!pendingDoorPoint) {
        setPendingDoorPoint(p);
      } else {
        update((d) => {
          const a = d.assets.find((x) => x.id === assetId);
          if (a) {
            a.segment = {
              $typeName: "talos.v1.Segment",
              a: { $typeName: "talos.v1.Point", ...pendingDoorPoint },
              b: { $typeName: "talos.v1.Point", ...p },
            };
          }
        });
        setPendingDoorPoint(undefined);
      }
    }
  }

  function addZone() {
    const id = prompt("New zone id (lowercase, e.g. garage)")?.trim();
    if (!id || !/^[a-z0-9_]+$/.test(id) || draft.zones.some((z) => z.id === id)) return;
    update((d) =>
      d.zones.push({
        $typeName: "talos.v1.Zone",
        id,
        name: id,
        kind: ZoneKind.INDOOR,
        polygon: [],
      }),
    );
    setZoneId(id);
    setTool("zone");
  }

  return (
    <div className="live">
      <section className="panel plan-panel">
        <LayoutCanvas
          property={draft}
          selectedZone={tool === "zone" ? zoneId : undefined}
          selectedDevice={tool === "device" ? deviceId : undefined}
          selectedAsset={tool === "door" ? assetId : undefined}
          onClick={onCanvas}
        />
        <p className="legend muted">
          Grid = 1 m; clicks snap to 0.5 m. Zone tool: each click adds a vertex. Device tool: click
          to place (zone follows the polygon). Door tool: two clicks draw the opening.
        </p>
      </section>
      <aside className="side">
        <section className="panel editor-panel">
          <h3>Tool</h3>
          <div className="sim-actions">
            {(["zone", "device", "door"] as Tool[]).map((t) => (
              <button key={t} className={tool === t ? "selected" : ""} onClick={() => setTool(t)}>
                {t}
              </button>
            ))}
          </div>
          {tool === "zone" && (
            <>
              <label>
                Zone
                <select
                  value={zoneId}
                  onChange={(e) => setZoneId(e.target.value)}
                  data-testid="layout-zone"
                >
                  {draft.zones.map((z) => (
                    <option key={z.id} value={z.id}>
                      {z.name || z.id}
                    </option>
                  ))}
                </select>
              </label>
              {zone && (
                <>
                  <label>
                    Name
                    <input
                      value={zone.name}
                      onChange={(e) =>
                        update((d) => (d.zones.find((z) => z.id === zoneId)!.name = e.target.value))
                      }
                      data-testid="layout-zone-name"
                    />
                  </label>
                  <label>
                    Kind
                    <select
                      value={zone.kind}
                      onChange={(e) =>
                        update(
                          (d) =>
                            (d.zones.find((z) => z.id === zoneId)!.kind = Number(
                              e.target.value,
                            ) as ZoneKind),
                        )
                      }
                    >
                      <option value={ZoneKind.INDOOR}>Indoor</option>
                      <option value={ZoneKind.OUTDOOR}>Outdoor</option>
                    </select>
                  </label>
                  <p className="muted small">{zone.polygon.length} vertices</p>
                  <div className="sim-actions">
                    <button
                      onClick={() =>
                        update((d) => (d.zones.find((z) => z.id === zoneId)!.polygon = []))
                      }
                    >
                      Clear outline
                    </button>
                    <button
                      onClick={() =>
                        update((d) => d.zones.find((z) => z.id === zoneId)!.polygon.pop())
                      }
                    >
                      Undo vertex
                    </button>
                  </div>
                </>
              )}
              <button onClick={addZone}>Add zone…</button>
            </>
          )}
          {tool === "device" && (
            <label>
              Device
              <select value={deviceId} onChange={(e) => setDeviceId(e.target.value)}>
                {draft.devices.map((d) => (
                  <option key={d.id} value={d.id}>
                    {d.id} ({d.zoneId ?? "no zone"})
                  </option>
                ))}
              </select>
            </label>
          )}
          {tool === "door" && (
            <label>
              Opening
              <select value={assetId} onChange={(e) => setAssetId(e.target.value)}>
                {draft.assets.map((a) => (
                  <option key={a.id} value={a.id}>
                    {a.id}
                  </option>
                ))}
              </select>
              {pendingDoorPoint && <span className="muted small">Click the second end…</span>}
            </label>
          )}
        </section>
        <section className="panel">
          <h3>Pending changes</h3>
          {diff.length === 0 ? (
            <p className="muted">No changes.</p>
          ) : (
            <ul className="events" data-testid="layout-diff">
              {diff.map((d) => (
                <li key={d}>{d}</li>
              ))}
            </ul>
          )}
          <div className="sim-actions">
            <button
              disabled={diff.length === 0}
              onClick={() => void save(withProperty(config!.document!, draft))}
              data-testid="save-layout"
            >
              Save layout
            </button>
            <button
              disabled={diff.length === 0}
              onClick={() => setDraft(clone(PropertySchema, saved))}
            >
              Discard
            </button>
          </div>
          {message && (
            <p
              role="status"
              className={message.tone === "bad" ? "error" : "muted"}
              data-testid="save-status"
            >
              {message.text}
            </p>
          )}
        </section>
      </aside>
    </div>
  );
}

const EVENT_TYPES = [
  "motion.detected",
  "contact.opened",
  "contact.closed",
  "impact.detected",
  "camera.object_detected",
];
const MODES: [ArmMode, string][] = [
  [ArmMode.ARMED_AWAY, "Armed away"],
  [ArmMode.ARMED_HOME, "Armed home"],
  [ArmMode.DISARMED, "Disarmed"],
];

function RuleEditor() {
  const { client, config } = useHub();
  const doc = config!.document!;
  const property = doc.property!;
  const [selected, setSelected] = useState<string>(doc.rules[0]?.id ?? "__new");
  const [draft, setDraft] = useState<RuleDraft>(() =>
    doc.rules[0] ? draftFromRule(doc.rules[0]) : emptyDraft(),
  );
  const [error, setError] = useState<string>();
  const [preview, setPreview] = useState<RulePreviewResponse>();
  const { save, message } = useSave();
  const isNew = selected === "__new";

  function select(id: string) {
    setSelected(id);
    setPreview(undefined);
    setError(undefined);
    const r = doc.rules.find((x) => x.id === id);
    setDraft(r ? draftFromRule(r) : emptyDraft());
  }

  const set = <K extends keyof RuleDraft>(k: K, v: RuleDraft[K]) =>
    setDraft((d) => ({ ...d, [k]: v }));
  const built = () => {
    const prev = doc.rules.find((r) => r.id === draft.id);
    const rule = ruleFromDraft(draft, property.id, prev ? prev.revision + 1 : 1);
    if (typeof rule === "string") {
      setError(rule);
      return undefined;
    }
    setError(undefined);
    return rule;
  };

  async function runPreview() {
    const rule = built();
    if (!rule) return;
    try {
      setPreview(await client.previewRule(rule, Date.now() - 24 * 3600 * 1000));
    } catch (e) {
      setError(e instanceof HubError ? e.message : String(e));
    }
  }

  async function saveRule(remove = false) {
    const rule = remove ? undefined : built();
    if (!remove && !rule) return;
    const others = doc.rules.filter((r) => r.id !== (remove ? selected : draft.id));
    if (await save(withRules(doc, rule ? [...others, rule] : others))) {
      if (remove) select("__new");
      else setSelected(draft.id);
    }
  }

  const speakers = property.devices.map((d) => d.id);
  return (
    <div className="live">
      <section className="panel editor-panel">
        <div className="panel-head">
          <h3>Rule</h3>
          <select
            value={selected}
            onChange={(e) => select(e.target.value)}
            data-testid="rule-select"
          >
            {doc.rules.map((r) => (
              <option key={r.id} value={r.id}>
                {r.id}
                {r.enabled ? "" : " (disabled)"}
              </option>
            ))}
            <option value="__new">+ New rule</option>
          </select>
        </div>
        <div className="form-grid">
          <label>
            Id
            <input
              value={draft.id}
              disabled={!isNew}
              onChange={(e) => set("id", e.target.value)}
              data-testid="rule-id"
            />
          </label>
          <label className="inline">
            <input
              type="checkbox"
              checked={draft.enabled}
              onChange={(e) => set("enabled", e.target.checked)}
            />{" "}
            Enabled
          </label>
          <label>
            When
            <select
              value={draft.triggerEventType}
              onChange={(e) => set("triggerEventType", e.target.value)}
              data-testid="rule-trigger"
            >
              {EVENT_TYPES.map((t) => (
                <option key={t}>{t}</option>
              ))}
            </select>
          </label>
          <label>
            In zone
            <select
              value={draft.zoneId}
              onChange={(e) => set("zoneId", e.target.value)}
              data-testid="rule-zone"
            >
              <option value="">any zone</option>
              {property.zones.map((z) => (
                <option key={z.id} value={z.id}>
                  {z.name || z.id}
                </option>
              ))}
            </select>
          </label>
          <fieldset>
            <legend>While</legend>
            {MODES.map(([m, label]) => (
              <label key={m} className="inline">
                <input
                  type="checkbox"
                  checked={draft.modes.includes(m)}
                  onChange={(e) =>
                    set(
                      "modes",
                      e.target.checked ? [...draft.modes, m] : draft.modes.filter((x) => x !== m),
                    )
                  }
                />{" "}
                {label}
              </label>
            ))}
          </fieldset>
          <label className="inline">
            <input
              type="checkbox"
              checked={draft.schedule !== null}
              onChange={(e) =>
                set("schedule", e.target.checked ? { start: "22:00", end: "06:00" } : null)
              }
            />{" "}
            Only between
          </label>
          {draft.schedule && (
            <div className="sim-actions">
              <input
                value={draft.schedule.start}
                onChange={(e) => set("schedule", { ...draft.schedule!, start: e.target.value })}
                aria-label="Start time"
              />
              <input
                value={draft.schedule.end}
                onChange={(e) => set("schedule", { ...draft.schedule!, end: e.target.value })}
                aria-label="End time"
              />
            </div>
          )}
          <label>
            Then
            <select
              value={draft.action}
              onChange={(e) => set("action", e.target.value as RuleDraft["action"])}
            >
              <option value="speak">Speak on a speaker (verified by playback)</option>
              <option value="notify">Notify the owner (acknowledged only)</option>
            </select>
          </label>
          <label>
            Device
            <select
              value={draft.targetDeviceId}
              onChange={(e) => set("targetDeviceId", e.target.value)}
              data-testid="rule-target"
            >
              <option value="">choose…</option>
              {speakers.map((d) => (
                <option key={d}>{d}</option>
              ))}
            </select>
          </label>
          <label>
            Message
            <input
              value={draft.text}
              onChange={(e) => set("text", e.target.value)}
              data-testid="rule-text"
            />
          </label>
        </div>
        {error && (
          <p role="alert" className="error">
            {error}
          </p>
        )}
        <div className="sim-actions">
          <button onClick={() => void runPreview()} data-testid="rule-preview">
            Preview last 24 h
          </button>
          <button onClick={() => void saveRule()} data-testid="rule-save">
            Save rule
          </button>
          {!isNew && <button onClick={() => void saveRule(true)}>Delete rule</button>}
        </div>
        {message && (
          <p
            role="status"
            className={message.tone === "bad" ? "error" : "muted"}
            data-testid="save-status"
          >
            {message.text}
          </p>
        )}
      </section>
      <aside className="side">
        <section className="panel">
          <h3>Preview</h3>
          {!preview ? (
            <p className="muted">
              Dry-runs this rule over recorded history with the arm mode at each moment. Nothing is
              sent.
            </p>
          ) : (
            <>
              <p data-testid="preview-summary">
                Would have fired {preview.matches.length} time
                {preview.matches.length === 1 ? "" : "s"} (of {preview.evaluatedObservations}{" "}
                observations)
              </p>
              <ol className="events">
                {preview.matches.slice(-20).map((m) => (
                  <li key={String(m.journalSeq)}>
                    <time>{new Date(Number(m.receivedAtUnixMs)).toLocaleTimeString()}</time>{" "}
                    {m.eventType} from {m.deviceId}
                  </li>
                ))}
              </ol>
            </>
          )}
        </section>
      </aside>
    </div>
  );
}
