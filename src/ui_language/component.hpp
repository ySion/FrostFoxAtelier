#pragma once

#include "ui_language/declaration.hpp"
#include "source/program.hpp"
#include "ui/controls.hpp"

namespace atelier::ui_language {
namespace detail { struct InstanceData; }

// A sampled edit reference: field keys are owned by the host's existing typed
// property/transaction routes. Neither the component nor this value retains Actors.
struct EditBinding {
    ui::ControlKey group{};
    bool enabled{}, modified{};
    std::vector<ui::NumberField> fields;
    bool operator==(const EditBinding&) const = default;
};
using InputValue=std::variant<bool,double,EditBinding>;
struct KeyBinding { std::string_view node; ui::ControlKey key{}, label{}, body{}; };
struct Statistics {
    std::uint64_t binding_evaluations{}, event_evaluations{}, publications{}, node_creations{}, node_removals{};
};

// Call during the host Runtime's existing bootstrap, before its first object.
Result<TypeId> register_types(Runtime&);

// One compiled Source/JIT module per component type in this Runtime. Instances
// share code and metadata; close all instances/components before the Runtime.
class Component {
public:
    static Result<std::shared_ptr<Component>> compile(Runtime&,std::string_view name,std::string_view source);
    ~Component();
    const Declaration& declaration() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Component(std::unique_ptr<Impl>);
    friend struct detail::InstanceData;
    friend class Instance;
};

// Single-threaded UI scope. publish() is the explicit external-data boundary;
// it only invalidates bindings that actually read changed inputs. update()
// drains one batch before layout. It never runs World/editor commands inside JIT.
class Instance {
public:
    static Result<std::unique_ptr<Instance>> create(std::shared_ptr<Component>,ui::ControlKey parent,
        ui::ControlKey& next_key,std::span<const KeyBinding>,std::span<const InputValue>);
    ~Instance();
    Result<void> publish(std::span<const InputValue>);
    Result<void> update();
    Result<bool> event(const ui::Action&);
    // Borrowed until the next update(), close(), or instance destruction.
    // Copy descriptors before retaining them in another owner or frame.
    std::span<const ui::Control> controls() const noexcept;
    Statistics statistics() const noexcept;
    void close() noexcept;
private:
    std::unique_ptr<detail::InstanceData> data_;
    explicit Instance(std::unique_ptr<detail::InstanceData>);
};
} // namespace atelier::ui_language
