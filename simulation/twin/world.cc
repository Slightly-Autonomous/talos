#include "simulation/twin/world.h"

namespace talos::sim {

namespace {
constexpr std::int32_t kDefaultTemperatureMilliC = 21'000;
constexpr std::uint32_t kDoorSlamMg = 300;
}  // namespace

Vec2 to_vec(const talos::sim::v1::Point& p) { return {p.x(), p.y()}; }

World::World(const talos::sim::v1::Site& site) {
  for (const auto& z : site.zones()) {
    ZoneShape shape{z.id(), {}};
    for (const auto& p : z.polygon()) shape.polygon.push_back(to_vec(p));
    zones_.push_back(std::move(shape));
  }
  for (const auto& w : site.walls()) walls_.push_back({to_vec(w.a()), to_vec(w.b())});
  for (const auto& p : site.portals()) {
    portals_[p.id()] = Portal{p.id(),
                              p.type(),
                              p.zone_id(),
                              {to_vec(p.a()), to_vec(p.b())},
                              p.initially_open(),
                              p.initially_locked()};
  }
}

std::optional<std::string> World::zone_at(Vec2 p) const {
  for (const auto& z : zones_) {
    if (contains(z.polygon, p)) return z.id;
  }
  return std::nullopt;
}

void World::record(std::int64_t t_ms, std::string kind, std::string subject, std::string detail) {
  truth_.push_back({t_ms, std::move(kind), std::move(subject), std::move(detail)});
}

void World::step(std::int64_t t_ms, double dt_s) {
  for (auto& [id, a] : actors_) {
    a.moved_this_tick = false;
    double budget = a.speed_mps * dt_s;
    while (budget > 0 && !a.path.empty()) {
      const Vec2 delta = a.path.front() - a.position;
      const double dist = length(delta);
      if (dist <= budget) {
        a.position = a.path.front();
        a.path.pop_front();
        budget -= dist;
      } else {
        a.position = a.position + delta * (budget / dist);
        budget = 0;
      }
      a.moved_this_tick = true;
    }
    auto zone = zone_at(a.position);
    if (zone != a.zone) {
      a.zone = zone;
      record(t_ms, "actor.zone", id, zone.value_or("(outside)"));
    }
  }
}

void World::actor_enter(std::int64_t t_ms, Actor actor) {
  actor.zone = zone_at(actor.position);
  record(t_ms, "actor.enter", actor.id, actor.zone.value_or("(outside)"));
  actors_[actor.id] = std::move(actor);
}

void World::actor_move(std::int64_t t_ms, const std::string& id, std::vector<Vec2> path) {
  auto it = actors_.find(id);
  if (it == actors_.end()) return;
  it->second.path.assign(path.begin(), path.end());
  record(t_ms, "actor.move", id, std::to_string(path.size()) + " waypoints");
}

void World::actor_leave(std::int64_t t_ms, const std::string& id) {
  if (actors_.erase(id)) record(t_ms, "actor.leave", id);
}

const Portal* World::portal(const std::string& id) const {
  auto it = portals_.find(id);
  return it == portals_.end() ? nullptr : &it->second;
}

bool World::set_portal_open(std::int64_t t_ms, const std::string& id, bool open) {
  auto it = portals_.find(id);
  if (it == portals_.end() || it->second.open == open) return false;
  if (open && it->second.locked) {
    record(t_ms, "portal.blocked", id, "locked");
    return false;
  }
  it->second.open = open;
  record(t_ms, open ? "portal.open" : "portal.close", id);
  if (!open && it->second.type == "portal.door") impact(t_ms, id, kDoorSlamMg);
  return true;
}

bool World::set_portal_locked(std::int64_t t_ms, const std::string& id, bool locked) {
  auto it = portals_.find(id);
  if (it == portals_.end()) return false;
  if (locked && it->second.open) {
    record(t_ms, "portal.lock_failed", id, "portal open");
    return false;
  }
  if (it->second.locked != locked) {
    it->second.locked = locked;
    record(t_ms, locked ? "portal.locked" : "portal.unlocked", id);
  }
  return true;
}

void World::impact(std::int64_t t_ms, const std::string& portal_id, std::uint32_t magnitude_mg) {
  pending_impacts_.emplace_back(portal_id, magnitude_mg);
  record(t_ms, "impact", portal_id, std::to_string(magnitude_mg) + " mg");
}

std::vector<std::pair<std::string, std::uint32_t>> World::take_impacts() {
  std::vector<std::pair<std::string, std::uint32_t>> out;
  out.swap(pending_impacts_);
  return out;
}

void World::set_temperature(std::int64_t t_ms, const std::string& zone_id, std::int32_t milli_c) {
  temperatures_[zone_id] = milli_c;
  record(t_ms, "temperature", zone_id, std::to_string(milli_c) + " mC");
}

std::int32_t World::temperature(const std::string& zone_id) const {
  auto it = temperatures_.find(zone_id);
  return it == temperatures_.end() ? kDefaultTemperatureMilliC : it->second;
}

void World::record_effect(std::int64_t t_ms, const std::string& device,
                          const std::string& command_id, const std::string& detail) {
  ++effects_[{device, command_id}];
  record(t_ms, "effect", device, command_id + ": " + detail);
}

std::uint32_t World::effect_count(const std::string& device) const {
  std::uint32_t n = 0;
  for (const auto& [key, count] : effects_) {
    if (key.first == device) n += count;
  }
  return n;
}

}  // namespace talos::sim
