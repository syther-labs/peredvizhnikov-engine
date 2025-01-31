/*
 *  This file is part of Peredvizhnikov Engine
 *  Copyright (C) 2023 Eduard Permyakov 
 *
 *  Peredvizhnikov Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Peredvizhnikov Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

export module ecs;

import flat_hash_map;
import bitwise_trie;
import assert;
import logger;
import meta;

import <tuple>;
import <vector>;
import <any>;
import <atomic>;
import <array>;
import <iostream>;
import <array>;
import <ranges>;
import <stack>;
import <type_traits>;

/* An implementation of an in-memory database for
 * storing and operating on large data sets. Any 
 * number of entities can be created by the user, 
 * each holding a set of components. To facilitate 
 * a good cache hit rate when iterating the components, 
 * all the components are grouped together and stored 
 * in flat sets. Furthermore, all entities having the 
 * same set of components are said to belong to an 
 * "archetype", and their component sets are further
 * grouped together. An auxiliary bitwise trie data 
 * structure is used to allow making efficing set 
 * algebra queries on the archetype set.
 */

namespace pe{

inline constexpr std::size_t kMaxComponents = 128;

export using entity_t = uint64_t;
export using component_id_t = uint64_t;
export using component_bitfield_t = __int128;

/*****************************************************************************/
/* FORWARD DECLARATIONS                                                      */
/*****************************************************************************/

export
template <typename Tag>
class World;

template <typename T>
concept CWorld = pe::is_template_instance_v<T, World>;

export 
template <typename Derived, CWorld World>
struct Entity;

template <typename T>
concept CEntity = pe::is_template_instance_v<T, Entity>;

/*****************************************************************************/
/* UNIQUE ID                                                                 */
/*****************************************************************************/
/* 
 * Use ADL friend injection technique (i.e. "stateful
 * metaprogramming") in order to statically generate a 
 * unique ID for every invocation.
 */

template <auto Id>
struct Counter
{
    using tag = Counter;

    struct generator
    {
        friend consteval auto is_defined(tag)
        { return true; }
    };
    friend consteval auto is_defined(tag);

    template <typename Tag = tag, auto = is_defined(Tag{})>
    static consteval auto exists(auto)
    { return true; }

    static consteval auto exists(...)
    { return generator(), false; }
};

template <typename Component, auto Id = uint64_t{}, auto = []{}>
inline consteval component_id_t ecs_component_id()
{
    if constexpr (Counter<Id>::exists(Id)) {
        return ecs_component_id<Component, Id + 1>();
    }else {
        return Id;
    }
}

/*****************************************************************************/
/* STRONG TYPEDEF                                                            */
/*****************************************************************************/

export
template <typename T, auto Id = []{}>
class StrongTypedef
{
public:

    template <typename U = T>
    requires (std::is_default_constructible_v<U>)
    constexpr StrongTypedef()
    noexcept(std::is_nothrow_copy_constructible_v<T>)
        : m_value()
    {}

    template <typename U = T>
    requires (std::is_copy_constructible_v<U>)
    constexpr StrongTypedef(const T& value)
    noexcept(std::is_nothrow_move_constructible_v<T>)
        : m_value{value}
    {}

    template <typename U = T>
    requires (std::is_move_constructible_v<U>)
    constexpr StrongTypedef(T&& value)
        : m_value{std::move(value)}
    {}

    constexpr operator T&() noexcept
    {
        return m_value;
    }

    constexpr operator const T&() const noexcept
    {
        return m_value;
    }

    constexpr StrongTypedef& operator=(const T& other)
    noexcept(std::is_nothrow_assignable_v<T, T>)
    {
        m_value = other;
        return *this;
    }

    constexpr StrongTypedef& operator=(const StrongTypedef& other)
    noexcept(std::is_nothrow_assignable_v<T, T>)
    {
        m_value = other.m_value;
        return *this;
    }

    constexpr std::strong_ordering operator<=>(const StrongTypedef& other) const noexcept
    {
        return (m_value <=> other.m_value);
    }

    constexpr friend bool operator==(const StrongTypedef& lhs, const StrongTypedef& rhs)
    {
        return (lhs.m_value == rhs.m_value);
    }

    friend void swap(StrongTypedef& a, StrongTypedef& b) noexcept
    {
        using std::swap;
        swap(static_cast<T&>(a), static_cast<T&>(b));
    }

    friend std::ostream& operator<<(std::ostream& stream, const StrongTypedef& obj) 
    {
        return stream << obj.m_value;
    }

private:

    T m_value;
};

/*****************************************************************************/
/* COLUMN SET ITERATOR                                                       */
/*****************************************************************************/

template <typename... Components>
class ColumnSetIterator
{
private:

    template <typename Component>
    using ComponentMap = FlatHashMap<entity_t, Component>;

    template <typename Component>
    using ComponentMapIterator = typename ComponentMap<Component>::iterator;

    std::tuple<ComponentMapIterator<Components>...> m_columns;

public:

    ColumnSetIterator() = default;

    ColumnSetIterator(ComponentMapIterator<Components>... iterators)
        : m_columns{iterators...}
    {}

    inline auto operator*() const
    {
        entity_t eid = (*std::get<0>(m_columns)).first;
        return std::make_tuple(
            eid,
            (std::ref((*std::get<ComponentMapIterator<Components>>(m_columns)).second)) ...
        );
    }

    inline ColumnSetIterator& operator++()
    {
        ((++std::get<ComponentMapIterator<Components>>(m_columns)), ...);
        return *this;
    }

    inline ColumnSetIterator operator++(int)
    {
        ColumnSetIterator ret = *this;
        ++(*this);
        return ret;
    }

    friend bool operator==(const ColumnSetIterator& a, const ColumnSetIterator& b)
    {
        return (... && (std::get<ComponentMapIterator<Components>>(a.m_columns)
                    ==  std::get<ComponentMapIterator<Components>>(b.m_columns)));
    }

    friend bool operator!=(const ColumnSetIterator& a, const ColumnSetIterator& b)
    {
        return !(a == b);
    }
};

/*****************************************************************************/
/* COMPONENT DEFAULT                                                         */
/*****************************************************************************/
/*
 * Type trait to allow specifying a constexpr default value for a constructor.
 */

export
template <typename Entity, typename Component>
struct Default
{};

/*****************************************************************************/
/* ARCHETYPE                                                                 */
/*****************************************************************************/

template <typename Component>
struct component_traits;

template <typename Tuple>
struct Archetype;

template <typename... Components>
struct Archetype<std::tuple<Components...>>
{
    std::tuple<FlatHashMap<entity_t, Components>...> m_columns;

    Archetype() = default;
    Archetype(std::size_t default_size)
        : m_columns{FlatHashMap<entity_t, Components>{default_size}...}
    {}

    template <typename Component>
    auto& Get(entity_t id)
    {
        auto& map = std::get<FlatHashMap<entity_t, Component>>(m_columns);
        return map[id];
    }

    template <typename Component>
    void Set(entity_t id, Component&& value)
    {
        using component = std::remove_cvref_t<Component>;
        auto& map = std::get<FlatHashMap<entity_t, component>>(m_columns);
        map[id] = std::forward<Component>(value);
    }

    template <typename Component>
    void Clear(entity_t id)
    {
        auto& map = std::get<FlatHashMap<entity_t, Component>>(m_columns);
        map.erase(id);
    }

    template <typename Component>
    auto begin()
    {
        auto& map = std::get<FlatHashMap<entity_t, Component>>(m_columns);
        return map.begin();
    }

    template <typename Component>
    auto end()
    {
        auto& map = std::get<FlatHashMap<entity_t, Component>>(m_columns);
        return map.end();
    }
};

struct ComponentDispatchTable
{
    using Getter = void*(*)(std::any*, entity_t);
    using Copyer = void (*)(std::any*, entity_t, void*);
    using Mover  = void (*)(std::any*, entity_t, void*);
    using Eraser = void (*)(std::any*, entity_t);

    using BeginGetter = std::any(*)(std::any*);
    using EndGetter   = std::any(*)(std::any*);

    std::array<Getter,      kMaxComponents> m_getters;
    std::array<Copyer,      kMaxComponents> m_copyers;
    std::array<Mover,       kMaxComponents> m_movers;
    std::array<Eraser,      kMaxComponents> m_erasers;
    std::array<BeginGetter, kMaxComponents> m_begin_getters;
    std::array<EndGetter,   kMaxComponents> m_end_getters;

    ComponentDispatchTable() = default;

    template <typename... Components>
    ComponentDispatchTable(std::type_identity<std::tuple<Components...>>)
    {
        ((m_getters[component_traits<Components>::id] = +[](std::any *arch, entity_t id){

            using archetype_type = Archetype<std::tuple<Components...>>;
            using component_type = Components;

            auto *archetype = any_cast<archetype_type>(arch);
            auto& ret = archetype->template Get<component_type>(id);
            return static_cast<void*>(&ret);

        }), ...);

        ((m_copyers[component_traits<Components>::id] = +[](std::any *arch, entity_t id, void *val){

            using archetype_type = Archetype<std::tuple<Components...>>;
            using component_type = Components;

            auto *archetype = any_cast<archetype_type>(arch);
            const auto& value = *static_cast<component_type*>(val);
            archetype->template Set(id, value);

        }), ...);

        ((m_movers[component_traits<Components>::id] = +[](std::any *arch, entity_t id, void *val){

            using archetype_type = Archetype<std::tuple<Components...>>;
            using component_type = Components;

            auto *archetype = any_cast<archetype_type>(arch);
            auto&& value = *static_cast<component_type*>(val);
            archetype->template Set(id, std::move(value));

        }), ...);

        ((m_erasers[component_traits<Components>::id] = +[](std::any *arch, entity_t id){

            using archetype_type = Archetype<std::tuple<Components...>>;
            using component_type = Components;

            auto archetype = any_cast<archetype_type>(arch);
            archetype->template Clear<component_type>(id);

        }), ...);

        ((m_begin_getters[component_traits<Components>::id] = +[](std::any *arch){

            using archetype_type = Archetype<std::tuple<Components...>>;
            using component_type = Components;

            auto archetype = any_cast<archetype_type>(arch);
            return std::any{archetype->template begin<component_type>()};

        }), ...);

        ((m_end_getters[component_traits<Components>::id] = +[](std::any *arch){

            using archetype_type = Archetype<std::tuple<Components...>>;
            using component_type = Components;

            auto archetype = any_cast<archetype_type>(arch);
            return std::any{archetype->template end<component_type>()};

        }), ...);
    }

    inline void *Get(std::any *archetype, entity_t eid, component_id_t cid) const
    {
        return m_getters[cid](archetype, eid);
    }

    inline void Copy(std::any *archetype, entity_t eid, component_id_t cid, void *val) const
    {
        m_copyers[cid](archetype, eid, val);
    }

    inline void Move(std::any *archetype, entity_t eid, component_id_t cid, void *val) const
    {
        m_movers[cid](archetype, eid, val);
    }

    inline void Clear(std::any *archetype, entity_t eid, component_id_t cid) const
    {
        m_erasers[cid](archetype, eid);
    }

    inline auto Begin(std::any *archetype, component_id_t cid) const
    {
        return m_begin_getters[cid](archetype);
    }

    inline auto End(std::any *archetype, component_id_t cid) const
    {
        return m_end_getters[cid](archetype);
    }
};

struct TypeErasedArchetype
{
    std::any               m_archetype;
    ComponentDispatchTable m_dispatch_table;

    struct tuple_tag{};

    TypeErasedArchetype() = default;

    template <typename... Components>
    TypeErasedArchetype(tuple_tag, Archetype<std::tuple<Components...>>&& archetype)
        : m_archetype{std::move(archetype)}
        , m_dispatch_table{std::type_identity<std::tuple<Components...>>{}}
    {}

    template <typename Tuple>
    TypeErasedArchetype(Archetype<Tuple>&& archetype)
        : TypeErasedArchetype{tuple_tag{}, std::forward<Archetype<Tuple>>(archetype)}
    {}

    template <typename Component>
    Component& Get(entity_t entity)
    {
        constexpr component_id_t component = component_traits<Component>::id;
        void *ret = m_dispatch_table.Get(&m_archetype, entity, component);
        return *static_cast<Component*>(ret);
    }

    template <typename Component>
    void Set(entity_t entity, Component&& value)
    {
        using component_type = std::remove_cvref_t<Component>;
        constexpr component_id_t component = component_traits<component_type>::id;
        void *ptr = static_cast<void*>(&value);
        if constexpr (std::is_rvalue_reference_v<Component>) {
            m_dispatch_table.Move(&m_archetype, entity, component, ptr);
        }else{
            m_dispatch_table.Copy(&m_archetype, entity, component, ptr);
        }
    }

    template <typename Component>
    void Clear(entity_t entity)
    {
        constexpr component_id_t component = component_traits<Component>::id;
        m_dispatch_table.Clear(&m_archetype, entity, component);
    }

    template <typename... Components>
    ColumnSetIterator<Components...> begin()
    {
        return ColumnSetIterator<Components...>{
            any_cast<typename FlatHashMap<entity_t, Components>::iterator>(
                m_dispatch_table.Begin(&m_archetype, component_traits<Components>::id))...
        };
    }

    template <typename... Components>
    ColumnSetIterator<Components...> end()
    {
        return ColumnSetIterator<Components...>{
            any_cast<typename FlatHashMap<entity_t, Components>::iterator>(
                m_dispatch_table.End(&m_archetype, component_traits<Components>::id))...
        };
    }
};

template <typename Tuple>
struct drop_row;

template <typename... Components>
struct drop_row<std::tuple<Components...>>
{
    constexpr auto operator()(TypeErasedArchetype& archetype, entity_t eid)
    {
        ((archetype.template Clear<Components>(eid)), ...);
    };
};

template <CEntity Entity, typename Tuple>
struct add_row;

template <CEntity Entity, typename... Components>
struct add_row<Entity, std::tuple<Components...>>
{
    template <typename Component>
    constexpr inline void set_cell(TypeErasedArchetype& archetype, entity_t eid)
    {
        using derived = typename Entity::derived_type;
        using component_ref = std::add_lvalue_reference_t<Component>;

        constexpr bool has_defaults = requires{
            {Default<derived, Component>::value} -> std::convertible_to<Component>;
        };
        if constexpr(has_defaults) {
            auto& ref = const_cast<component_ref>(Default<derived, Component>::value);
            archetype.template Set(eid, ref);
        }else{
            archetype.template Set<Component>(eid, {});
        }
    }

    constexpr inline auto operator()(TypeErasedArchetype& archetype, entity_t eid)
    {
        (set_cell<Components>(archetype, eid), ...);
    };
};

/*****************************************************************************/
/* WORLD                                                                     */
/*****************************************************************************/

export
struct DefaultWorldTag {};

export
template <typename Tag = DefaultWorldTag>
class World
{
private:

    using ComponentTrieType = BitwiseTrie<component_bitfield_t>;
    using ArchetypeMapType = FlatHashMap<component_bitfield_t, TypeErasedArchetype>;
    using EntityArchetypeMap = FlatHashMap<entity_t, component_bitfield_t>;

    static inline std::atomic_uint64_t s_next_entity_id{0};
    static inline ComponentTrieType    s_component_trie{};
    static inline ArchetypeMapType     s_component_archetype_map{};
    static inline EntityArchetypeMap   s_entity_archetype_map{};

    template <typename Derived, CWorld World>
    friend struct Entity;

    template <CWorld World, typename... Components>
    friend class components_view;

public:

    static entity_t AllocateID() 
    { 
        return s_next_entity_id.fetch_add(1, std::memory_order_relaxed); 
    };

    static void Register(const CEntity auto& entity, component_bitfield_t components);
    static void Unregister(const CEntity auto& entity);
};

/*****************************************************************************/
/* COMPONENTS VIEW                                                           */
/*****************************************************************************/
/*
 * Allows iterating all components in a set.
 */

export
template <CWorld World, typename... Components>
class components_view;

/*****************************************************************************/
/* ENTITY                                                                    */
/*****************************************************************************/

template <CEntity Entity, typename Component>
constexpr bool has_component();

export 
template <typename Derived, CWorld World = pe::World<DefaultWorldTag>>
struct Entity
{
    using world_type = World;
    using derived_type = Derived;

    const uint64_t m_id{world_type::AllocateID()};

    Entity();
    ~Entity();

    Entity(Entity&&) = default;
    Entity& operator=(Entity&&) = default;

    /* Non-copyable since the Entity object 
     * holds ownership of the component state.
     */
    Entity(Entity const&) = delete;
    Entity& operator=(Entity const&) = delete;

    template <typename Component>
    Component Get()
    requires (has_component<Entity, Component>());

    template <typename Component>
    void Set(Component&& value)
    requires (has_component<Entity, Component>());

    template <typename Component>
    bool HasComponent();
};

/*****************************************************************************/
/* COMPONENT                                                                 */
/*****************************************************************************/

template <typename Component>
struct component_traits
{
    static constexpr component_id_t id = ecs_component_id<Component>();
    static_assert(id < kMaxComponents, "Exceeded maximum number of components!");
};

template <typename Tuple>
struct component_bitfield
{
    template <typename... Args>
    struct helper;

    template <>
    struct helper<>
    {
        static constexpr auto value = component_bitfield_t{};
    };

    template <typename Head, typename... Tail>
    struct helper<Head, Tail...>
    {
        static constexpr auto bits()
        {
            using component_type = Head;
            constexpr auto id = component_traits<component_type>::id;
            return component_bitfield_t{1} << id | helper<Tail...>::value;
        }

        static constexpr auto value = bits();
    };

    template <typename TupleType>
    struct unpack;

    template <typename... Args>
    struct unpack<std::tuple<Args...>>
    {
        static constexpr auto value = helper<Args...>::value;
    };

    static constexpr auto value()
    {
        return unpack<Tuple>::value;
    }
};

template <typename Tuple>
inline consteval component_bitfield_t ecs_component_mask()
{
    return component_bitfield<Tuple>::value();
}

template <typename Tuple>
struct components_from_bases
{
    using type = decltype(transform_tuple(std::declval<Tuple>(), []<typename T>() constexpr{
        using with_components_type = typename std::remove_cvref_t<T>::type;
        using component_type = typename with_components_type::type;
        return std::declval<component_type>();
    }));
};

template <typename Tuple>
using components_from_bases_t = typename components_from_bases<Tuple>::type;

template <CEntity Entity, typename Component>
constexpr bool has_component()
{
    using bases_type = base_list_t<typename Entity::derived_type>;
    using components_type = components_from_bases_t<bases_type>;
    return contains_type_v<std::remove_cvref_t<Component>, components_type>;
}

/*****************************************************************************/
/* WITH COMPONENT                                                            */
/*****************************************************************************/
/*
 * Mixin base to add components to an entity...
 */

export
template <typename Derived, typename Component>
struct WithComponent : Base<WithComponent<Derived, Component>>
{
    using type = Component;
};

/*****************************************************************************/
/* INHERIT COMPONENTS                                                        */
/*****************************************************************************/
/*
 * Mixin base to inherit all components from another entity type.
 */

template <typename... Args>
struct InheritAll;

template <typename... Args>
struct InheritAll<std::tuple<Args...>> : public Args...
{};

template <typename Derived, typename Tuple>
struct transform_components
{
    using type = decltype(transform_tuple(std::declval<Tuple>(), []<typename T>() constexpr{
        using with_components_type = typename std::remove_cvref_t<T>::type;
        using component_type = typename with_components_type::type;
        return std::declval<WithComponent<Derived, component_type>>();
    }));
};

template <typename Derived, typename Tuple>
using transform_components_t = typename transform_components<Derived, Tuple>::type;

export
template <typename Derived, typename Base>
struct InheritComponents : public InheritAll<transform_components_t<Derived, base_list_t<Base>>>
{};

/*****************************************************************************/
/* MODULE IMPLEMENTATION                                                     */
/*****************************************************************************/

template <typename Derived, CWorld World>
Entity<Derived, World>::Entity()
{
    using bases_type = base_list_t<derived_type>;
    using components_type = components_from_bases_t<bases_type>;
    constexpr auto bitmap = ecs_component_mask<components_type>();
    world_type::Register(*this, bitmap);
}

template <typename Derived, CWorld World>
Entity<Derived, World>::~Entity()
{
    world_type::Unregister(*this);
}

template <typename Derived, CWorld World>
template <typename Component>
Component Entity<Derived, World>::Get()
requires (has_component<Entity, Component>())
{
    component_bitfield_t components = world_type::s_entity_archetype_map[m_id];
    auto& archetype = world_type::s_component_archetype_map[components];
    return archetype.template Get<Component>(m_id);
}

template <typename Derived, CWorld World>
template <typename Component>
void Entity<Derived, World>::Set(Component&& value)
requires (has_component<Entity, Component>())
{
    component_bitfield_t components = world_type::s_entity_archetype_map[m_id];
    auto& archetype = world_type::s_component_archetype_map[components];
    archetype.template Set<Component>(m_id, std::forward<Component>(value));
}

template <typename Derived, CWorld World>
template <typename Component>
bool Entity<Derived, World>::HasComponent()
{
    component_bitfield_t components = world_type::s_entity_archetype_map[m_id];
    auto mask = component_bitfield_t(1) << component_traits<Component>::id;
    return ((components & mask) == mask);
}

template <typename Tag>
void World<Tag>::Register(const CEntity auto& entity, component_bitfield_t components)
{
    using entity_type = std::remove_cvref_t<decltype(entity)>;
    using bases_type = base_list_t<typename entity_type::derived_type>;
    using components_type = components_from_bases_t<bases_type>;

    auto end = s_component_archetype_map.end();
    if(auto it = s_component_archetype_map.find(components); it == end) {

        /* Create a new archetype */
        s_component_archetype_map.insert(std::make_pair(components, 
            TypeErasedArchetype{Archetype<components_type>{}}));
        s_component_trie.Insert(components);
    }

    s_entity_archetype_map.insert(std::make_pair(entity.m_id, components));
    add_row<entity_type, components_type>{}(s_component_archetype_map[components], entity.m_id);
}

template <typename Tag>
void World<Tag>::Unregister(const CEntity auto& entity)
{
    using entity_type = std::remove_cvref_t<decltype(entity)>;
    using bases_type = base_list_t<typename entity_type::derived_type>;
    using components_type = components_from_bases_t<bases_type>;

    component_bitfield_t components = s_entity_archetype_map[entity.m_id];
    auto& archetype = s_component_archetype_map[components];
    entity_t eid = entity.m_id;
    drop_row<components_type>{}(archetype, eid);
}

export
template <CWorld World, typename... Components>
class components_view : public std::ranges::view_interface<components_view<World, Components...>>
{
private:

    using world_type = World;
    using components_type = std::tuple<Components...>;
    using components_ref_type = decltype(transform_tuple(std::declval<components_type>(),
        []<typename T>() constexpr -> std::add_lvalue_reference_t<T> {
            return std::declval<std::add_lvalue_reference_t<T>>();
        }));
    using components_ptr_type = decltype(transform_tuple(std::declval<components_type>(),
        []<typename T>() constexpr{
            return std::declval<std::add_pointer<T>>();
        }));

    using ArchetypeView = trie_view_match_mask<
        component_bitfield_t, trie_view<component_bitfield_t>>;
    using ArchetypeViewIterator = typename ArchetypeView::iterator;

    class Iterator
    {
    public:

        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = decltype(std::tuple_cat(std::declval<std::tuple<entity_t>>(),
            std::declval<components_ref_type>()));
        using pointer = decltype(std::tuple_cat(std::declval<std::tuple<entity_t>>(),
            std::declval<components_ptr_type>()));
        using reference = const value_type;

    private:

        struct GeneratorContext
        {
            enum class Stage
            {
                eBeginArchetype,
                eNextArchetype,
                eNextRow,
                eFinished
            };
            Stage                            m_stage;
            ArchetypeViewIterator            m_curr_archetype_it;
            ArchetypeViewIterator            m_sentinel_archetype_it;
            ColumnSetIterator<Components...> m_curr_row;
            ColumnSetIterator<Components...> m_sentinel_row;

            friend auto operator<=>(const GeneratorContext&, const GeneratorContext&) = default;
        };

        GeneratorContext m_ctx;

        void next_row()
        {
            switch(m_ctx.m_stage) {
            case GeneratorContext::Stage::eNextArchetype:
                ++m_ctx.m_curr_archetype_it;
                [[fallthrough]];

            case GeneratorContext::Stage::eBeginArchetype: {
                if(m_ctx.m_curr_archetype_it == m_ctx.m_sentinel_archetype_it) {
                    m_ctx.m_stage = GeneratorContext::Stage::eFinished;
                    break;
                }
                auto& arch = world_type::s_component_archetype_map[*m_ctx.m_curr_archetype_it];
                m_ctx.m_curr_row = arch.template begin<Components...>();
                m_ctx.m_sentinel_row = arch.template end<Components...>();
                if(m_ctx.m_curr_row == m_ctx.m_sentinel_row) {
                    m_ctx.m_stage = GeneratorContext::Stage::eNextArchetype;
                    return next_row();
                }
                m_ctx.m_stage = GeneratorContext::Stage::eNextRow;
                break;
            }
            case GeneratorContext::Stage::eNextRow:
                ++m_ctx.m_curr_row;
                if(m_ctx.m_curr_row == m_ctx.m_sentinel_row) {
                    m_ctx.m_stage = GeneratorContext::Stage::eNextArchetype;
                    return next_row();
                }
                /* m_curr_row now points to the next row */
                break;

            case GeneratorContext::Stage::eFinished:
                pe::assert(false);
                break;
            }
        }

    public:

        Iterator() = default;

        Iterator(ArchetypeViewIterator begin, ArchetypeViewIterator end)
            : m_ctx{
                .m_stage = GeneratorContext::Stage::eBeginArchetype,
                .m_curr_archetype_it = begin,
                .m_sentinel_archetype_it = end
            }
        {
            next_row();
        }

        value_type operator*() const
        {
            return *m_ctx.m_curr_row;
        }

        Iterator& operator++()
        {
            next_row();
            return *this;
        }

        Iterator operator++(int)
        {
            Iterator ret = *this;
            ++(*this);
            return ret;
        }

        friend bool operator==(const Iterator& a, const Iterator& b)
        {
            if(a.m_ctx.m_stage == GeneratorContext::Stage::eFinished
            && b.m_ctx.m_stage == GeneratorContext::Stage::eFinished)
                return true;
            return (a.m_ctx == b.m_ctx);
        }

        friend bool operator!=(const Iterator& a, const Iterator& b)
        {
            return !(a == b);
        }
    };

    ArchetypeView m_arch_view;

public:

    components_view()
        : m_arch_view{trie_view(world_type::s_component_trie), 
            ecs_component_mask<components_type>()}
    {}

    auto begin() const { return Iterator(m_arch_view.begin(), m_arch_view.end()); }
    auto end()   const { return Iterator(m_arch_view.end(),   m_arch_view.end()); }
};

} // namespace pe

