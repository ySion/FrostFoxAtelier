#include "foundation/array.hpp"
#include "foundation/map.hpp"
#include "foundation/set.hpp"
#include "foundation/shared_array.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using namespace atelier;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct ComparisonResult {
    operator bool() const { throw std::runtime_error("Comparison conversion failed."); }
};
struct ProxyKey {
    int id{};
    friend ComparisonResult operator==(const ProxyKey&, const ProxyKey&) noexcept { return {}; }
};
struct StoredKey {
    int id{};
};
struct Query {
    int id{};
    operator StoredKey() const { throw std::runtime_error("Query conversion failed."); }
};
// Deliberately not transparent: the backend actually needs StoredKey, even
// though these policies can also inspect Query without throwing.
struct ConvertedHash {
    std::size_t operator()(StoredKey key) const noexcept { return static_cast<std::size_t>(key.id); }
    std::size_t operator()(Query key) const noexcept { return static_cast<std::size_t>(key.id); }
};
struct ConvertedEqual {
    bool operator()(StoredKey a, StoredKey b) const noexcept { return a.id == b.id; }
    bool operator()(Query a, StoredKey b) const noexcept { return a.id == b.id; }
};
template <class C>
concept AcceptsConvertedQuery = requires(const C& values, const Query& query) { values.find(query); };

// A transferable policy resource. Moved-from policies remain destructible and
// assignable but do not themselves promise to be callable before reinitializing.
struct SeededHash {
    static inline bool used_moved_policy{};
    unsigned seed{17};
    SeededHash() noexcept = default;
    SeededHash(const SeededHash&) noexcept = default;
    SeededHash& operator=(const SeededHash&) noexcept = default;
    SeededHash(SeededHash&& other) noexcept : seed(std::exchange(other.seed, 0)) {}
    SeededHash& operator=(SeededHash&& other) noexcept {
        seed = std::exchange(other.seed, 0);
        return *this;
    }
    std::size_t operator()(int key) const noexcept {
        if (!seed)
            used_moved_policy = true;
        return static_cast<std::size_t>(key) ^ seed;
    }
};
struct SeededEqual {
    static inline bool used_moved_policy{};
    bool active{true};
    SeededEqual() noexcept = default;
    SeededEqual(const SeededEqual&) noexcept = default;
    SeededEqual& operator=(const SeededEqual&) noexcept = default;
    SeededEqual(SeededEqual&& other) noexcept : active(std::exchange(other.active, false)) {}
    SeededEqual& operator=(SeededEqual&& other) noexcept {
        active = std::exchange(other.active, false);
        return *this;
    }
    bool operator()(int a, int b) const noexcept {
        if (!active)
            used_moved_policy = true;
        return a == b;
    }
};

template <class T>
void self_operations(T& value) {
    auto* alias = std::addressof(value);
    value = *alias;
    value = std::move(*alias);
    value.swap(*alias);
}

// Terminate at the lifetime violation instead of proceeding to access the
// destroyed nested source. This check is effective in Dev and Release.
void require_detached(bool watched, bool empty) noexcept {
    if (watched && !empty) {
        std::fputs("Owned move source was destroyed before its storage was transferred.\n", stderr);
        std::exit(EXIT_FAILURE);
    }
}
struct ArrayOwner {
    Array<std::shared_ptr<ArrayOwner>> children;
    bool watch{};
    ~ArrayOwner() { require_detached(watch, children.empty()); }
};
struct MapOwner {
    Map<int, std::shared_ptr<MapOwner>> children;
    bool watch{};
    ~MapOwner() { require_detached(watch, children.empty()); }
};
struct SetOwner;
struct OwnerEqual {
    bool operator()(const std::shared_ptr<SetOwner>& a, const std::shared_ptr<SetOwner>& b) const noexcept {
        return a.get() == b.get();
    }
};
struct SetOwner {
    Set<std::shared_ptr<SetOwner>, Hash<std::shared_ptr<SetOwner>>, OwnerEqual> children;
    bool watch{};
    ~SetOwner() { require_detached(watch, children.empty()); }
};

void owned_source_moves() {
    Array<std::shared_ptr<ArrayOwner>> array;
    array.add(std::make_shared<ArrayOwner>());
    array[0]->children.add(std::make_shared<ArrayOwner>());
    auto* array_leaf = array[0]->children[0].get();
    array[0]->watch = true;
    array = std::move(array[0]->children);
    require(array.size() == 1 && array[0].get() == array_leaf, "Array subtree promotion lost its child.");

    Map<int, std::shared_ptr<MapOwner>> map;
    map.try_emplace(1, std::make_shared<MapOwner>());
    auto& map_source = (*map.find(1))->children;
    map_source.try_emplace(2, std::make_shared<MapOwner>());
    auto* map_leaf = map_source.find(2)->get();
    (*map.find(1))->watch = true;
    map = std::move(map_source);
    require(map.size() == 1 && map.find(2)->get() == map_leaf, "Map subtree promotion lost its child.");

    Set<std::shared_ptr<SetOwner>, Hash<std::shared_ptr<SetOwner>>, OwnerEqual> set;
    set.add(std::make_shared<SetOwner>());
    auto& set_source = (*set.begin())->children;
    set_source.add(std::make_shared<SetOwner>());
    auto* set_leaf = set_source.begin()->get();
    (*set.begin())->watch = true;
    set = std::move(set_source);
    require(set.size() == 1 && set.begin()->get() == set_leaf, "Set subtree promotion lost its child.");
}

template <class T>
concept Shareable = requires { typename SharedArray<T>; };

void shared_arrays() {
    static_assert(sizeof(SharedArray<int>) == sizeof(void*));
    static_assert(Shareable<String> && !Shareable<const int> && !Shareable<std::unique_ptr<int>> &&
                  !Shareable<std::shared_ptr<int>>);
    const SharedArray<String> none;
    require(none.empty() && none.array().capacity() == 0 && same(none, SharedArray<String>{}),
            "An empty handle owned storage.");

    Array<String> labels;
    labels.emplace("short");
    labels.emplace("a label longer than the inline capacity");
    const auto* buffer = labels.data();
    SharedArray<String> first{std::move(labels)};
    require(labels.empty() && first.view().data() == buffer, "Adopting an Array copied its elements.");
    auto second = first;
    require(same(first, second) && second.view().data() == buffer, "Copying a handle copied its contents.");
    auto& changed = second.write();
    changed[0] = String{"changed"};
    changed.add(String{"tail"});
    require(!same(first, second) && first.size() == 2 && first.array()[0].view() == "short" &&
                first.view().data() == buffer && second.size() == 3 && second.array()[0].view() == "changed",
            "Writing one handle changed a shared copy.");
    require(first.write().data() == buffer && second.write().data() == changed.data(),
            "Writing a unique handle copied it.");
    auto moved = std::move(second);
    require(second.empty() && !same(second, moved) && moved.size() == 3, "A moved handle kept its block.");
    second.write().add(String{"reused"});
    require(second.size() == 1 && second.array()[0].view() == "reused", "A moved-from handle could not be written.");
    self_operations(moved);
    require(moved.size() == 3 && moved.array()[2].view() == "tail", "Self assignment or swap changed a shared array.");
    auto reused = second;
    const auto* reused_buffer = reused.view().data();
    second = moved;
    require(same(second, moved) && moved.array()[0].view() == "changed", "Assignment did not share the block.");
    require(reused.write().data() == reused_buffer, "Copy assignment kept the replaced block.");
    auto watched = reused;
    reused = std::move(second);
    require(same(reused, moved) && second.empty() && watched.write().data() == reused_buffer,
            "Move assignment kept the replaced block.");

    // A lost count update either frees the block early or keeps it shared after
    // every worker copy is gone; the final in-place write detects the latter.
    Array<std::uint32_t> numbers;
    numbers.resize(4096);
    for (std::uint32_t i = 0; i != numbers.size(); ++i)
        numbers[i] = i;
    SharedArray<std::uint32_t> shared{std::move(numbers)};
    const auto* original = shared.view().data();
    std::atomic<bool> failed{};
    {
        std::array<std::jthread, 8> workers;
        for (std::uint32_t worker = 0; worker != workers.size(); ++worker)
            workers[worker] = std::jthread{[&, worker] {
                for (std::uint32_t round = 0; round != 20000; ++round) {
                    SharedArray<std::uint32_t> copy = shared;
                    if (copy.view()[round % 4096] != round % 4096)
                        failed = true;
                    if (round % 64 != worker)
                        continue;
                    auto& own = copy.write();
                    if (own.data() == original || own[worker] != worker)
                        failed = true;
                    own[worker] = ~0u;
                }
            }};
    }
    require(!failed, "Concurrent copies or writes broke sharing.");
    require(shared.write().data() == original, "Concurrent copies left the shared block counted as shared.");
    for (std::uint32_t i = 0; i != shared.size(); ++i)
        require(shared.array()[i] == i, "A worker's write reached the shared block.");
}
}

void verify_container_lifecycle() {
    // Diagnose both dangerous noexcept boundaries without deliberately causing
    // terminate in a production operation.
    bool proxy_threw = false;
    try {
        (void)static_cast<bool>(ProxyKey{} == ProxyKey{});
    } catch (const std::runtime_error&) {
        proxy_threw = true;
    }
    require(proxy_threw, "Comparison proxy did not exercise its throwing conversion.");
    const bool comparison_rejected = !noexcept(Equal<ProxyKey>{}(ProxyKey{}, ProxyKey{}));
    const bool conversion_rejected = !AcceptsConvertedQuery<Map<StoredKey, int, ConvertedHash, ConvertedEqual>>;
    if (!comparison_rejected)
        std::fputs("Comparison proxy conversion is incorrectly marked noexcept.\n", stderr);
    if (!conversion_rejected)
        std::fputs("Nontransparent query conversion bypasses the noexcept gate.\n", stderr);

    SeededHash::used_moved_policy = false;
    SeededEqual::used_moved_policy = false;
    Map<int, int, SeededHash, SeededEqual> source;
    source.try_emplace(1, 10);
    auto moved = std::move(source);
    source.try_emplace(2, 20);
    require(source.contains(2), "Move-constructed map source could not be reused.");
    Map<int, int, SeededHash, SeededEqual> assigned;
    assigned = std::move(source);
    source.try_emplace(3, 30);
    require(source.contains(3), "Move-assigned map source could not be reused.");
    Set<int, SeededHash, SeededEqual> members;
    members.add(1);
    auto moved_members = std::move(members);
    members.add(2);
    require(members.contains(2), "Move-constructed set source could not be reused.");
    Set<int, SeededHash, SeededEqual> assigned_members;
    assigned_members = std::move(members);
    members.add(3);
    require(members.contains(3), "Move-assigned set source could not be reused.");
    self_operations(moved);
    self_operations(moved_members);
    const bool policy_valid = !SeededHash::used_moved_policy && !SeededEqual::used_moved_policy;
    if (!policy_valid)
        std::fputs("Moved-from container reused an invalid policy state.\n", stderr);
    require(comparison_rejected && conversion_rejected && policy_valid, "Hash contract/lifecycle checks failed.");
    require(*moved.find(1) == 10 && *assigned.find(2) == 20 && *source.find(3) == 30 && moved_members.contains(1) &&
                assigned_members.contains(2) && members.contains(3),
            "Policy transfer changed membership.");
    owned_source_moves();
    shared_arrays();

    Array<String> labels;
    labels.emplace("short");
    labels.emplace("a longer owned label crossing inline capacity");
    self_operations(labels);
    require(labels.size() == 2 && labels[0].view() == "short", "Array self operation changed its value.");
    labels.append(labels.view());
    labels.remove_at(labels.size(), 0);
    labels.reserve(labels.capacity());
    labels.resize(labels.size());
    require(labels.size() == 4 && labels[3] == labels[1], "Idempotent Array operation changed an element.");
    for (int i = 0; i < 3; ++i) {
        labels.clear();
        labels.clear();
        labels.squeeze();
        labels.release();
        labels.release();
    }
    labels.emplace("reused");
    auto moved_labels = std::move(labels);
    labels.emplace("new");
    require(moved_labels[0].view() == "reused" && labels[0].view() == "new", "Moved-from Array could not be reused.");
    bool rejected = false;
    try {
        labels.remove_at(1, 1);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    require(rejected && labels.size() == 1 && labels.try_at(1) == nullptr, "Array rejected range changed its value.");

    for (String text :
         {String{}, String{"123456789012345"}, String{"1234567890123456"}, String{"中文\xF0\x9F\xA6\x8A"}}) {
        const auto original = text;
        self_operations(text);
        text.assign(text.view());
        text.reserve(text.capacity());
        require(text == original && text.c_str()[text.size()] == '\0',
                "String self operation lost value or terminator.");
        text.append(text.view());
        require(text.size() == original.size() * 2 && text.view().is_valid_utf8(), "Self append damaged UTF-8.");
        for (int i = 0; i < 3; ++i) {
            text.clear();
            text.squeeze();
            text.release();
        }
        text.assign(original);
        text.reserve(80);
        text.squeeze();
        auto transfer = std::move(text);
        require(transfer == original && text.empty() && text.c_str()[0] == '\0',
                "String move retained source content.");
        text.append("ready");
        const auto before = text;
        rejected = false;
        try {
            text.append(StringView{"\xf0\x80\x80\x80", 4});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && text == before, "Invalid UTF-8 append changed its owner.");
    }
    const Span<int> empty;
    require(empty.slice(0).empty() && empty.begin() == empty.end(), "Empty Span slice is invalid.");
    const StringView empty_text;
    require(empty_text.slice(0).empty() && empty_text.is_valid_utf8(), "Empty text view is invalid.");
    std::array<int, 2> pair{1, 2};
    const Span<int> view{pair.data(), pair.size()};
    require(view.slice(2, 0).empty() && view.slice(2).data() == pair.data() + 2,
            "End Span slice changed its position.");
    rejected = false;
    try {
        (void)view.slice(1, 2);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    require(rejected && pair == std::array{1, 2}, "Invalid view range was admitted or changed storage.");

    Map<String, String> names;
    Set<String> unique;
    names.try_emplace("name", String{"value"});
    unique.add("name");
    self_operations(names);
    self_operations(unique);
    require(names.size() == 1 && names.find("name")->view() == "value" && unique.contains("name"),
            "Self assignment or swap changed a hash collection.");
    names.reserve(128);
    unique.reserve(128);
    names.reserve(1);
    unique.reserve(1);
    for (int i = 0; i < 3; ++i) {
        require(!names.try_emplace("name", String{"ignored"}).inserted && !unique.add("name"),
                "Repeated insertion was not idempotent.");
        require(!names.remove("missing") && !unique.remove("missing"), "Removing a missing key changed membership.");
    }
    require(names.remove("name") && !names.remove("name") && unique.remove("name") && !unique.remove("name"),
            "Repeated removal did not report presence correctly.");
    for (int i = 0; i < 3; ++i) {
        names.clear();
        unique.clear();
        names.release();
        unique.release();
        require(names.begin() == names.end() && unique.begin() == unique.end(),
                "Released container iteration was nonempty.");
    }
    names.try_emplace("after", String{"release"});
    unique.add("after");
    require(names.find("after")->view() == "release" && unique.contains("after"),
            "Released collections could not be reused.");
}
