#include "edyn/constraints/hinge_constraint.hpp"
#include "edyn/dynamics/position_solver.hpp"
#include "edyn/math/geom.hpp"
#include "edyn/math/math.hpp"
#include "edyn/math/constants.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/math/transform.hpp"
#include "edyn/comp/mass.hpp"
#include "edyn/comp/inertia.hpp"
#include "edyn/comp/linvel.hpp"
#include "edyn/comp/angvel.hpp"
#include "edyn/comp/delta_linvel.hpp"
#include "edyn/comp/delta_angvel.hpp"
#include "edyn/comp/origin.hpp"
#include "edyn/dynamics/row_cache.hpp"
#include "edyn/util/constraint_util.hpp"
#include <entt/entity/registry.hpp>
#include <cmath>

namespace edyn {

void hinge_constraint::set_axes(const vector3 &axisA, const vector3 &axisB) {
    vector3 p, q;
    plane_space(axisA, p, q);
    frame[0] = matrix3x3_columns(axisA, p, q);
    plane_space(axisB, p, q);
    frame[1] = matrix3x3_columns(axisB, p, q);
}

void hinge_constraint::reset_angle(const quaternion &ornA, const quaternion &ornB) {
    auto p = rotate(ornA, frame[0].column(1));
    auto q = rotate(ornA, frame[0].column(2));
    auto angle_axisB = rotate(ornB, frame[1].column(1));
    angle = std::atan2(dot(angle_axisB, q), dot(angle_axisB, p));
}

template<>
void prepare_constraint<hinge_constraint>(
    const entt::registry &, entt::entity, hinge_constraint &con,
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

    const auto rA_skew = skew_matrix(rA);
    const auto rB_skew = skew_matrix(rB);
    constexpr auto I = matrix3x3_identity;
    auto row_idx = size_t{};

    // Make the position of pivot points match, akin to a `point_constraint`.
    for (int i = 0; i < 3; ++i) {
        auto &row = cache.add_row();
        row.J = {I.row[i], -rA_skew.row[i],
                -I.row[i],  rB_skew.row[i]};
        row.lower_limit = -EDYN_SCALAR_MAX;
        row.upper_limit = EDYN_SCALAR_MAX;
        row.impulse = con.impulse[row_idx++];
    }

    // Make relative angular velocity go to zero along directions orthogonal
    // to the hinge axis where rotations are allowed.
    auto p = rotate(ornA, con.frame[0].column(1));
    auto q = rotate(ornA, con.frame[0].column(2));

    {
        auto &row = cache.add_row();
        row.J = {vector3_zero, p, vector3_zero, -p};
        row.lower_limit = -EDYN_SCALAR_MAX;
        row.upper_limit = EDYN_SCALAR_MAX;
        row.impulse = con.impulse[row_idx++];
    }

    {
        auto &row = cache.add_row();
        row.J = {vector3_zero, q, vector3_zero, -q};
        row.lower_limit = -EDYN_SCALAR_MAX;
        row.upper_limit = EDYN_SCALAR_MAX;
        row.impulse = con.impulse[row_idx++];
    }

    // Handle angular limits and friction.
    auto has_limit = con.angle_min < con.angle_max;
    auto has_spring = con.stiffness > 0;
    auto has_friction = con.friction_torque > 0 || con.damping > 0;
    vector3 hinge_axis;

    if (has_limit || has_spring || has_friction) {
        hinge_axis = rotate(ornA, con.frame[0].column(0));
    }

    if (has_limit || has_spring) {
        auto angle_axisB = rotate(ornB, con.frame[1].column(1));
        auto angle = std::atan2(dot(angle_axisB, q), dot(angle_axisB, p));
        auto previous_angle = normalize_angle(con.angle);
        // Find shortest path from previous angle to current in the [-π, π] range.
        auto angle_delta0 = angle - previous_angle;
        auto angle_delta1 = angle_delta0 + pi2 * to_sign(angle_delta0 < 0);
        auto angle_delta = std::abs(angle_delta0) < std::abs(angle_delta1) ? angle_delta0 : angle_delta1;
        con.angle += angle_delta;
    }

    if (has_limit) {
        // One row for angular limits.
        auto &row = cache.add_row();
        row.J = {vector3_zero, hinge_axis, vector3_zero, -hinge_axis};
        row.impulse = con.impulse[row_idx++];

        auto limit_error = scalar{0};
        auto halfway_limit = (con.angle_min + con.angle_max) / scalar(2);

        // Set constraint limits according to which is the closer angular limit.
        if (con.angle < halfway_limit) {
            limit_error = con.angle_min - con.angle;
            row.lower_limit = -large_scalar;
            row.upper_limit = 0;
        } else {
            limit_error = con.angle_max - con.angle;
            row.lower_limit = 0;
            row.upper_limit = large_scalar;
        }

        auto &options = cache.get_options();
        options.error = limit_error / dt;
        options.restitution = con.limit_restitution;

        // Another row for bump stop spring.
        if (con.bump_stop_stiffness > 0 && con.bump_stop_angle > 0) {
            auto bump_stop_deflection = scalar{0};
            auto bump_stop_min = con.angle_min + con.bump_stop_angle;
            auto bump_stop_max = con.angle_max - con.bump_stop_angle;

            if (con.angle < bump_stop_min) {
                bump_stop_deflection = con.angle - bump_stop_min;
            } else if (con.angle > bump_stop_max) {
                bump_stop_deflection = con.angle - bump_stop_max;
            }

            if (bump_stop_deflection != 0) {
                auto &row = cache.add_row();
                row.J = {vector3_zero, hinge_axis, vector3_zero, -hinge_axis};
                row.impulse = con.impulse[row_idx++];

                auto spring_force = con.bump_stop_stiffness * bump_stop_deflection;
                auto spring_impulse = spring_force * dt;
                row.lower_limit = std::min(spring_impulse, scalar(0));
                row.upper_limit = std::max(scalar(0), spring_impulse);

                auto &options = cache.get_options();
                options.error = -bump_stop_deflection / dt;
            }
        }
    }

    if (has_spring) {
        auto &row = cache.add_row();
        row.J = {vector3_zero, hinge_axis, vector3_zero, -hinge_axis};
        row.impulse = con.impulse[row_idx++];

        auto deflection = con.angle - con.rest_angle;
        auto spring_force = con.stiffness * deflection;
        auto spring_impulse = spring_force * dt;
        row.lower_limit = std::min(spring_impulse, scalar(0));
        row.upper_limit = std::max(scalar(0), spring_impulse);

        auto &options = cache.get_options();
        options.error = -deflection / dt;
    }

    if (has_friction) {
        // Since damping acts as a speed-dependent friction, a single row
        // is employed for both damping and constant friction.
        auto &row = cache.add_row();
        row.J = {vector3_zero, hinge_axis, vector3_zero, -hinge_axis};
        row.impulse = con.impulse[row_idx++];

        auto friction_impulse = con.friction_torque * dt;

        if (con.damping > 0) {
            auto relvel = dot(angvelA, hinge_axis) - dot(angvelB, hinge_axis);
            friction_impulse += std::abs(relvel) * con.damping * dt;
        }

        row.lower_limit = -friction_impulse;
        row.upper_limit = friction_impulse;
    }
}

template<>
void prepare_position_constraint<hinge_constraint>(
    entt::registry &registry, entt::entity entity, hinge_constraint &con,
    position_solver &solver) {

    auto originA = solver.get_originA(), originB = solver.get_originB();
    auto &posA = *solver.posA, &posB = *solver.posB;
    auto &ornA = *solver.ornA, &ornB = *solver.ornB;

    auto axisA = rotate(ornA, con.frame[0].column(0));
    auto axisB = rotate(ornB, con.frame[1].column(0));

    // Apply angular correction first, with the goal of aligning the hinge axes.
    vector3 p, q;
    plane_space(axisA, p, q);
    auto u = cross(axisA, axisB);

    if (auto error = dot(u, p); std::abs(error) > EDYN_EPSILON) {
        solver.solve({vector3_zero, p, vector3_zero, -p}, error);
    }

    if (auto error = dot(u, q); std::abs(error) > EDYN_EPSILON) {
        solver.solve({vector3_zero, q, vector3_zero, -q}, error);
    }

    // Now apply another correction to join the pivot points together.
    auto pivotA = to_world_space(con.pivot[0], originA, ornA);
    auto pivotB = to_world_space(con.pivot[1], originB, ornB);
    auto dir = pivotA - pivotB;
    auto error = length(dir);

    if (error > EDYN_EPSILON) {
        dir /= error;
        auto rA = pivotA - posA;
        auto rB = pivotB - posB;
        solver.solve({dir, cross(rA, dir), -dir, -cross(rB, dir)}, -error);
    }
}

}
