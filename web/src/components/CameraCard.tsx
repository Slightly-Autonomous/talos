// One Talos camera (ADR 0020): the Vision Node's stream and its recent detections with
// snapshots. Images come through the hub's media proxy with the signed-in person's token
// (never straight from Frigate), so the hub's checks apply to every image.
import { useEffect, useState } from "react";
import { HubError } from "@talos/contracts/client";
import { useHub } from "../hub/HubProvider";
import { recentDetections, streamView, type CameraGroup, type DetectionView } from "../model/view";

const TONE = { streaming: "fresh", down: "fault", unknown: "unknown" } as const;

function Snapshot({ d }: { d: DetectionView }) {
  const { client } = useHub();
  const [url, setUrl] = useState<string>();
  const [error, setError] = useState<string>();
  useEffect(() => {
    if (!d.mediaId || !d.hasSnapshot) return;
    let revoked: string | undefined;
    let stop = false;
    client
      .media(d.deviceId, d.mediaId, "snapshot.jpg")
      .then((b) => {
        if (stop) return;
        revoked = URL.createObjectURL(b);
        setUrl(revoked);
      })
      .catch((e) => !stop && setError(e instanceof HubError ? e.code : "unavailable"));
    return () => {
      stop = true;
      if (revoked) URL.revokeObjectURL(revoked);
    };
  }, [client, d.deviceId, d.mediaId, d.hasSnapshot]);
  if (!d.mediaId || !d.hasSnapshot) return <div className="snapshot muted small">no image</div>;
  if (error) return <div className="snapshot muted small">image {error}</div>;
  if (!url) return <div className="snapshot muted small">loading…</div>;
  return (
    <img
      className="snapshot"
      src={url}
      alt={`${d.label} at ${new Date(d.atMs).toLocaleTimeString()}`}
    />
  );
}

export function CameraCard({ group }: { group: CameraGroup }) {
  const { client, state, entries } = useHub();
  const [clipError, setClipError] = useState<string>();
  const stream = streamView(state, group.host.id);
  const detections = recentDetections(
    entries,
    group.views.map((v) => v.id),
  );

  async function openClip(d: DetectionView) {
    setClipError(undefined);
    try {
      const blob = await client.media(d.deviceId, d.mediaId!, "clip.mp4");
      const url = URL.createObjectURL(blob);
      window.open(url, "_blank", "noopener");
      setTimeout(() => URL.revokeObjectURL(url), 60_000);
    } catch (e) {
      setClipError(e instanceof HubError ? `${e.code}: ${e.message}` : String(e));
    }
  }

  return (
    <div className="camera" data-testid={`camera-${group.host.id}`}>
      <div>
        <span className={`dot dot-${TONE[stream.display]}`} aria-hidden /> {group.host.id}
      </div>
      <div
        className={stream.display === "down" ? "error small" : "muted small"}
        data-testid={`stream-${group.host.id}`}
        data-stream={stream.display}
        role={stream.display === "down" ? "alert" : undefined}
      >
        {stream.label}
      </div>
      {detections.length === 0 ? (
        <p className="muted small">No detections yet.</p>
      ) : (
        <ul className="detections">
          {detections.map((d) => (
            <li key={`${d.deviceId}:${d.mediaId ?? d.atMs}`} data-testid="detection">
              <Snapshot d={d} />
              <div className="small">
                {d.label} ({d.confidencePercent}%) · {new Date(d.atMs).toLocaleTimeString()}
                {d.mediaId && d.hasClip && (
                  <>
                    {" "}
                    <button className="link" onClick={() => void openClip(d)}>
                      clip
                    </button>
                  </>
                )}
              </div>
            </li>
          ))}
        </ul>
      )}
      {clipError && (
        <p role="alert" className="error small">
          Clip unavailable: {clipError}
        </p>
      )}
    </div>
  );
}
