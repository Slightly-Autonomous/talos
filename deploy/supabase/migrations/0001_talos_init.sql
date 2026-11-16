-- Talos read-model copy in Supabase (ADR 0010).
-- The hub is the authority. These tables are a read-only COPY written by
-- connectors/supabase_sync with the service role (which bypasses RLS).
-- Signed-in users can read only properties they are members of.
-- Nothing here can issue commands: remote control goes through the hub API.

create table if not exists public.talos_property_members (
  property_id text not null,
  user_id uuid not null references auth.users (id) on delete cascade,
  role text not null default 'viewer',
  primary key (property_id, user_id)
);

create table if not exists public.talos_journal (
  property_id text not null,
  journal_seq bigint not null,
  received_at timestamptz not null,
  kind text not null,
  entry jsonb not null,
  primary key (property_id, journal_seq)
);
create index if not exists talos_journal_received_at on public.talos_journal (property_id, received_at);

create table if not exists public.talos_capability_state (
  property_id text not null,
  device_id text not null,
  slot integer not null,
  capability_id text not null,
  value jsonb,
  event_type text not null,
  freshness text not null,
  received_at timestamptz not null,
  journal_seq bigint not null,
  primary key (property_id, device_id, slot, capability_id)
);

create table if not exists public.talos_commands (
  property_id text not null,
  command_id text not null,
  operation text not null,
  status text not null,
  outcome text not null,
  view jsonb not null,
  primary key (property_id, command_id)
);

create table if not exists public.talos_property_state (
  property_id text primary key,
  arm_mode text not null,
  through_seq bigint not null,
  synced_at timestamptz not null
);

create table if not exists public.talos_sync_cursor (
  property_id text primary key,
  after_seq bigint not null,
  updated_at timestamptz not null
);

-- Row level security: deny by default, members may read their properties.
alter table public.talos_property_members enable row level security;
alter table public.talos_journal enable row level security;
alter table public.talos_capability_state enable row level security;
alter table public.talos_commands enable row level security;
alter table public.talos_property_state enable row level security;
alter table public.talos_sync_cursor enable row level security;  -- service role only

create or replace function public.talos_is_member(pid text)
returns boolean language sql stable security definer set search_path = public as $$
  select exists (
    select 1 from public.talos_property_members m
    where m.property_id = pid and m.user_id = auth.uid()
  );
$$;

create policy "members read own membership" on public.talos_property_members
  for select to authenticated using (user_id = auth.uid());
create policy "members read journal" on public.talos_journal
  for select to authenticated using (public.talos_is_member(property_id));
create policy "members read capability state" on public.talos_capability_state
  for select to authenticated using (public.talos_is_member(property_id));
create policy "members read commands" on public.talos_commands
  for select to authenticated using (public.talos_is_member(property_id));
create policy "members read property state" on public.talos_property_state
  for select to authenticated using (public.talos_is_member(property_id));

-- Live updates for remote dashboards.
alter publication supabase_realtime add table
  public.talos_capability_state, public.talos_commands, public.talos_property_state;
