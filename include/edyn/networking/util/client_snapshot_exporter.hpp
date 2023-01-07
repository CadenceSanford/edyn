#ifndef EDYN_NETWORKING_UTIL_CLIENT_SNAPSHOT_EXPORTER_HPP
#define EDYN_NETWORKING_UTIL_CLIENT_SNAPSHOT_EXPORTER_HPP

#include <entt/entity/fwd.hpp>
#include <type_traits>
#include "edyn/comp/action_list.hpp"
#include "edyn/comp/graph_edge.hpp"
#include "edyn/comp/graph_node.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/core/entity_graph.hpp"
#include "edyn/networking/comp/action_history.hpp"
#include "edyn/networking/comp/network_input.hpp"
#include "edyn/networking/comp/networked_comp.hpp"
#include "edyn/networking/packet/registry_snapshot.hpp"
#include "edyn/networking/util/component_index_type.hpp"
#include "edyn/serialization/memory_archive.hpp"
#include "edyn/util/island_util.hpp"
#include "edyn/util/tuple_util.hpp"

namespace edyn {

class client_snapshot_exporter {
public:
    client_snapshot_exporter(entt::registry &registry)
        : m_registry(&registry)
    {}

    virtual ~client_snapshot_exporter() = default;

    // Write all networked entities and components into a snapshot.
    virtual void export_all(packet::registry_snapshot &snap, const entt::sparse_set &entities) const = 0;
    virtual void export_all(packet::registry_snapshot &snap, const std::vector<entt::entity> &entities) const = 0;

    // Write all modified networked entities and components into a snapshot.
    virtual void export_modified(packet::registry_snapshot &snap, entt::entity client_entity,
                                 const entt::sparse_set &owned_entities, bool allow_full_ownership) const = 0;

    void append_current_actions(double time) {
        if (m_append_current_actions_func != nullptr) {
            (*m_append_current_actions_func)(*m_registry, time);
        }
    }

    virtual void update(double time) = 0;

    void set_observer_enabled(bool enabled) {
        m_observer_enabled = enabled;
    }

    template<typename Component>
    component_index_type get_component_index() const {
        auto id = entt::type_index<Component>();
        return m_component_indices.at(id);
    }

protected:
    entt::registry *m_registry;
    bool m_observer_enabled {true};

    using append_current_actions_func_t = void(entt::registry &registry, double time);
    append_current_actions_func_t *m_append_current_actions_func {nullptr};

    std::map<entt::id_type, component_index_type> m_component_indices;
};

template<typename... Components>
class client_snapshot_exporter_impl : public client_snapshot_exporter {

    struct modified_components {
        struct comp_index_time {
            uint16_t index;
            uint16_t remaining;
        };

        std::array<comp_index_time, sizeof...(Components)> entry {};
        uint16_t count {};

        bool empty() const {
            for (unsigned i = 0; i < count; ++i) {
                if (entry[i].remaining > 0) {
                    return false;
                }
            }
            return true;
        }
    };

    template<typename Action>
    static void append_actions(entt::registry &registry, unsigned index, double time) {
        auto view = registry.view<action_list<Action>, action_history>();

        for (auto [entity, list, history] : view.each()) {
            if (list.actions.empty()) {
                continue;
            }

            auto data = std::vector<uint8_t>{};
            auto archive = memory_output_archive(data);
            archive(list);
            history.entries.emplace_back(time, index, std::move(data));
        }
    }

    template<typename... Actions>
    static void append_current_actions(entt::registry &registry, double time) {
        unsigned index = 0;
        (append_actions<Actions>(registry, index++, time), ...);
    }

    template<typename Component>
    void on_update(entt::registry &registry, entt::entity entity) {
        if (!m_observer_enabled) {
            return;
        }

        if (auto *modified = registry.try_get<modified_components>(entity)) {
            static const auto index = index_of_v<unsigned, Component, Components...>;
            bool found = false;
            const uint16_t time_remaining = 400;

            for (unsigned i = 0; i < modified->count; ++i) {
                if (modified->entry[i].index == index) {
                    modified->entry[i].remaining = time_remaining;
                    found = true;
                }
            }

            if (!found) {
                EDYN_ASSERT(modified->count < sizeof...(Components));
                modified->entry[modified->count++] = {index, time_remaining};
            }
        }
    }

    // Only add _on update_ observer to non-empty components.
    template<typename Component>
    void observe_update(entt::registry &registry) {
        if constexpr(!std::is_empty_v<Component>) {
            m_connections.push_back(registry.on_update<Component>().template connect<&client_snapshot_exporter_impl<Components...>::template on_update<Component>>(*this));
        }
    }

public:
    template<typename... Actions>
    client_snapshot_exporter_impl(entt::registry &registry,
                                  [[maybe_unused]] std::tuple<Components...>,
                                  [[maybe_unused]] std::tuple<Actions...>)
        : client_snapshot_exporter(registry)
    {
        m_connections.push_back(registry.on_construct<networked_tag>().connect<&entt::registry::emplace<modified_components>>());
        (observe_update<Components>(registry), ...);

        if constexpr(sizeof...(Actions) > 0) {
            m_append_current_actions_func = &append_current_actions<Actions...>;
        }

        auto i = component_index_type{};
        (m_component_indices.emplace(entt::type_index<Components>(), i++), ...);
    }

    template<typename It>
    void export_all(packet::registry_snapshot &snap, It first, It last) const {
        for (; first != last; ++first) {
            auto entity = *first;
            unsigned i = 0;
            (((m_registry->all_of<Components>(entity) ?
                internal::snapshot_insert_entity<Components>(*m_registry, entity, snap, i) : void(0)), ++i), ...);
        }
    }

    void export_all(packet::registry_snapshot &snap, const std::vector<entt::entity> &entities) const override {
        export_all(snap, entities.begin(), entities.end());
    }

    void export_all(packet::registry_snapshot &snap, const entt::sparse_set &entities) const override {
        export_all(snap, entities.begin(), entities.end());
    }

    void export_modified(packet::registry_snapshot &snap, entt::entity client_entity,
                         const entt::sparse_set &owned_entities, bool allow_full_ownership) const override {
        auto &registry = *m_registry;
        auto modified_view = registry.view<const modified_components>();
        auto sleeping_view = registry.view<sleeping_tag>();
        static const auto components_tuple = std::tuple<Components...>{};

        if (allow_full_ownership) {
            // Include all networked entities in the islands that contain an entity
            // owned by this client, excluding entities that are owned by other clients.
            auto owner_view = registry.view<entity_owner>();
            auto node_view = registry.view<graph_node>();
            auto edge_view = registry.view<graph_edge>();
            auto body_view = registry.view<position, orientation, linvel, angvel, dynamic_tag>();
            auto &graph = registry.ctx().at<entity_graph>();

            // Collect nodes to visit in entity graph from owned nodes and edges.
            auto to_visit = entt::sparse_set{};

            for (auto entity : owned_entities) {
                if (sleeping_view.contains(entity)) {
                    continue;
                }

                if (edge_view.contains(entity)) {
                    auto [edge] = edge_view.get(entity);
                    auto edge_node_entity = graph.edge_node_entities(edge.edge_index).first;

                    if (!to_visit.contains(edge_node_entity)) {
                        to_visit.emplace(edge_node_entity);
                    }
                } else if (node_view.contains(entity) && !to_visit.contains(entity)) {
                    EDYN_ASSERT(node_view.contains(entity));
                    to_visit.emplace(entity);
                }
            }

            // Visit each node in the graph and collect all nodes that are reachable
            // from it. Remove then from the `to_visit` list to avoid traversing
            // the same connected component.
            std::vector<entt::entity> entities_export;

            while (!to_visit.empty()) {
                auto [node] = node_view.get(*to_visit.begin());
                bool client_reachable = false;
                entities_export.clear();

                graph.traverse(node.node_index, [&](auto node_index) {
                    auto node_entity = graph.node_entity(node_index);
                    entities_export.push_back(node_entity);

                    if (to_visit.contains(node_entity)) {
                        to_visit.remove(node_entity);
                    }

                    if (owner_view.contains(node_entity)) {
                        auto [owner] = owner_view.get(node_entity);

                        if (owner.client_entity == client_entity) {
                            client_reachable = true;
                        }
                    }
                });

                if (client_reachable) {
                    // Client is present in this connected component, thus export
                    // entities and components.
                    for (auto entity : entities_export) {
                        auto is_owned_by_another_client =
                            owner_view.contains(entity) &&
                            std::get<0>(owner_view.get(entity)).client_entity != client_entity;

                        // Do not send state of entities owned by other client.
                        if (is_owned_by_another_client) {
                            continue;
                        }

                        if (modified_view.contains(entity)) {
                            auto [modified] = modified_view.get(entity);

                            for (unsigned i = 0; i < modified.count; ++i) {
                                auto comp_index = modified.entry[i].index;
                                visit_tuple(components_tuple, comp_index, [&](auto &&c) {
                                    using CompType = std::decay_t<decltype(c)>;
                                    internal::snapshot_insert_entity<CompType>(registry, entity, snap, comp_index);
                                });
                            }
                        }

                        if (body_view.contains(entity)) {
                            internal::snapshot_insert_entity<position   >(registry, entity, snap, index_of_v<unsigned, position,    Components...>);
                            internal::snapshot_insert_entity<orientation>(registry, entity, snap, index_of_v<unsigned, orientation, Components...>);
                            internal::snapshot_insert_entity<linvel     >(registry, entity, snap, index_of_v<unsigned, linvel,      Components...>);
                            internal::snapshot_insert_entity<angvel     >(registry, entity, snap, index_of_v<unsigned, angvel,      Components...>);
                        }
                    }
                }
            }
        } else {
            // Otherwise, only entities owned by this client which contain an
            // input component are included.
            for (auto entity : owned_entities) {
                if (sleeping_view.contains(entity)) {
                    continue;
                }

                auto [modified] = modified_view.get(entity);

                for (unsigned i = 0; i < modified.count; ++i) {
                    auto comp_index = modified.entry[i].index;
                    visit_tuple(components_tuple, comp_index, [&](auto &&c) {
                        using CompType = std::decay_t<decltype(c)>;
                        if constexpr(std::is_base_of_v<network_input, CompType>) {
                            internal::snapshot_insert_entity<CompType>(registry, entity, snap, comp_index);
                        }
                    });
                }
            }
        }

        // Always include actions.
        auto history_view = registry.view<action_history>();
        static const auto action_history_index = index_of_v<unsigned, action_history, Components...>;

        for (auto entity : owned_entities) {
            if (history_view.contains(entity) && !std::get<0>(history_view.get(entity)).entries.empty()) {
                internal::snapshot_insert_entity<action_history>(registry, entity, snap, action_history_index);
            }
        }
    }

    void update(double time) override {
        EDYN_ASSERT(!(time < m_last_time));
        auto elapsed_ms = static_cast<unsigned>((time - m_last_time) * 1000u);
        m_last_time = time;

        m_registry->view<modified_components>().each([&](modified_components &modified) {
            for (unsigned i = 0; i < modified.count;) {
                auto &entry = modified.entry[i];
                if (elapsed_ms > entry.remaining) {
                    // Assign value of last and decrement count.
                    // Note that `i` isn't incremented in this case.
                    entry = modified.entry[--modified.count];
                } else {
                    entry.remaining -= elapsed_ms;
                    ++i;
                }
            }
        });
    }

private:
    std::vector<entt::scoped_connection> m_connections;
    double m_last_time {};
};

}

#endif // EDYN_NETWORKING_UTIL_CLIENT_SNAPSHOT_EXPORTER_HPP
