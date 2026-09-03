// mode.h -- the flight state machine's vocabulary.
//
// Policy decides which mode we are in. Flight turns that mode into an
// acceleration. Illegal combinations (committed AND stalking) cannot be
// represented. Flags that belong inside a state (leashed) live
// on Policy, not as extra modes.
#ifndef SWARM_MODE_H
#define SWARM_MODE_H

#include <cstdint>

namespace sw {

enum class Mode : uint8_t {
    Forming,      // spawned; flying out to the ring slot. Not chasing.
    Picketing,    // on station, facing outward, watching its sector
    Watching,     // still on the ring, yawed at an owned inbound, classifying
    Stalking,     // eased ≤40 m off the slot toward a compact inbound; can reverse
    Scrambling,   // left the ring early, before Hostile; abort if not a threat
    Ramming,      // spent; flying to collide with a Hostile
};

inline bool Intercepting(Mode m) {
    return m == Mode::Scrambling || m == Mode::Ramming;
}

inline const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Forming:    return "forming";
        case Mode::Picketing:  return "picket";
        case Mode::Watching:   return "watch";
        case Mode::Stalking:   return "stalk";
        case Mode::Scrambling: return "scramble";
        case Mode::Ramming:    return "ram";
    }
    return "forming";
}

}  // namespace sw

#endif  // SWARM_MODE_H
