// Phase 0 shell. The real UI (property editor, live state, replay) arrives in
// Phase 4 and talks only to the hub API.
export function App() {
  return (
    <main style={{ fontFamily: "system-ui, sans-serif", maxWidth: 720, margin: "3rem auto" }}>
      <h1>Talos</h1>
      <p>Local-first spatial security and automation platform.</p>
      <p>
        No hub is connected yet. This web app is a client of the property hub and never the
        authority.
      </p>
    </main>
  );
}
