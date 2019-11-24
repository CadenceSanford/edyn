#include "edyn/shapes/sphere_shape.hpp"

namespace edyn {

contact_manifold collide(const sphere_shape &shA, const vector3 &posA, const quaternion &ornA,
                         const sphere_shape &shB, const vector3 &posB, const quaternion &ornB) {
    auto d = posA - posB;
    auto l2 = length2(d);

    if (l2 < shA.radius * shA.radius + shB.radius * shB.radius) {
        auto l = std::sqrt(l2);
        auto dn = d / l;
        auto rA = -dn * shA.radius;
        rA = rotate(inverse(ornA), rA);
        auto rB = dn * shB.radius;
        rB = rotate(inverse(ornB), rB);

        auto manifold = contact_manifold {};
        manifold.num_points = 1;
        manifold.point[0].pivotA = rA;
        manifold.point[0].pivotB = rB;
        manifold.point[0].normalB = rotate(inverse(ornB), dn);
        return manifold;
    }

    return {};
}

}