#include "edyn/constraints/soft_distance_constraint.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/mass.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/delta_linvel.hpp"
#include "edyn/comp/delta_angvel.hpp"
#include "edyn/comp/origin.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/dynamics/row_cache.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/math/transform.hpp"
#include <entt/entity/registry.hpp>

namespace edyn {

template<>
void prepare_constraint<soft_distance_constraint>(
    const entt::registry &, entt::entity, soft_distance_constraint &con,
    constraint_row_prep_cache &cache, scalar dt,
    const vector3 &originA, const vector3 &posA, const quaternion &ornA,
    const vector3 &linvelA, const vector3 &angvelA,
    scalar inv_mA, const matrix3x3 &inv_IA,
    const vector3 &originB, const vector3 &posB, const quaternion &ornB,
    const vector3 &linvelB, const vector3 &angvelB,
    scalar inv_mB, const matrix3x3 &inv_IB) {

    auto pivotA = to_world_space(con.pivot[0], originA, ornA);
    auto pivotB = to_world_space(con.pivot[1], originB, ornB);
    auto rA = pivotA - posA;
    auto rB = pivotB - posB;
    auto d = pivotA - pivotB;
    auto dist_sqr = length_sqr(d);
    auto dist = std::sqrt(dist_sqr);
    vector3 dn;

    if (dist_sqr > EDYN_EPSILON) {
        dn = d / dist;
    } else {
        d = dn = vector3_x;
    }

    auto p = cross(rA, dn);
    auto q = cross(rB, dn);

    {
        // Spring row. By setting the error to +/- `large_scalar`, it will
        // always apply the impulse set in the limits.
        auto error = con.distance - dist;
        auto spring_force = con.stiffness * error;
        auto spring_impulse = spring_force * dt;

        auto &row = cache.add_row();
        row.J = {dn, p, -dn, -q};
        row.lower_limit = std::min(spring_impulse, scalar(0));
        row.upper_limit = std::max(scalar(0), spring_impulse);
        row.impulse = con.impulse[0];

        auto &options = cache.get_options();
        options.error = spring_impulse > 0 ? -large_scalar : large_scalar;
    }

    {
        // Damping row. It functions like friction where the force is
        // proportional to the relative speed.
        auto &row = cache.add_row();
        row.J = {dn, p, -dn, -q};

        auto relspd = dot(row.J[0], linvelA) +
                        dot(row.J[1], angvelA) +
                        dot(row.J[2], linvelB) +
                        dot(row.J[3], angvelB);
        auto damping_force = con.damping * relspd;
        auto damping_impulse = damping_force * dt;
        auto impulse = std::abs(damping_impulse);
        row.lower_limit = -impulse;
        row.upper_limit =  impulse;
        row.impulse = con.impulse[1];
    }
}

}
