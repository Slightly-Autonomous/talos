// Ground-truth world for the twin: geometry, actors, portals, impacts,
// temperatures and actuator effects (spec §40). Production code never sees
// this; only virtual sensor outputs cross into the hub.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "simulation/base/geometry.h"
#include "talos/sim/v1/site.pb.h"

namespace talos::sim {

enum class ActorKind { kPerson, kPet };

struct Actor {
  std::string id;
  ActorKind kind = ActorKind::kPerson;
  Vec2 position;
  double speed_mps = 1.2;
  std::deque<Vec2> path;
  bool moved_this_tick = false;
  std::optional<std::string> zone;
};

struct Portal {
  std::string id;
  std::string type;
  std::string zone_id;
  Segment segment;
  bool open = false;
  bool locked = false;
};

struct TruthEvent {
  std::int64_t t_ms = 0;
  std::string kind;
  std::string subject;
  std::string detail;
};

Vec2 to_vec(const talos::sim::v1::Point& p);

class World {
 public:
  explicit World(const talos::sim::v1::Site& site);

  // Advances actors by dt and records zone transitions.
  void step(std::int64_t t_ms, double dt_s);

  void actor_enter(std::int64_t t_ms, Actor actor);
  void actor_move(std::int64_t t_ms, const std::string& id, std::vector<Vec2> path);
  void actor_leave(std::int64_t t_ms, const std::string& id);

  // Opening/closing a portal; closing a door produces a slam impact.
  bool set_portal_open(std::int64_t t_ms, const std::string& id, bool open);
  // A lock can only bolt a closed portal. Returns whether the bolt moved.
  bool set_portal_locked(std::int64_t t_ms, const std::string& id, bool locked);
  void impact(std::int64_t t_ms, const std::string& portal_id, std::uint32_t magnitude_mg);
  // Impacts raised since the last call: (portal_id, magnitude_mg).
  std::vector<std::pair<std::string, std::uint32_t>> take_impacts();

  void set_temperature(std::int64_t t_ms, const std::string& zone_id, std::int32_t milli_c);
  std::int32_t temperature(const std::string& zone_id) const;

  // Physical actuator effect, e.g. a speaker actually playing for a command.
  void record_effect(std::int64_t t_ms, const std::string& device, const std::string& command_id,
                     const std::string& detail);
  std::uint32_t effect_count(const std::string& device) const;
  // Effects per (device, command id): must never exceed 1.
  const std::map<std::pair<std::string, std::string>, std::uint32_t>& effects() const {
    return effects_;
  }

  // Enclosure tamper loops (ground truth per device): open = lid off or loop cut.
  void set_enclosure(std::int64_t t_ms, const std::string& device, bool open);
  // The loop opens for `ms` and closes again (a bounce), starting at t_ms.
  void enclosure_bounce(std::int64_t t_ms, const std::string& device, std::uint32_t ms);
  bool enclosure_open(const std::string& device) const;
  // Bounces raised since the last call for this device: durations in ms.
  std::vector<std::uint32_t> take_enclosure_bounces(const std::string& device);

  // USER button presses (TL-SN-002) since the last call for this device: hold times in ms.
  void press_user_button(std::int64_t t_ms, const std::string& device, std::uint32_t hold_ms);
  std::vector<std::uint32_t> take_user_presses(const std::string& device);
  // Battery voltage override (ground truth per device), if a stimulus set one.
  void set_battery(std::int64_t t_ms, const std::string& device, std::uint32_t mv);
  std::optional<std::uint32_t> battery_mv(const std::string& device) const;

  std::optional<std::string> zone_at(Vec2 p) const;
  const std::vector<Segment>& walls() const { return walls_; }
  const std::map<std::string, Actor>& actors() const { return actors_; }
  const Portal* portal(const std::string& id) const;
  const std::vector<TruthEvent>& truth() const { return truth_; }
  void record(std::int64_t t_ms, std::string kind, std::string subject, std::string detail = "");

 private:
  struct ZoneShape {
    std::string id;
    std::vector<Vec2> polygon;
  };
  std::vector<ZoneShape> zones_;
  std::vector<Segment> walls_;
  std::map<std::string, Portal> portals_;
  std::map<std::string, Actor> actors_;
  std::map<std::string, std::int32_t> temperatures_;
  std::vector<std::pair<std::string, std::uint32_t>> pending_impacts_;
  std::map<std::pair<std::string, std::string>, std::uint32_t> effects_;
  std::vector<TruthEvent> truth_;
  std::map<std::string, bool> enclosures_;
  std::map<std::string, std::vector<std::uint32_t>> bounces_;
  std::map<std::string, std::vector<std::uint32_t>> user_presses_;
  std::map<std::string, std::uint32_t> batteries_;
};

}  // namespace talos::sim
