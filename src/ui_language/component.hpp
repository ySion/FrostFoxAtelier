#pragma once

#include "ui_language/declaration.hpp"
#include "source/program.hpp"
#include "ui/controls.hpp"

namespace atelier::ui_language {
namespace detail { struct InstanceData; struct PreparedUpdate; }

// A sampled edit reference: field keys are owned by the host's existing typed
// property/transaction routes. Neither the component nor this value retains Actors.
struct EditBinding {
    ui::ControlKey group{};
    bool enabled{}, modified{};
    std::vector<ui::NumberField> fields;
    bool operator==(const EditBinding&) const = default;
};
struct TextEditBinding {
    ui::ControlKey key{};
    std::wstring text;
    bool enabled{true};
    bool operator==(const TextEditBinding&) const = default;
};
struct Item;
struct ItemList {
    // An immutable snapshot: relinquish all mutable aliases before publish().
    std::shared_ptr<const std::vector<Item>> values;
    bool operator==(const ItemList&) const;
};
using InputValue=std::variant<bool,double,EditBinding,std::wstring,TextEditBinding,ui::ControlKey,ItemList>;
struct Item {
    ui::ControlKey key{}; // Host-allocated stable item identity; never the list index.
    std::vector<InputValue> inputs;
    bool operator==(const Item&) const = default;
};
struct KeyBinding {
    std::string node; ui::ControlKey key{}, label{}, body{};
    bool operator==(const KeyBinding&) const = default;
};
struct Statistics {
    std::uint64_t binding_evaluations{}, event_evaluations{}, publications{}, node_creations{}, node_removals{};
};

// Call during the host Runtime's existing bootstrap, before its first object.
Result<TypeId> register_types(Runtime&);

// One compiled Source/JIT module per component type in this Runtime. Instances
// share code and metadata; close all instances/components before the Runtime.
class Component {
public:
    static Result<std::shared_ptr<Component>> compile(Runtime&,std::string_view name,std::string_view source,std::string_view entry={});
    ~Component();
    const Declaration& declaration() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Component(std::unique_ptr<Impl>);
    friend struct detail::InstanceData;
    friend class Instance;
    friend class Module;
};

// One source module may declare several types. Child instances share these
// compiled types; cycles are rejected before any UI instances are mounted.
class Module {
public:
    // Reuse only identical dispatch code from the active module in this Runtime.
    // Every revision still lowers/validates the whole graph and owns fresh metadata.
    static Result<std::shared_ptr<Module>> compile(Runtime&,std::string_view name,std::string_view source,const Module* previous=nullptr);
    std::shared_ptr<Component> find(std::string_view) const;
    struct Compilation {std::size_t compiled{},reused{};};
    Compilation compilation() const noexcept {return compilation_;}
private:
    std::vector<std::shared_ptr<Component>> components_;
    Compilation compilation_;
    friend class Component;
};

// Single-threaded UI scope. publish() is the explicit external-data boundary;
// it only invalidates bindings that actually read changed inputs. update()
// prepares a reversible control snapshot. Submit it to Session, route cleanup
// effects through the old scopes, then commit_controls(); on refusal, reject.
// It never runs World/editor commands inside JIT.
// The host key counter and Runtime must outlive the instance, including its
// dynamically mounted children. Call commit_controls() after Session effects.
class Instance {
public:
    static Result<std::unique_ptr<Instance>> create(std::shared_ptr<Component>,ui::ControlKey parent,
        ui::ControlKey& next_key,std::span<const KeyBinding>,std::span<const InputValue>,const Instance* previous=nullptr);
    ~Instance();
    Result<void> publish(std::span<const InputValue>);
    Result<void> update();
    Result<bool> event(const ui::Action&);
    // Borrowed until the next publish/update/reject, close, or destruction.
    // Copy descriptors before retaining them in another owner or frame.
    std::span<const ui::Control> controls() const noexcept;
    Statistics statistics() const noexcept;
    // Commit the prepared snapshot and scope membership after Session effects.
    void commit_controls() noexcept;
    void reject_controls() noexcept;
    void close() noexcept;
private:
    std::unique_ptr<detail::InstanceData> data_;
    explicit Instance(std::unique_ptr<detail::InstanceData>);
    static Result<std::unique_ptr<Instance>> create_bounded(std::shared_ptr<Component>,ui::ControlKey,
        ui::ControlKey&,std::span<const KeyBinding>,std::span<const InputValue>,const Instance*,std::size_t);
    std::span<const InputValue> inputs() const noexcept;
    bool has_pending() const noexcept;
    bool changes_controls() const noexcept;
    friend struct detail::InstanceData;
    friend struct detail::PreparedUpdate;
    friend class LiveDocument;
};
} // namespace atelier::ui_language
