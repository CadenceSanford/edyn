#ifndef EDYN_COLLISION_CONTACT_POINT_HPP
#define EDYN_COLLISION_CONTACT_POINT_HPP

#include <array>
#include <cstdint>
#include <entt/entity/fwd.hpp>
#include <entt/entity/entity.hpp>
#include "edyn/math/vector3.hpp"

namespace edyn {

enum class contact_normal_attachment {
    normal_on_A,
    normal_on_B
};

struct contact_point {
    std::array<entt::entity, 2> body {entt::null, entt::null};
    vector3 pivotA; // A's pivot in object space.
    vector3 pivotB; // B's pivot in object space.
    vector3 normal; // Normal in world space.
    vector3 local_normal;
    contact_normal_attachment normal_attachment;
    scalar friction;
    scalar restitution;
    uint32_t lifetime {0};
    scalar distance;
};

}

#endif // EDYN_COLLISION_CONTACT_POINT_HPP
