#include "edyn/util/collision_util.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/math/quaternion.hpp"
#include "edyn/shapes/mesh_shape.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/constraints/contact_constraint.hpp"
#include "edyn/collision/collide.hpp"
#include "edyn/comp/continuous.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/math/math.hpp"
#include "edyn/dynamics/material_mixing.hpp"
#include "edyn/util/triangle_util.hpp"

namespace edyn {

void update_contact_distances(entt::registry &registry) {
    auto cp_view = registry.view<contact_point>();
    auto tr_view = registry.view<position, orientation>();
    auto origin_view = registry.view<origin>();

    cp_view.each([&] (contact_point &cp) {
        auto [posA, ornA] = tr_view.get<position, orientation>(cp.body[0]);
        auto [posB, ornB] = tr_view.get<position, orientation>(cp.body[1]);
        auto originA = origin_view.contains(cp.body[0]) ? origin_view.get<origin>(cp.body[0]) : static_cast<vector3>(posA);
        auto originB = origin_view.contains(cp.body[1]) ? origin_view.get<origin>(cp.body[1]) : static_cast<vector3>(posB);
        auto pivotA_world = to_world_space(cp.pivotA, originA, ornA);
        auto pivotB_world = to_world_space(cp.pivotB, originB, ornB);
        cp.distance = dot(cp.normal, pivotA_world - pivotB_world);
    });
}

static scalar get_trimesh_friction(const triangle_mesh &trimesh,
                                   const vector3 &pivot, collision_feature coll_feature) {
    auto feature = std::get<triangle_feature>(coll_feature.feature);

    switch (feature) {
    case triangle_feature::vertex:
        return trimesh.get_vertex_friction(coll_feature.index);
    case triangle_feature::edge:
        return trimesh.get_edge_friction(coll_feature.index, pivot);
    case triangle_feature::face:
        return trimesh.get_face_friction(coll_feature.index, pivot);
    }

    return {};
}

static scalar get_trimesh_restitution(const triangle_mesh &trimesh,
                                      const vector3 &pivot, collision_feature coll_feature) {
    auto feature = std::get<triangle_feature>(coll_feature.feature);

    switch (feature) {
    case triangle_feature::vertex:
        return trimesh.get_vertex_restitution(coll_feature.index);
    case triangle_feature::edge:
        return trimesh.get_edge_restitution(coll_feature.index, pivot);
    case triangle_feature::face:
        return trimesh.get_face_restitution(coll_feature.index, pivot);
    }

    return {};
}

static scalar get_paged_mesh_friction(const paged_mesh_shape &shape,
                                      const vector3 &pivot, collision_feature coll_feature) {
    auto submesh = shape.trimesh->get_submesh(coll_feature.part);

    if (submesh) {
        return get_trimesh_friction(*submesh, pivot, coll_feature);
    }

    return {};
}

static scalar get_paged_mesh_restitution(const paged_mesh_shape &shape,
                                         const vector3 &pivot, collision_feature coll_feature) {
    auto submesh = shape.trimesh->get_submesh(coll_feature.part);

    if (submesh) {
        return get_trimesh_restitution(*submesh, pivot, coll_feature);
    }

    return {};
}

static bool try_assign_per_vertex_friction(
    contact_point &cp,
    const material_view_t &material_view,
    const mesh_shape_view_t &mesh_shape_view,
    const paged_mesh_shape_view_t &paged_mesh_shape_view) {

    if (mesh_shape_view.contains(cp.body[0])) {
        auto [shapeA] = mesh_shape_view.get(cp.body[0]);
        if (shapeA.trimesh->has_per_vertex_friction()) {
            auto [materialB] = material_view.get(cp.body[1]);
            cp.friction = material_mix_friction(get_trimesh_friction(*shapeA.trimesh, cp.pivotA, *cp.featureA), materialB.friction);
            return true;
        }
    } else if (mesh_shape_view.contains(cp.body[1])) {
        auto [shapeB] = mesh_shape_view.get(cp.body[1]);
        if (shapeB.trimesh->has_per_vertex_friction()) {
            auto [materialA] = material_view.get(cp.body[0]);
            cp.friction = material_mix_friction(materialA.friction, get_trimesh_friction(*shapeB.trimesh, cp.pivotB, *cp.featureB));
            return true;
        }
    } else if (paged_mesh_shape_view.contains(cp.body[0])) {
        auto [shapeA] = paged_mesh_shape_view.get(cp.body[0]);
        if (shapeA.trimesh->has_per_vertex_friction()) {
            auto [materialB] = material_view.get(cp.body[1]);
            cp.friction = material_mix_friction(get_paged_mesh_friction(shapeA, cp.pivotA, *cp.featureA), materialB.friction);
            return true;
        }
    } else if (paged_mesh_shape_view.contains(cp.body[1])) {
        auto [shapeB] = paged_mesh_shape_view.get(cp.body[1]);
        if (shapeB.trimesh->has_per_vertex_friction()) {
            auto [materialA] = material_view.get(cp.body[0]);
            cp.friction = material_mix_friction(materialA.friction, get_paged_mesh_friction(shapeB, cp.pivotB, *cp.featureB));
            return true;
        }
    }

    return false;
}

static bool try_assign_per_vertex_restitution(
    contact_point &cp,
    const material_view_t &material_view,
    const mesh_shape_view_t &mesh_shape_view,
    const paged_mesh_shape_view_t &paged_mesh_shape_view) {

    if (mesh_shape_view.contains(cp.body[0])) {
        auto [shapeA] = mesh_shape_view.get(cp.body[0]);
        if (shapeA.trimesh->has_per_vertex_restitution()) {
            auto [materialB] = material_view.get(cp.body[1]);
            cp.restitution = material_mix_restitution(get_trimesh_restitution(*shapeA.trimesh, cp.pivotA, *cp.featureA), materialB.restitution);
            return true;
        }
    } else if (mesh_shape_view.contains(cp.body[1])) {
        auto [shapeB] = mesh_shape_view.get(cp.body[1]);
        if (shapeB.trimesh->has_per_vertex_restitution()) {
            auto [materialA] = material_view.get(cp.body[0]);
            cp.restitution = material_mix_restitution(materialA.restitution, get_trimesh_restitution(*shapeB.trimesh, cp.pivotB, *cp.featureB));
            return true;
        }
    } else if (paged_mesh_shape_view.contains(cp.body[0])) {
        auto [shapeA] = paged_mesh_shape_view.get(cp.body[0]);
        if (shapeA.trimesh->has_per_vertex_restitution()) {
            auto [materialB] = material_view.get(cp.body[1]);
            cp.restitution = material_mix_restitution(get_paged_mesh_restitution(shapeA, cp.pivotA, *cp.featureA), materialB.restitution);
            return true;
        }
    } else if (paged_mesh_shape_view.contains(cp.body[1])) {
        auto [shapeB] = paged_mesh_shape_view.get(cp.body[1]);
        if (shapeB.trimesh->has_per_vertex_restitution()) {
            auto [materialA] = material_view.get(cp.body[0]);
            cp.restitution = material_mix_restitution(materialA.restitution, get_paged_mesh_restitution(shapeB, cp.pivotB, *cp.featureB));
            return true;
        }
    }

    return false;
}

void merge_point(const collision_result::collision_point &rp, contact_point &cp,
                 const orientation_view_t &orn_view,
                 const material_view_t &material_view,
                 const mesh_shape_view_t &mesh_shape_view,
                 const paged_mesh_shape_view_t &paged_mesh_shape_view) {
    cp.pivotA = rp.pivotA;
    cp.pivotB = rp.pivotB;
    cp.normal = rp.normal;
    cp.distance = rp.distance;
    cp.normal_attachment = rp.normal_attachment;
    cp.featureA = rp.featureA;
    cp.featureB = rp.featureB;

    if (rp.normal_attachment != contact_normal_attachment::none) {
        auto idx = rp.normal_attachment == contact_normal_attachment::normal_on_A ? 0 : 1;
        auto [orn] = orn_view.get(cp.body[idx]);
        cp.local_normal = rotate(conjugate(orn), rp.normal);
    } else {
        cp.local_normal = vector3_zero;
    }

    try_assign_per_vertex_friction(cp, material_view, mesh_shape_view, paged_mesh_shape_view);
    try_assign_per_vertex_restitution(cp, material_view, mesh_shape_view, paged_mesh_shape_view);
}

void create_contact_constraint(entt::registry &registry,
                               entt::entity contact_entity,
                               contact_point &cp) {
    auto material_view = registry.view<material>();
    auto [materialA] = material_view.get(cp.body[0]);
    auto [materialB] = material_view.get(cp.body[1]);

    auto &material_table = registry.ctx<material_mix_table>();
    auto stiffness = large_scalar;
    auto damping = large_scalar;

    if (auto *material = material_table.try_get({materialA.id, materialB.id})) {
        cp.restitution = material->restitution;
        cp.friction = material->friction;
        cp.roll_friction = material->roll_friction;
        cp.spin_friction = material->spin_friction;
        stiffness = material->stiffness;
        damping = material->damping;
    } else {
        auto mesh_shape_view = registry.view<mesh_shape>();
        auto paged_mesh_shape_view = registry.view<paged_mesh_shape>();

        if (!try_assign_per_vertex_friction(cp, material_view, mesh_shape_view, paged_mesh_shape_view)) {
            cp.friction = material_mix_friction(materialA.friction, materialB.friction);
        }

        if (!try_assign_per_vertex_restitution(cp, material_view, mesh_shape_view, paged_mesh_shape_view)) {
            cp.restitution = material_mix_restitution(materialA.restitution, materialB.restitution);
        }

        cp.roll_friction = material_mix_roll_friction(materialA.roll_friction, materialB.roll_friction);
        cp.spin_friction = material_mix_spin_friction(materialA.spin_friction, materialB.spin_friction);

        if (materialA.stiffness < large_scalar || materialB.stiffness < large_scalar) {
            stiffness = material_mix_stiffness(materialA.stiffness, materialB.stiffness);
            damping = material_mix_damping(materialA.damping, materialB.damping);
        }
    }

    // Contact constraints are never graph edges since they're effectively
    // a child of a manifold and the manifold is the graph edge.
    constexpr auto is_graph_edge = false;
    auto &contact = make_constraint<contact_constraint>(contact_entity, registry, cp.body[0], cp.body[1], is_graph_edge);
    contact.stiffness = stiffness;
    contact.damping = damping;
}

size_t find_nearest_contact(const contact_point &cp,
                            const collision_result &result) {
    auto shortest_dist_sqr = square(contact_caching_threshold);
    auto nearest_idx = result.num_points;

    for (size_t i = 0; i < result.num_points; ++i) {
        auto &coll_pt = result.point[i];
        auto dA = length_sqr(coll_pt.pivotA - cp.pivotA);
        auto dB = length_sqr(coll_pt.pivotB - cp.pivotB);

        if (dA < shortest_dist_sqr) {
            shortest_dist_sqr = dA;
            nearest_idx = i;
        }

        if (dB < shortest_dist_sqr) {
            shortest_dist_sqr = dB;
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

size_t find_nearest_contact_rolling(const collision_result &result, const vector3 &cp_pivot,
                                    const vector3 &origin, const quaternion &orn,
                                    const vector3 &angvel, scalar dt) {
    // Calculate previous orientation by integrating the angular velocity
    // backwards and check if the contact point pivot lies near the same
    // location as the result point in world space.
    auto nearest_idx = result.num_points;
    auto prev_orn = integrate(orn, angvel, -dt);
    auto prev_pivot = to_world_space(cp_pivot, origin, prev_orn);
    auto shortest_dist_sqr = square(contact_caching_threshold);

    for (size_t i = 0; i < result.num_points; ++i) {
        auto &coll_pt = result.point[i];
        auto pivotA = to_world_space(coll_pt.pivotA, origin, orn);
        auto dist_sqr = distance_sqr(pivotA, prev_pivot);

        if (dist_sqr < shortest_dist_sqr) {
            shortest_dist_sqr = dist_sqr;
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

entt::entity create_contact_point(entt::registry& registry,
                                  entt::entity manifold_entity,
                                  contact_manifold& manifold,
                                  const collision_result::collision_point& rp) {
    auto idx = manifold.num_points();

    EDYN_ASSERT(idx < max_contacts);

    auto contact_entity = registry.create();
    manifold.point[idx] = contact_entity;

    EDYN_ASSERT(length_sqr(rp.normal) > EDYN_EPSILON);

    auto local_normal = vector3{};

    if (rp.normal_attachment != contact_normal_attachment::none) {
        auto idx = rp.normal_attachment == contact_normal_attachment::normal_on_A ? 0 : 1;
        auto &orn = registry.get<orientation>(manifold.body[idx]);
        local_normal = rotate(conjugate(orn), rp.normal);
    }

    registry.emplace<contact_point>(
        contact_entity,
        manifold.body,
        rp.pivotA, // pivotA
        rp.pivotB, // pivotB
        rp.normal, // world space normal
        local_normal, // object space normal
        rp.normal_attachment, // to which rigid body the local normal is attached
        scalar{}, // friction
        scalar{}, // spin friction
        scalar{}, // roll friction
        scalar{}, // restitution
        uint32_t{0}, // lifetime
        rp.distance, // distance
        rp.featureA,
        rp.featureB
    );

    auto &contact_dirty = registry.get_or_emplace<dirty>(contact_entity);
    contact_dirty.set_new().created<contact_point>();

    if (registry.any_of<continuous_contacts_tag>(manifold.body[0]) ||
        registry.any_of<continuous_contacts_tag>(manifold.body[1])) {

        registry.emplace<continuous>(contact_entity).insert<edyn::contact_point>();
        contact_dirty.created<continuous>();
    }

    registry.get_or_emplace<dirty>(manifold_entity).updated<contact_manifold>();

    return contact_entity;
}

bool maybe_remove_point(contact_manifold &manifold, const contact_point &cp, size_t pt_idx,
                        const vector3 &posA, const quaternion &ornA,
                        const vector3 &posB, const quaternion &ornB) {
    constexpr auto threshold = contact_breaking_threshold;
    constexpr auto threshold_sqr = threshold * threshold;

    // Remove separating contact points.
    auto pA = to_world_space(cp.pivotA, posA, ornA);
    auto pB = to_world_space(cp.pivotB, posB, ornB);
    auto n = cp.normal;
    auto d = pA - pB;
    auto normal_dist = dot(d, n);
    auto tangential_dir = d - normal_dist * n; // tangential separation on contact plane
    auto tangential_dist_sqr = length_sqr(tangential_dir);

    if (normal_dist < threshold && tangential_dist_sqr < threshold_sqr) {
        return false;
    }

    // Swap with last element.
    EDYN_ASSERT(manifold.num_points() > 0);
    size_t last_idx = manifold.num_points() - 1;
    manifold.point[pt_idx] = manifold.point[last_idx];
    manifold.point[last_idx] = entt::null;

    return true;
}

void destroy_contact_point(entt::registry &registry, entt::entity manifold_entity, entt::entity contact_entity) {
    registry.destroy(contact_entity);
    registry.get_or_emplace<dirty>(manifold_entity).updated<contact_manifold>();
}

void detect_collision(std::array<entt::entity, 2> body, collision_result &result,
                      const detect_collision_body_view_t &body_view, const origin_view_t &origin_view,
                      const tuple_of_shape_views_t &views_tuple) {
    auto &aabbA = body_view.get<AABB>(body[0]);
    auto &aabbB = body_view.get<AABB>(body[1]);
    const auto offset = vector3_one * -contact_breaking_threshold;

    // Only proceed to closest points calculation if AABBs intersect, since
    // a manifold is allowed to exist whilst the AABB separation is smaller
    // than `manifold.separation_threshold` which is greater than the
    // contact breaking threshold.
    if (intersect(aabbA.inset(offset), aabbB)) {
        auto &ornA = body_view.get<orientation>(body[0]);
        auto &ornB = body_view.get<orientation>(body[1]);

        auto originA = origin_view.contains(body[0]) ?
            static_cast<vector3>(origin_view.get<origin>(body[0])) :
            static_cast<vector3>(body_view.get<position>(body[0]));
        auto originB = origin_view.contains(body[1]) ?
            static_cast<vector3>(origin_view.get<origin>(body[1])) :
            static_cast<vector3>(body_view.get<position>(body[1]));

        auto shape_indexA = body_view.get<shape_index>(body[0]);
        auto shape_indexB = body_view.get<shape_index>(body[1]);
        auto ctx = collision_context{originA, ornA, aabbA, originB, ornB, aabbB, collision_threshold};

        visit_shape(shape_indexA, body[0], views_tuple, [&] (auto &&shA) {
            visit_shape(shape_indexB, body[1], views_tuple, [&] (auto &&shB) {
                collide(shA, shB, ctx, result);
            });
        });
    } else {
        result.num_points = 0;
    }
}

}
