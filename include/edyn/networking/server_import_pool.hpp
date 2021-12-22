#ifndef EDYN_NETWORKING_SERVER_IMPORT_POOL_HPP
#define EDYN_NETWORKING_SERVER_IMPORT_POOL_HPP

#include <entt/entity/registry.hpp>
#include "edyn/comp/dirty.hpp"
#include "edyn/edyn.hpp"
#include "edyn/networking/remote_client.hpp"
#include "edyn/parallel/merge/merge_component.hpp"

namespace edyn {

bool is_fully_owned_by_client(const entt::registry &, entt::entity client_entity, entt::entity entity);

template<typename Component>
void import_pool_server(entt::registry &registry, entt::entity client_entity,
                        const std::vector<std::pair<entt::entity, Component>> &pool) {
    auto &client = registry.get<remote_client>(client_entity);
    auto merge_ctx = merge_context{&registry, &client.entity_map};

    for (auto &pair : pool) {
        auto remote_entity = pair.first;

        if (!client.entity_map.has_rem(pair.first)) {
            continue;
        }

        auto local_entity = client.entity_map.remloc(remote_entity);

        if (!registry.valid(local_entity)) {
            client.entity_map.erase_loc(local_entity);
            continue;
        }

        // Do not apply this update if this is a dynamic entity which is not
        // fully owned by this client.
        if (registry.any_of<dynamic_tag>(local_entity) && !is_fully_owned_by_client(registry, client_entity, local_entity)) {
            continue;
        }

        if constexpr(std::is_empty_v<Component>) {
            if (!registry.any_of<Component>(local_entity)) {
                registry.emplace<Component>(local_entity);
                registry.emplace_or_replace<dirty>(local_entity).template created<Component>();
            }
        } else {
            auto comp = pair.second;
            merge(static_cast<Component *>(nullptr), comp, merge_ctx);

            if (registry.any_of<Component>(local_entity)) {
                registry.replace<Component>(local_entity, comp);
                refresh<Component>(registry, local_entity);
            } else {
                registry.emplace<Component>(local_entity, comp);
                registry.emplace_or_replace<dirty>(local_entity).template created<Component>();
            }
        }
    }
}

template<typename... Component>
void import_pool_server(entt::registry &registry, entt::entity client_entity, const pool_snapshot &pool, const std::tuple<Component...> &all_components) {
    visit_tuple(all_components, pool.component_index, [&] (auto &&c) {
        using CompType = std::decay_t<decltype(c)>;
        import_pool_server(registry, client_entity, std::static_pointer_cast<pool_snapshot_data<CompType>>(pool.ptr)->pairs);
    });
}

inline void import_pool_server_default(entt::registry &registry, entt::entity client_entity, const pool_snapshot &pool) {
    import_pool_server(registry, client_entity, pool, networked_components);
}

}

#endif // EDYN_NETWORKING_SERVER_IMPORT_POOL_HPP
