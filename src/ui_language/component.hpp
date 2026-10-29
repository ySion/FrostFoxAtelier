#pragma once

#include "ui_language/declaration.hpp"
#include "source/program.hpp"
#include "ui/controls.hpp"

namespace atelier::ui_language {
namespace detail { struct InstanceData; struct DefinitionState; struct PreparedUpdate; struct ScopeLifetime; }

// A sampled edit reference: field keys are owned by the host's existing typed
// property/transaction routes. Neither the component nor this value retains Entities.
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
// A sampled host action. The host owns its nonzero target and revalidates the
// command when invoked; the binding never retains a business object.
struct ActionBinding {
    ui::ControlKey target{};
    bool enabled{true};
    bool operator==(const ActionBinding&) const = default;
};
// The shared Table consumes the host's immutable data directly. Selection is a
// small independent value; changing it never copies rows or cell strings.
struct TableBinding {
    std::shared_ptr<const ui::TableData> data;
    ui::TableSelection selection;
    bool enabled{true};
    bool operator==(const TableBinding&) const = default;
};
struct CodeBinding {
    std::shared_ptr<ui::CodeDocument> document;
    bool enabled{true};
    bool read_only{}; // Selection, copy and navigation remain enabled; UI input cannot modify the document.
    bool operator==(const CodeBinding&) const = default;
};
// Optional immutable help shared with native controls. It owns text only, never
// an Entity or borrowed reflection metadata. Null means no local help.
using TooltipBinding=std::shared_ptr<const ui::Tooltip>;
// One flattened host row (see ui::flatten_tree); TreeItem copies it into its descriptor.
struct TreeRowBinding {
    std::uint32_t index{}, depth{};
    bool branch{}, expanded{}, primary{};
    std::bitset<128> lines;
    bool operator==(const TreeRowBinding&) const = default;
};
struct Item;
struct ItemList {
    // An immutable snapshot: relinquish all mutable aliases before update().
    std::shared_ptr<const std::vector<Item>> values;
    bool operator==(const ItemList&) const;
};
using InputValue=std::variant<bool,double,EditBinding,std::wstring,TextEditBinding,ui::ControlKey,ItemList,ActionBinding,TableBinding,CodeBinding,TooltipBinding,TreeRowBinding>;
struct KeyBinding {
    // A node in this component, or a dotted path through named static component
    // instances. Dotted endpoints own their keys; TextField uses TextEditBinding.
    std::string node; ui::ControlKey key{}, label{}, body{};
    bool operator==(const KeyBinding&) const = default;
};
struct Item {
    ui::ControlKey key{}; // Host-allocated stable item identity; never the list index.
    std::vector<InputValue> inputs;
    // Optional host slots and named native interaction identities. The root key
    // always comes from key. Reparenting uses the same reversible scope update.
    std::vector<KeyBinding> keys{};
    std::optional<ui::ControlKey> parent{};
    bool operator==(const Item&) const = default;
};
struct Statistics {
    std::uint64_t binding_evaluations{}, event_evaluations{}, publications{}, node_creations{}, node_removals{};
    ObjectId instance;
    // Actual Context lifetimes in this root's entire subtree, including refused candidates.
    std::uint64_t scope_creations{},scope_retirements{};
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
    friend struct detail::DefinitionState;
    friend struct detail::PreparedUpdate;
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

// Single-threaded UI scope. update() prepares inputs and an optional replacement
// definition through one reversible tree transaction. Compatible scopes keep
// their Runtime Context and state. Submit to Session, route cleanup
// effects through the old scopes, then commit_controls(); on refusal, reject.
// It never runs World/editor commands inside JIT.
// The host key counter and Runtime must outlive the instance, including its
// dynamically mounted children. Call commit_controls() after Session effects.
class Instance {
public:
    static Result<std::unique_ptr<Instance>> create(std::shared_ptr<Component>,ui::ControlKey parent,
        ui::ControlKey& next_key,std::span<const KeyBinding>,std::span<const InputValue>);
    ~Instance();
    Result<void> update(std::span<const InputValue>,std::shared_ptr<Component> definition={});
    // Re-evaluate local events, or keep an already prepared snapshot unchanged.
    Result<void> update();
    Result<bool> event(const ui::Action&);
    // Borrowed until the next update/reject/commit, close, or destruction.
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
        ui::ControlKey&,std::span<const KeyBinding>,std::span<const InputValue>,std::size_t,std::shared_ptr<detail::ScopeLifetime>);
    std::span<const InputValue> inputs() const noexcept;
    bool has_pending() const noexcept;
    bool changes_controls() const noexcept;
    void reject_controls(bool retain_events) noexcept;
    friend struct detail::InstanceData;
    friend struct detail::PreparedUpdate;
    friend class LiveDocument;
};
} // namespace atelier::ui_language
