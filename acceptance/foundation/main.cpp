#include "../helpers.hpp"
#include "foundation/fixture.hpp"
#include "foundation/shared_array.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

void verify_associative_containers();

void verify_container_lifecycle();

void verify_frame_timing();

namespace {
using namespace atelier;

using namespace atelier::acceptance::foundation;

using namespace atelier::acceptance::helpers;

// Real owned native resources exercise Array's copy failure and over-aligned storage.
struct alignas(64) Resource {
    static inline int live{};

    static inline int copies_until_failure = -1;

    std::unique_ptr<int> value;

    explicit Resource(int number = 0) : value(std::make_unique<int>(number)) { ++live; }

    Resource(const Resource& other) {
        if (copies_until_failure == 0)
            throw std::runtime_error("Resource copy rejected.");
        if (copies_until_failure > 0)
            --copies_until_failure;
        value = std::make_unique<int>(*other.value);
        ++live;
    }

    Resource(Resource&& other) noexcept(false) : value(std::move(other.value)) { ++live; }

    Resource& operator=(const Resource& other) {
        *value = *other.value;
        return *this;
    }

    Resource& operator=(Resource&& other) noexcept {
        value = std::move(other.value);
        return *this;
    }

    ~Resource() { --live; }
};

// These ordinary value types expose constructor/assignment events selected by
// C++ overload resolution, despite having a trivially copyable representation.
struct SelectedOperations {
    static inline int moves{};

    static inline int assignments{};

    int value;

    explicit SelectedOperations(int number) : value(number) {}

    SelectedOperations(const SelectedOperations&) = default;

    SelectedOperations& operator=(const SelectedOperations&) = default;

    template <class U>
        requires std::same_as<std::remove_cvref_t<U>, SelectedOperations>
    SelectedOperations(U&& other) noexcept : value(other.value) {
        ++moves;
    }

    template <class U>
        requires std::same_as<std::remove_cvref_t<U>, SelectedOperations>
    SelectedOperations& operator=(U&& other) noexcept {
        value = other.value;
        ++assignments;
        return *this;
    }
};

struct ImmutableValue {
    const int value;
};

struct alignas(64) Packet {
    std::array<std::uint32_t, 16> words;
};

void array_storage_workload() {
    static_assert(sizeof(Array<int>) == 24);
    static_assert(std::is_trivially_copyable_v<SelectedOperations>);
    static_assert(!std::is_trivially_move_constructible_v<SelectedOperations>);
    static_assert(!std::is_trivially_move_assignable_v<SelectedOperations>);
    std::array<std::int32_t, 513> input;
    for (std::size_t i = 0; i != input.size(); ++i)
        input[i] = static_cast<std::int32_t>(i) - 256;
    Array<std::int32_t> values{Span<const std::int32_t>{input.data(), input.size()}};
    values.squeeze();
    values.append(values.view().slice(3, 127));
    for (std::size_t i = 0; i != values.size(); ++i)
        require(values[i] == input[i < input.size() ? i : i - input.size() + 3],
                "Scalar self-append changed its source.");
    Array<std::int32_t> cache;
    cache.reserve(2048);
    cache.resize(700);
    const auto* allocation = cache.data();
    cache = values;
    require(cache.data() == allocation && cache.capacity() == 2048 && cache.size() == values.size(),
            "Scalar copy assignment discarded reserved storage.");
    const Array<std::int32_t> short_batch{Span<const std::int32_t>{input.data(), 63}};
    cache = short_batch;
    cache = values;
    for (std::size_t i = 0; i != cache.size(); ++i)
        require(cache[i] == values[i], "Capacity reuse lost live prefix or newly constructed tail.");
    cache[0] = 999;
    require(values[0] == -256, "Scalar copy shared its owned buffer.");
    auto& self = cache;
    cache = self;
    require(cache[0] == 999, "Scalar self-assignment changed data.");
    const Array<std::int32_t> empty;
    cache = empty;
    require(cache.empty() && cache.data() == allocation && cache.capacity() == 2048,
            "Empty assignment discarded scalar capacity.");
    cache.resize(32);
    for (auto value : cache)
        require(value == 0, "resize failed to value-initialize reused storage.");

    Array<int> selection{1, 2, 3, 4, 5, 6};
    const auto capacity = selection.capacity();
    selection.remove_at(1, 2);
    selection.remove_at_swap(0);
    require(selection.size() == 3 && selection[0] == 6 && selection[1] == 4 && selection[2] == 5,
            "Ordered or swapped scalar erase changed values.");
    selection.remove_at(selection.size(), 0);
    require(selection.remove_if([](int value) { return value % 2 == 0; }) == 2 && selection[0] == 5,
            "Scalar filter changed order or count.");
    selection.remove_at(0, selection.size());
    require(selection.empty() && selection.capacity() == capacity, "Erase shrank scalar capacity.");
    selection.squeeze();
    selection.append({});
    selection.reserve(0);
    selection.resize(0);
    require(selection.data() == nullptr, "Empty scalar operations acquired storage.");
    Array<int> ordered{1, 2, 3, 4};
    ordered.remove_if([&](int value) {
        if (value == 3)
            require(ordered[0] == 2, "Filter delayed a prior move past its next predicate.");
        return value % 2 != 0;
    });
    require(ordered.size() == 2 && ordered[0] == 2 && ordered[1] == 4, "Filter failed to preserve order.");

    std::array<Packet, 8> packets;
    for (std::size_t i = 0; i != packets.size(); ++i)
        for (std::size_t j = 0; j != 16; ++j)
            packets[i].words[j] = static_cast<std::uint32_t>(i * 16 + j);
    Array<Packet> batch{Span<const Packet>{packets.data(), packets.size()}};
    batch.reserve(64);
    batch.remove_at(1, 2);
    require(reinterpret_cast<std::uintptr_t>(batch.data()) % alignof(Packet) == 0,
            "Bulk storage ignored packet alignment.");
    for (std::size_t i = 0; i != batch.size(); ++i)
        require(batch[i].words == packets[i ? i + 2 : 0].words,
                "Bulk packet relocation or overlapping erase lost bytes.");

    Array<SelectedOperations> events;
    events.emplace(11);
    events.emplace(22);
    events.reserve(32);
    require(SelectedOperations::moves == 2, "Byte relocation skipped a selected native constructor.");
    events.append(events.view());
    events.squeeze();
    require(SelectedOperations::moves == 6 && events[2].value == 11,
            "Growth or copying changed selected constructor events.");
    events.remove_at(0, 2);
    events.remove_at_swap(0);
    require(SelectedOperations::assignments == 3 && events[0].value == 22,
            "Byte erase skipped a selected native assignment.");
    Array<ImmutableValue> immutable;
    immutable.emplace(42);
    Array<ImmutableValue> immutable_copy;
    immutable_copy.emplace(7);
    immutable_copy = immutable;
    require(immutable_copy[0].value == 42, "Copyable elements acquired a new assignment requirement.");
}

void string_storage_workload() {
    Array<String> catalog;
    for (const std::size_t size : {0u, 1u, 7u, 8u, 15u, 16u, 23u, 31u, 63u, 128u, 4096u}) {
        const std::string bytes(size, 'x');
        String text{StringView{bytes}};
        require(text.size() == size && text.c_str()[size] == 0 && text.view().std_view() == bytes,
                "String catalog changed its bytes or terminator.");
        String copy{text};
        text.append(StringView{"+"});
        require(copy.view().std_view() == bytes, "String catalog copy did not own its data.");
        String moved{std::move(copy)};
        require(copy.empty() && copy.c_str()[0] == 0 && moved.view().std_view() == bytes,
                "Moving inline/heap text changed the value or source invariant.");
        catalog.add(std::move(moved));
    }
    catalog.append(catalog.view());
    require(catalog.size() == 22 && catalog[5].size() == 16 && catalog[16].size() == 16,
            "Array growth or self-copy mishandled an owning String.");
    String cache{"short"};
    cache.reserve(1024);
    const auto* storage = cache.c_str();
    const auto capacity = cache.capacity();
    cache.assign(StringView{"replacement"});
    require(cache.c_str() == storage && cache.capacity() == capacity,
            "String assignment discarded sufficient capacity.");
    cache.assign(catalog[5]);
    require(cache.c_str() == storage && cache.size() == 16, "Owning assignment discarded sufficient capacity.");
    cache = catalog[4];
    require(cache.c_str() == storage && cache.size() == 15, "Copy assignment did not reuse the heap buffer.");
    cache.assign(cache.view().slice(2, 7));
    require(cache.c_str() == storage && cache.view() == "xxxxxxx",
            "Overlapping assignment did not preserve its input.");
    cache.squeeze();
    require(cache.capacity() == String::inline_capacity && cache.view() == "xxxxxxx",
            "Heap-to-inline squeeze changed text.");
    cache.assign(StringView{"abcdefghijklmno"});
    cache.append(cache.view().slice(1));
    require(cache.view() == "abcdefghijklmnobcdefghijklmno", "Inline-to-heap append invalidated its self-view.");
    cache.squeeze();
    cache.append(cache);
    require(cache.size() == 58 && cache.view().slice(0, 29) == cache.view().slice(29),
            "Owning self-append changed its source.");
    String small{"name"};
    small.swap(cache);
    require(cache.view() == "name" && small.size() == 58, "Inline/heap swap changed values.");
    small = std::move(cache);
    require(cache.empty() && small.view() == "name", "Move assignment did not replace heap text with inline text.");
    auto& self = small;
    small = std::move(self);
    small = self;
    small.swap(self);
    require(small.view() == "name", "Self-assignment or swap destroyed String.");
    small.reserve(64);
    small.clear();
    require(small.empty() && small.capacity() >= 64 && small.c_str()[0] == 0,
            "clear lost heap capacity or termination.");
    small.release();
    require(small.empty() && small.capacity() == String::inline_capacity, "release did not return to inline storage.");
    bool rejected = false;
    try {
        small.reserve(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected && small.empty(), "String capacity overflow mutated its value.");

    constexpr std::array<StringView, 9> valid{"\x7f",         "\xc2\x80",         "\xdf\xbf",
                                              "\xe0\xa0\x80", "\xed\x9f\xbf",     "\xee\x80\x80",
                                              "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"};
    constexpr std::array<StringView, 10> invalid{"\x80",
                                                 "\xc0\x80",
                                                 "\xc1\xbf",
                                                 "\xc2",
                                                 "\xe0\x9f\xbf",
                                                 "\xed\xa0\x80",
                                                 "\xf0\x8f\xbf\xbf",
                                                 "\xf4\x90\x80\x80",
                                                 "\xf5\x80\x80\x80",
                                                 "\xe2\x82"};
    for (std::size_t prefix = 0; prefix != 18; ++prefix) {
        const std::string ascii(prefix, 'a');
        for (auto value : valid) {
            const std::string input = ascii + std::string{value.std_view()} + "tail";
            small.assign(StringView{input});
            require(small.view().std_view() == input, "UTF-8 validation rejected a scalar at an ASCII word boundary.");
        }
        const String previous{small};
        for (auto value : invalid) {
            const std::string input = ascii + std::string{value.std_view()};
            rejected = false;
            try {
                small.assign(StringView{input});
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected && small == previous, "Invalid UTF-8 assignment changed a live String.");
        }
    }
    small.assign(StringView{"🙂🙂🙂"});
    const auto previous = small;
    rejected = false;
    try {
        small.append(small.view().slice(1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && small == previous, "A byte slice bypassed UTF-8 validation through the owning fast path.");
}

void native_workload() {
    string_storage_workload();
    array_storage_workload();
    Array<String> names;
    names.emplace("sword");
    names.emplace("盾牌");
    names.squeeze();
    names.add(names[0]);
    names.append(names.view());
    require(names.size() == 6 && names[2].view() == "sword" && names[4].view() == "盾牌",
            "Self append invalidated its input.");
    auto copy = names;
    copy[0].append("+1");
    require(names[0].view() == "sword", "Array copy shared owning strings.");
    const auto capacity = copy.capacity();
    copy.remove_at(1);
    copy.remove_at_swap(0);
    require(copy.size() == 4 && copy.capacity() == capacity, "Erase unexpectedly changed capacity.");
    require(copy.remove_if([](const String& value) { return value.view() == "sword"; }) == 3,
            "Ordered removal lost elements.");
    copy.clear();
    require(copy.empty() && copy.capacity() == capacity, "clear lost reserved storage.");
    copy.squeeze();
    require(copy.capacity() == 0, "Empty squeeze retained storage.");
    Array<String> filled{String{"a label longer than the inline capacity"}};
    filled.squeeze();
    filled.resize(40, filled[0]);
    for (const auto& label : filled)
        require(label.view() == "a label longer than the inline capacity", "Filled growth lost its aliased value.");
    const auto filled_capacity = filled.capacity();
    filled.resize(3, String{"unused"});
    require(filled.size() == 3 && filled.capacity() == filled_capacity &&
                filled[2].view() == "a label longer than the inline capacity",
            "Filled shrink changed capacity or kept elements.");
    Array<std::unique_ptr<int>> unique;
    unique.emplace(std::make_unique<int>(42));
    unique.reserve(32);
    require(*unique[0] == 42, "Move-only resource relocation failed.");
    static_assert(!std::is_copy_constructible_v<decltype(unique)>);
    static_assert(std::is_nothrow_move_constructible_v<Array<Resource>>);
    static_assert(std::is_convertible_v<Span<int>, Span<const int>>);
    static_assert(!std::is_convertible_v<Span<const int>, Span<int>>);
    static_assert(detail::borrowed_result<Span<int>> && detail::borrowed_result<StringView>);
    static_assert(SharedElement<ObjectId> && !SharedElement<ObjectReference> && !SharedElement<ReferenceSlot> &&
                  !SharedElement<ObjectRoot>);
    {
        Array<Resource> resources;
        resources.emplace(7);
        resources.emplace(9);
        resources.squeeze();
        require(reinterpret_cast<std::uintptr_t>(resources.data()) % alignof(Resource) == 0,
                "Array ignored over-alignment.");
        Resource::copies_until_failure = 1;
        bool failed = false;
        try {
            resources.reserve(50);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "Expected resource failure.");
        require(resources.size() == 2 && *resources[0].value == 7 && Resource::live == 2,
                "Failed growth leaked or damaged old resources.");
        Resource::copies_until_failure = 2;
        failed = false;
        try {
            resources.append(resources.view());
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed, "Expected prefix relocation failure.");
        require(resources.size() == 2 && Resource::live == 2 && *resources[1].value == 9,
                "Failed self-append left a live suffix.");
        Resource::copies_until_failure = -1;
        {
            Array<Resource> destination;
            destination.reserve(64);
            destination.emplace(100);
            const auto* original = destination.data();
            Resource::copies_until_failure = 1;
            failed = false;
            try {
                destination = resources;
            } catch (const std::runtime_error&) {
                failed = true;
            }
            require(failed && destination.size() == 1 && *destination[0].value == 100 &&
                        destination.data() == original && destination.capacity() == 64 && Resource::live == 3,
                    "Failed resource assignment changed reserved target or leaked its copy.");
            Resource::copies_until_failure = -1;
        }
        Resource::copies_until_failure = 2;
        failed = false;
        try {
            resources.resize(5, resources[1]);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        Resource::copies_until_failure = -1;
        require(failed && resources.size() == 2 && *resources[1].value == 9 && Resource::live == 2,
                "Failed filled resize left a partial suffix.");
        resources.resize(4, resources[0]);
        require(resources.size() == 4 && *resources[3].value == 7 && *resources[1].value == 9 && Resource::live == 4,
                "Filled resize lost its aliased resource.");
        resources.resize(2, resources[0]);
        require(Resource::live == 2, "Filled shrink kept destroyed resources.");
        auto moved = std::move(resources);
        require(resources.empty() && moved.size() == 2, "Moved-from array is not empty.");
    }
    require(Resource::live == 0, "Array leaked resource elements.");
    bool rejected = false;
    try {
        unique.reserve(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected && *unique[0] == 42, "Capacity overflow changed the array.");
    rejected = false;
    try {
        (void)unique.view().slice(2);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    require(rejected, "Invalid view slice was accepted.");

    String text{"你好🙂"};
    require(text.size() == 10 && text.view().is_valid_utf8(), "String length is not UTF-8 bytes.");
    text.squeeze();
    text.append(text.view());
    require(text.size() == 20 && text.c_str()[20] == 0, "Self append lost its UTF-8 terminator.");
    text.assign(text.view().slice(6, 4));
    require(text.view() == "🙂", "Overlapping assign lost its source.");
    const char embedded[]{'a', '\0', 'b'};
    text.assign(StringView{embedded, 3});
    require(text.size() == 3 && text[2] == 'b' && text.c_str()[3] == 0, "String lost embedded zero bytes.");
    for (const auto invalid : {StringView{"\xc0\x80", 2}, StringView{"\xed\xa0\x80", 3},
                               StringView{"\xf4\x90\x80\x80", 4}, StringView{"\xe4\xbd", 2}}) {
        rejected = false;
        try {
            text.append(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && text.size() == 3, "Invalid UTF-8 modified String.");
    }
    text.clear();
    text.append("reuse");
    auto moved = std::move(text);
    require(text.empty() && text.c_str()[0] == 0 && moved.view() == "reuse", "Moved String invariants failed.");
}

struct RefRecord {
    ReferenceSlot target;
};

void reflection_workload() {
    Runtime runtime;
    // Publication stays explicit and atomic, with arbitrary dependency order.
    const auto types = take(runtime.register_types(
        {reflection::Definition<Inventory>::build(runtime), array_definition<Item>(runtime, "foundation.Items"),
         array_definition<String>(runtime, "foundation.Strings"),
         array_definition<Array<std::int32_t>>(runtime, "foundation.Batches"),
         array_definition<std::int32_t>(runtime, "foundation.Counts"), reflection::Definition<Item>::build(runtime),
         reflection::Definition<Quality>::build(runtime), string_definition(runtime)}));
    const auto array_type = take(runtime.native_type<Array<Item>>());
    const auto item_type = take(runtime.native_type<Item>());
    const auto info = take(runtime.describe(array_type));
    require(info->kind == TypeKind::array && info->element == item_type && !info->contains_references,
            "Array reflection lost element identity.");
    require(take(runtime.describe(take(runtime.native_type<String>())))->size == sizeof(String),
            "Runtime did not publish the current String representation.");
    auto bad = runtime.register_types(
        {array_definition<RefRecord>(runtime, "foundation.BadArray"),
         NativeRegistration<RefRecord>{runtime, "foundation.RefRecord", TypeKind::record}
             .field<&RefRecord::target>("target", {TypeReference{"core.Object"}, ValueForm::strong_reference})
             .build()});
    require(!bad && !runtime.native_type<RefRecord>() && !runtime.native_type<Array<RefRecord>>(),
            "GC-bearing array bypassed validation or partly published.");
    take(runtime.register_types({NativeRegistration<Resource>{runtime, "foundation.Resource", TypeKind::record}.build(),
                                 array_definition<Resource>(runtime, "foundation.Resources")}));
    TypeDefinition described;
    described.name = "foundation.DescribedInventory";
    described.fields = {{"items", {array_type}},
                        {"label", {take(runtime.native_type<String>())}},
                        {"resources", {take(runtime.native_type<Array<Resource>>())}}};
    const auto described_type = take(runtime.register_type(std::move(described)));

    auto native = take(runtime.create(types[0]));
    auto data = take(runtime.create(described_type));
    take(runtime.with_native<Inventory>(native.id(), [&](Inventory& value) -> Result<void> {
        value.label.assign("loot");
        value.items.emplace(Item{String{"shield"}, 2, Quality::rare});
        value.items.emplace(Item{String{"asset:/weapons/steel_sword.asset"}, 1, Quality::common});
        value.tags.emplace("equipment");
        value.batches.emplace(Array<std::int32_t>{1, 2, 3});
        require(!runtime.destroy(native.id()), "Native storage was destroyed during borrow.");
        return {};
    }));
    const std::array native_path{take(runtime.find_field(types[0], "items"))};
    auto saved = take(runtime.with_native<const Array<Item>>(
        native.id(), native_path, [](const Array<Item>& value) -> Result<Array<Item>> { return value; }));
    const std::array path{take(runtime.find_field(described_type, "items"))};
    take(runtime.with_native<Array<Item>>(data.id(), path, [&](Array<Item>& value) -> Result<void> {
        value = saved;
        value[0].name.append("+1");
        return {};
    }));
    require(saved[0].name.view() == "shield" && saved.size() == 2 &&
                saved[1].name.view() == "asset:/weapons/steel_sword.asset",
            "Described field copy changed inline/heap String values.");
    const auto wrong = runtime.with_native<String>(data.id(), path, [](String&) -> Result<void> { return {}; });
    require(!wrong && wrong.error().code == ErrorCode::type_mismatch, "Field borrow relabeled native storage.");

    alignas(Array<Item>) std::array<std::byte, sizeof(Array<Item>)> storage{};
    auto source = std::as_bytes(std::span{&saved, 1});
    take(runtime.copy_construct_value(array_type, storage, source));
    auto& clone = *std::launder(reinterpret_cast<Array<Item>*>(storage.data()));
    clone[0].count = 99;
    require(saved[0].count == 2 && clone[0].name.view() == "shield",
            "Runtime value copy did not construct owned elements.");
    take(runtime.assign_value(array_type, storage, source));
    require(clone[0].count == 2, "Runtime array assignment failed.");
    take(runtime.destroy_value(array_type, storage));

    const Array<std::int32_t> counts{10, 20, 30, 40};
    const auto counts_type = take(runtime.native_type<Array<std::int32_t>>());
    alignas(Array<std::int32_t>) std::array<std::byte, sizeof(Array<std::int32_t>)> counts_storage{};
    take(runtime.copy_construct_value(counts_type, counts_storage, std::as_bytes(std::span{&counts, 1})));
    auto& counts_copy = *std::launder(reinterpret_cast<Array<std::int32_t>*>(counts_storage.data()));
    counts_copy.reserve(64);
    counts_copy.resize(2);
    const auto* counts_allocation = counts_copy.data();
    take(runtime.assign_value(counts_type, counts_storage, std::as_bytes(std::span{&counts, 1})));
    require(counts_copy.data() == counts_allocation && counts_copy.capacity() == 64 && counts_copy.size() == 4 &&
                counts_copy[3] == 40,
            "Shared Runtime scalar array assignment lost capacity or its new tail.");
    take(runtime.destroy_value(counts_type, counts_storage));
    const std::array resource_path{take(runtime.find_field(described_type, "resources"))};
    take(runtime.with_native<Array<Resource>>(data.id(), resource_path, [](Array<Resource>& resources) -> Result<void> {
        resources.emplace(123);
        resources.emplace(456);
        return {};
    }));
    require(Resource::live == 2, "Described object did not own its native resources.");
    take(runtime.destroy(native.id()));
    const auto data_id = data.id();
    data.reset();
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        (void)take(runtime.collect_step({1}));
    require(!runtime.type_of(data_id) && Resource::live == 0, "GC did not destroy the value array and its resources.");
    require(saved[0].name.view() == "shield" && saved[1].name.view() == "asset:/weapons/steel_sword.asset",
            "Owned inline/heap snapshot depended on destroyed runtime object.");
}
}

int main() {
    try {
        native_workload();
        reflection_workload();
        verify_associative_containers();
        verify_container_lifecycle();
        verify_frame_timing();
        std::puts(
            "Foundation acceptance: PASS (RAII, Array/String, Map/Set resource catalog, aliasing, UTF-8, failure cleanup, generated fields, shared value operations, frame timing catalog/scopes)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Foundation acceptance: FAIL: %s\n", error.what());
        return 1;
    }
}
