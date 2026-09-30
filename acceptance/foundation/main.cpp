#include "foundation/fixture.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using namespace atelier;
using namespace atelier::acceptance::foundation;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}
void take(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

// Real owned native resources exercise Array's copy failure and over-aligned storage.
struct alignas(64) Resource {
    static inline int live{}, copies_until_failure = -1;
    std::unique_ptr<int> value;
    explicit Resource(int number = 0) : value(std::make_unique<int>(number)) { ++live; }
    Resource(const Resource& other) {
        if (copies_until_failure == 0) throw std::runtime_error("Resource copy rejected.");
        if (copies_until_failure > 0) --copies_until_failure;
        value = std::make_unique<int>(*other.value); ++live;
    }
    Resource(Resource&& other) noexcept(false) : value(std::move(other.value)) { ++live; }
    Resource& operator=(const Resource& other) { *value = *other.value; return *this; }
    Resource& operator=(Resource&& other) noexcept { value = std::move(other.value); return *this; }
    ~Resource() { --live; }
};

void native_workload() {
    Array<String> names;
    names.emplace("sword"); names.emplace("盾牌");
    names.squeeze();
    names.add(names[0]);
    names.append(names.view());
    require(names.size() == 6 && names[2].view() == "sword" && names[4].view() == "盾牌", "Self append invalidated its input.");
    auto copy = names;
    copy[0].append("+1");
    require(names[0].view() == "sword", "Array copy shared owning strings.");
    const auto capacity = copy.capacity();
    copy.remove_at(1); copy.remove_at_swap(0);
    require(copy.size() == 4 && copy.capacity() == capacity, "Erase unexpectedly changed capacity.");
    require(copy.remove_if([](const String& value) { return value.view() == "sword"; }) == 3, "Ordered removal lost elements.");
    copy.clear(); require(copy.empty() && copy.capacity() == capacity, "clear lost reserved storage.");
    copy.squeeze(); require(copy.capacity() == 0, "Empty squeeze retained storage.");
    Array<std::unique_ptr<int>> unique;
    unique.emplace(std::make_unique<int>(42)); unique.reserve(32);
    require(*unique[0] == 42, "Move-only resource relocation failed.");
    static_assert(!std::is_copy_constructible_v<decltype(unique)>);
    static_assert(std::is_nothrow_move_constructible_v<Array<Resource>>);
    static_assert(std::is_convertible_v<Span<int>, Span<const int>>);
    static_assert(!std::is_convertible_v<Span<const int>, Span<int>>);
    static_assert(detail::borrowed_result<Span<int>> && detail::borrowed_result<StringView>);
    {
        Array<Resource> resources; resources.emplace(7); resources.emplace(9); resources.squeeze();
        require(reinterpret_cast<std::uintptr_t>(resources.data()) % alignof(Resource) == 0, "Array ignored over-alignment.");
        Resource::copies_until_failure = 1;
        bool failed = false;
        try { resources.reserve(50); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "Expected resource failure.");
        require(resources.size() == 2 && *resources[0].value == 7 && Resource::live == 2, "Failed growth leaked or damaged old resources.");
        Resource::copies_until_failure = 2;
        failed = false;
        try { resources.append(resources.view()); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "Expected prefix relocation failure.");
        require(resources.size() == 2 && Resource::live == 2 && *resources[1].value == 9, "Failed self-append left a live suffix.");
        Resource::copies_until_failure = -1;
        auto moved = std::move(resources); require(resources.empty() && moved.size() == 2, "Moved-from array is not empty.");
    }
    require(Resource::live == 0, "Array leaked resource elements.");
    bool rejected = false;
    try { unique.reserve(std::numeric_limits<std::size_t>::max()); } catch (const std::length_error&) { rejected = true; }
    require(rejected && *unique[0] == 42, "Capacity overflow changed the array.");
    rejected = false;
    try { (void)unique.view().slice(2); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "Invalid view slice was accepted.");

    String text{"你好🙂"};
    require(text.size() == 10 && text.view().is_valid_utf8(), "String length is not UTF-8 bytes.");
    text.squeeze(); text.append(text.view());
    require(text.size() == 20 && text.c_str()[20] == 0, "Self append lost its UTF-8 terminator.");
    text.assign(text.view().slice(6, 4));
    require(text.view() == "🙂", "Overlapping assign lost its source.");
    const char embedded[]{'a', '\0', 'b'};
    text.assign(StringView{embedded, 3});
    require(text.size() == 3 && text[2] == 'b' && text.c_str()[3] == 0, "String lost embedded zero bytes.");
    for (const auto invalid : {StringView{"\xc0\x80", 2}, StringView{"\xed\xa0\x80", 3}, StringView{"\xf4\x90\x80\x80", 4}, StringView{"\xe4\xbd", 2}}) {
        rejected = false;
        try { text.append(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && text.size() == 3, "Invalid UTF-8 modified String.");
    }
    text.clear(); text.append("reuse");
    auto moved = std::move(text);
    require(text.empty() && text.c_str()[0] == 0 && moved.view() == "reuse", "Moved String invariants failed.");
}

struct RefRecord { ReferenceSlot target; };

void reflection_workload() {
    Runtime runtime;
    // Publication stays explicit and atomic, with arbitrary dependency order.
    const auto types = take(runtime.register_types({
        reflection::Definition<Inventory>::build(runtime),
        array_definition<Item>(runtime, "foundation.Items"),
        array_definition<String>(runtime, "foundation.Strings"),
        array_definition<Array<std::int32_t>>(runtime, "foundation.Batches"),
        array_definition<std::int32_t>(runtime, "foundation.Counts"),
        reflection::Definition<Item>::build(runtime),
        reflection::Definition<Quality>::build(runtime),
        string_definition(runtime)}));
    const auto array_type = take(runtime.native_type<Array<Item>>());
    const auto item_type = take(runtime.native_type<Item>());
    const auto info = take(runtime.describe(array_type));
    require(info->kind == TypeKind::array && info->element == item_type && !info->contains_references,
        "Array reflection lost element identity.");
    auto bad = runtime.register_types({
        array_definition<RefRecord>(runtime, "foundation.BadArray"),
        NativeRegistration<RefRecord>{runtime, "foundation.RefRecord", TypeKind::record}
            .field<&RefRecord::target>("target", {TypeReference{"core.Object"}, ValueForm::strong_reference}).build()});
    require(!bad && !runtime.native_type<RefRecord>() && !runtime.native_type<Array<RefRecord>>(), "GC-bearing array bypassed validation or partly published.");
    take(runtime.register_types({
        NativeRegistration<Resource>{runtime, "foundation.Resource", TypeKind::record}.build(),
        array_definition<Resource>(runtime, "foundation.Resources")}));
    TypeDefinition described;
    described.name = "foundation.DescribedInventory";
    described.fields = {{"items", {array_type}}, {"label", {take(runtime.native_type<String>())}},
        {"resources", {take(runtime.native_type<Array<Resource>>())}}};
    const auto described_type = take(runtime.register_type(std::move(described)));

    auto native = take(runtime.create(types[0]));
    auto data = take(runtime.create(described_type));
    take(runtime.with_native<Inventory>(native.id(), [&](Inventory& value) -> Result<void> {
        value.label.assign("loot"); value.items.emplace(Item{String{"shield"}, 2, Quality::rare});
        value.tags.emplace("equipment"); value.batches.emplace(Array<std::int32_t>{1, 2, 3});
        require(!runtime.destroy(native.id()), "Native storage was destroyed during borrow.");
        return {};
    }));
    const std::array native_path{take(runtime.find_field(types[0], "items"))};
    auto saved = take(runtime.with_native<const Array<Item>>(native.id(), native_path,
        [](const Array<Item>& value) -> Result<Array<Item>> { return value; }));
    const std::array path{take(runtime.find_field(described_type, "items"))};
    take(runtime.with_native<Array<Item>>(data.id(), path, [&](Array<Item>& value) -> Result<void> {
        value = saved; value[0].name.append("+1"); return {};
    }));
    require(saved[0].name.view() == "shield", "Described field copy shared native array storage.");
    const auto wrong = runtime.with_native<String>(data.id(), path, [](String&) -> Result<void> { return {}; });
    require(!wrong && wrong.error().code == ErrorCode::type_mismatch, "Field borrow relabeled native storage.");

    alignas(Array<Item>) std::array<std::byte, sizeof(Array<Item>)> storage{};
    auto source = std::as_bytes(std::span{&saved, 1});
    take(runtime.copy_construct_value(array_type, storage, source));
    auto& clone = *std::launder(reinterpret_cast<Array<Item>*>(storage.data()));
    clone[0].count = 99;
    require(saved[0].count == 2 && clone[0].name.view() == "shield", "Runtime value copy did not construct owned elements.");
    take(runtime.assign_value(array_type, storage, source));
    require(clone[0].count == 2, "Runtime array assignment failed.");
    take(runtime.destroy_value(array_type, storage));
    const std::array resource_path{take(runtime.find_field(described_type, "resources"))};
    take(runtime.with_native<Array<Resource>>(data.id(), resource_path, [](Array<Resource>& resources) -> Result<void> {
        resources.emplace(123); resources.emplace(456); return {};
    }));
    require(Resource::live == 2, "Described object did not own its native resources.");
    take(runtime.destroy(native.id()));
    const auto data_id = data.id();
    data.reset(); take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle) (void)take(runtime.collect_step({1}));
    require(!runtime.type_of(data_id) && Resource::live == 0, "GC did not destroy the value array and its resources.");
    require(saved[0].name.view() == "shield", "Owned snapshot depended on destroyed runtime object.");
}
}

int main() {
    try {
        native_workload(); reflection_workload();
        std::puts("Foundation acceptance: PASS (RAII, aliasing, UTF-8, failure cleanup, generated fields, shared value operations)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Foundation acceptance: FAIL: %s\n", error.what()); return 1;
    }
}
