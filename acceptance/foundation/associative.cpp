#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "runtime/runtime.hpp"

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace atelier;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// An owned resource catalog exercises the public container's lifecycle under
// refresh, independent snapshots, failed construction and dense deletion.
struct alignas(64) Resource {
    static inline int live{}, copies_until_failure = -1;
    std::unique_ptr<int> payload;
    explicit Resource(int id) {
        if (id < 0) throw std::runtime_error("Resource creation rejected.");
        payload = std::make_unique<int>(id); ++live;
    }
    Resource(const Resource& other) {
        if (copies_until_failure == 0) throw std::runtime_error("Resource snapshot rejected.");
        if (copies_until_failure > 0) --copies_until_failure;
        payload = std::make_unique<int>(*other.payload); ++live;
    }
    Resource(Resource&& other) noexcept : payload(std::move(other.payload)) { ++live; }
    Resource& operator=(Resource&&) noexcept = default;
    ~Resource() { --live; }
};
struct CollisionHash {
    std::size_t operator()(int) const noexcept { return 0; }
};
struct OwnedPointerEqual {
    bool operator()(const std::unique_ptr<int>& left, const std::unique_ptr<int>& right) const noexcept {
        return left.get() == right.get();
    }
};
}

void verify_associative_containers() {
    using Catalog = Map<String, Resource>;
    static_assert(std::forward_iterator<Catalog::Iterator> && std::forward_iterator<Catalog::ConstIterator>);
    static_assert(!std::is_assignable_v<decltype(std::declval<Catalog::Iterator>()->key), String>);
    static_assert(std::same_as<decltype(std::declval<Catalog::ConstIterator>()->value), const Resource&>);
    static_assert(std::same_as<decltype(*std::declval<Set<int>&>().begin()), const int&>);
    static_assert(!std::is_copy_constructible_v<Map<int, std::unique_ptr<int>>>);
    using OwnedSet = Set<std::unique_ptr<int>, Hash<std::unique_ptr<int>>, OwnedPointerEqual>;
    static_assert(!std::is_copy_constructible_v<OwnedSet>);
    {
        Catalog catalog;
        Set<String> selected;
        catalog.reserve(300); selected.reserve(300);
        for (int i = 0; i < 300; ++i) {
            const auto key = "asset/材质/" + std::to_string(i);
            const auto added = catalog.try_emplace(StringView{key}, i);
            require(added.inserted && *added.value->payload == i &&
                reinterpret_cast<std::uintptr_t>(added.value) % alignof(Resource) == 0,
                "Catalog insertion lost its value or resource alignment.");
            require(selected.add(StringView{key}) && !selected.add(StringView{key}), "Selection did not deduplicate text keys.");
        }
        require(Resource::live == 300 && catalog.size() == 300, "Catalog leaked a resource during growth.");
        const auto duplicate = catalog.try_emplace("asset/材质/0", -1);
        require(!duplicate.inserted && *duplicate.value->payload == 0, "Duplicate insertion constructed a resource.");
        for (int i = 0; i < 300; i += 3) {
            const auto key = "asset/材质/" + std::to_string(i);
            require(catalog.remove(StringView{key}) && selected.remove(StringView{key}), "Catalog refresh could not remove a key.");
        }
        for (int i = 0; i < 300; ++i) {
            const auto key = "asset/材质/" + std::to_string(i);
            const auto* value = std::as_const(catalog).find(StringView{key});
            require((value != nullptr) == (i % 3 != 0) && selected.contains(StringView{key}) == (value != nullptr),
                "Dense deletion lost a moved entry or retained a removed one.");
            if (value) require(*value->payload == i, "Catalog lookup found another resource.");
        }
        Catalog snapshot = catalog;
        auto selection_snapshot = selected;
        for (auto [key, value] : catalog) {
            require(snapshot.contains(key) && selection_snapshot.contains(key), "Snapshot lost an owned key.");
            *value.payload += 1000;
        }
        require(*snapshot.find("asset/材质/1")->payload == 1, "Catalog snapshot shared its owned resource.");
        const auto replaced = catalog.insert_or_assign("asset/材质/1", 7);
        require(!replaced.inserted && *replaced.value->payload == 7, "Explicit replacement changed key cardinality.");
        bool rejected = false;
        try { catalog.insert_or_assign("asset/材质/1", -1); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && *catalog.find("asset/材质/1")->payload == 7, "Failed replacement changed the old resource.");
        rejected = false;
        try { catalog.try_emplace("missing", -1); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && !catalog.contains("missing") && catalog.size() == 200, "Failed insertion published a key.");

        Catalog destination;
        destination.try_emplace("keep", 42);
        const auto live_before = Resource::live;
        Resource::copies_until_failure = 3;
        rejected = false;
        try { destination = catalog; } catch (const std::runtime_error&) { rejected = true; }
        Resource::copies_until_failure = -1;
        require(rejected && destination.size() == 1 && *destination.find("keep")->payload == 42 && Resource::live == live_before,
            "Failed catalog snapshot leaked partial resources or changed the destination.");

        const String embedded{StringView{"part\0tail", 9}};
        catalog.try_emplace(embedded, 71);
        require(catalog.contains(embedded.view()) && !catalog.contains("part") &&
            Hash<String>{}(embedded) == Hash<StringView>{}(embedded.view()), "Embedded NUL hash/equality disagreed.");
        const StringView invalid{"\xff", 1};
        require(!catalog.contains(invalid) && !selected.contains(invalid), "Arbitrary-byte lookup constructed an owning String.");
        const auto old_size = catalog.size();
        rejected = false;
        try { catalog.try_emplace(invalid, 5); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && catalog.size() == old_size && Resource::live == live_before + 1,
            "Invalid UTF-8 insertion published storage or leaked a resource.");

        auto moved = std::move(catalog);
        require(catalog.empty() && moved.size() == old_size, "Catalog move did not transfer ownership.");
        catalog.try_emplace("reused", 8);
        catalog.swap(moved);
        require(catalog.contains(embedded.view()) && moved.contains("reused"), "Catalog swap lost owned storage.");
        catalog.clear(); selected.clear();
        Catalog::ConstIterator empty_iterator = catalog.begin();
        require(catalog.empty() && empty_iterator == std::as_const(catalog).end() && selected.empty(),
            "Cleared container iteration is not empty.");
        catalog.try_emplace("after-clear", 9); selected.add("after-clear");
        catalog.release(); selected.release();
        require(catalog.empty() && selected.empty(), "Release retained live entries.");
        for (bool map : {false, true}) {
            rejected = false;
            try {
                if (map) catalog.reserve(std::numeric_limits<std::size_t>::max());
                else selected.reserve(std::numeric_limits<std::size_t>::max());
            } catch (const std::length_error&) { rejected = true; }
            require(rejected, "Impossible hash capacity was silently clamped.");
        }
    }
    require(Resource::live == 0, "Catalog lifetime leaked resources.");

    // Collision-heavy membership and repeated refresh exercise the same public
    // operations as the resource catalog, including erase's moved-tail repair.
    Map<int, int, CollisionHash> indexes;
    Set<int, CollisionHash> identities;
    for (int i = 0; i < 256; ++i) { indexes.try_emplace(i, i * 2); identities.add(i); }
    for (int pass = 0; pass < 4; ++pass) {
        for (int i = pass; i < 256; i += 4) {
            require(indexes.remove(i) && identities.remove(i), "Collision refresh could not erase an identity.");
            require(!indexes.find(i) && !identities.contains(i), "Collision refresh retained an erased identity.");
        }
        for (int i = pass; i < 256; i += 4) { indexes.try_emplace(i, i * 2); identities.add(i); }
        for (int i = 0; i < 256; ++i)
            require(indexes.find(i) && *indexes.find(i) == i * 2 && identities.contains(i), "Collision repair corrupted the index.");
    }
    auto transferred = std::move(identities);
    require(identities.empty() && transferred.size() == 256, "Set move did not transfer membership.");
    identities.add(1000); identities.swap(transferred);
    require(identities.size() == 256 && transferred.contains(1000), "Set swap lost membership.");

    Map<int, std::unique_ptr<int>> owned;
    owned.try_emplace(1, std::make_unique<int>(11));
    auto spare = std::make_unique<int>(22);
    require(!owned.try_emplace(1, std::move(spare)).inserted && spare && **owned.find(1) == 11,
        "Duplicate insertion consumed a move-only resource.");
    owned.try_emplace(2, std::move(spare)); owned.remove(1);
    require(!spare && **owned.find(2) == 22, "Move-only dense deletion lost the surviving resource.");
    OwnedSet owned_set;
    owned_set.add(std::make_unique<int>(33));
    const auto& pointer_key = *owned_set.begin();
    require(owned_set.contains(pointer_key) && owned_set.remove(pointer_key) && owned_set.empty(),
        "Move-only Set failed to remove its borrowed key.");

    // The hash adapter reuses the Runtime's existing full identity contract.
    Runtime left, right;
    auto first = left.create(*left.find_type("core.Object"));
    auto foreign = right.create(*right.find_type("core.Object"));
    require(first.has_value() && foreign.has_value(), "Identity consumer could not create its objects.");
    Set<ObjectId> objects;
    const auto old = first->id(); objects.add(old); objects.add(foreign->id());
    require(left.destroy(old).has_value(), "Identity consumer could not destroy its object.");
    auto next = left.create(*left.find_type("core.Object"));
    require(next.has_value() && next->id() != old && objects.add(next->id()) && objects.size() == 3 &&
        objects.contains(old) && objects.contains(foreign->id()), "Set lost Runtime context or generation identity.");
    const auto unrooted = next->id(); next->reset();
    require(left.begin_collection().has_value(), "Identity consumer could not start GC.");
    while (left.collection_phase() != CollectionPhase::idle)
        require(left.collect_step({8}).has_value(), "Identity consumer could not collect.");
    require(!left.type_of(unrooted) && objects.contains(unrooted), "Ordinary identity Set became a GC root.");
}
