// JULES compiler — core support containers.
// Project laws honored here:
//   * No std::unordered_map / std::set anywhere in the compiler (FlatMap /
//     open-addressing with deterministic probe order instead).
//   * SmallVector with small-buffer optimization for hot-path lists.
//   * Bump arena for bulk-freed compiler artifacts (symbol strings, dumps).
//   * -fno-exceptions / -fno-rtti: no throw, no typeid, no dynamic_cast.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace jules {

using i8  = int8_t;   using i16 = int16_t; using i32 = int32_t; using i64 = int64_t;
using u8  = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using f32 = float;    using f64 = double;

// Deterministic FNV-1a for content hashing (GVN hash-consing, symbol table).
inline constexpr u64 kFnvOffset = 1469598103934665603ull;
inline constexpr u64 kFnvPrime  = 1099511628211ull;
constexpr u64 fnv1a(const void* data, size_t n, u64 seed = kFnvOffset) {
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= kFnvPrime; }
    return h;
}
inline u64 hash_mix(u64 a, u64 b) { return fnv1a(&b, sizeof b, a); }

// ---------------------------------------------------------------------------
// SmallVec<T,N>: vector with N inline elements (small-buffer optimization).
// Falls back to heap for larger sizes. Grows by 2x. Move-only friendly.
// ---------------------------------------------------------------------------
template <typename T, size_t N>
class SmallVec {
public:
    SmallVec() = default;
    explicit SmallVec(size_t n, const T& v = T{}) { resize(n, v); }
    SmallVec(std::initializer_list<T> il) {
        reserve(il.size());
        for (const T& e : il) push_back(e);
    }
    SmallVec(const SmallVec& o) { copy_from(o); }
    SmallVec& operator=(const SmallVec& o) {
        if (this != &o) { clear(); copy_from(o); }
        return *this;
    }
    SmallVec(SmallVec&& o) noexcept { move_from(std::move(o)); }
    SmallVec& operator=(SmallVec&& o) noexcept {
        if (this != &o) { free_heap(); move_from(std::move(o)); }
        return *this;
    }
    ~SmallVec() { free_heap(); }

    T* begin() { return start(); }
    T* end() { return start() + size_; }
    const T* begin() const { return start(); }
    const T* end() const { return start() + size_; }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T& operator[](size_t i) { assert(i < size_); return start()[i]; }
    const T& operator[](size_t i) const { assert(i < size_); return start()[i]; }

    void push_back(const T& v) {
        grow_if(size_ + 1);
        start()[size_++] = v;
    }
    void push_back(T&& v) {
        grow_if(size_ + 1);
        start()[size_++] = std::move(v);
    }
    void pop_back() { assert(size_ > 0); start()[--size_].~T(); }
    void clear() { while (size_) pop_back(); }
    void reserve(size_t n) { if (n > cap()) reallocate(n); }
    void resize(size_t n, const T& v = T{}) {
        if (n > size_) { reserve(n); while (size_ < n) push_back(v); }
        else { while (size_ > n) pop_back(); }
    }
    T& back() { assert(size_); return start()[size_ - 1]; }
    const T& back() const { assert(size_); return start()[size_ - 1]; }

    bool operator==(const SmallVec& o) const {
        if (size_ != o.size_) return false;
        for (size_t i = 0; i < size_; ++i) if (!((*this)[i] == o[i])) return false;
        return true;
    }

private:
    size_t cap() const { return is_inline() ? N : heap_cap_; }
    bool is_inline() const { return heap_ == nullptr; }
    T* start() { return is_inline() ? reinterpret_cast<T*>(&inline_) : heap_; }
    const T* start() const { return is_inline() ? reinterpret_cast<const T*>(&inline_) : heap_; }

    void grow_if(size_t need) {
        if (need <= cap()) return;
        reallocate(need < 4 ? 4 : (need + need / 2));
    }
    void reallocate(size_t newcap) {
        T* nheap = static_cast<T*>(::operator new(newcap * sizeof(T)));
        for (size_t i = 0; i < size_; ++i) {
            new (&nheap[i]) T(std::move_if_noexcept(start()[i]));
            start()[i].~T();
        }
        free_heap();
        heap_ = nheap; heap_cap_ = newcap;
    }
    void free_heap() {
        if (!is_inline()) { ::operator delete(heap_); heap_ = nullptr; heap_cap_ = 0; }
    }
    void copy_from(const SmallVec& o) {
        reserve(o.size_);
        for (const T& e : o) push_back(e);
    }
    void move_from(SmallVec&& o) {
        if (o.is_inline()) {
            for (size_t i = 0; i < o.size_; ++i) push_back(std::move(o[i]));
        } else {
            heap_ = o.heap_; heap_cap_ = o.heap_cap_; size_ = o.size_;
            o.heap_ = nullptr; o.heap_cap_ = 0; o.size_ = 0;
        }
    }

    // Capacity bookkeeping. size_ always live; heap_cap_ valid only if heap_.
    // Layout note: we keep size_ first for cache locality of the common query.
    size_t size_ = 0;
    size_t heap_cap_ = 0;
    T* heap_ = nullptr;
    alignas(T) unsigned char inline_[sizeof(T) * N];
};

// ---------------------------------------------------------------------------
// FlatMap<K,V>: sorted-vector associative map. Deterministic iteration order
// (key order), O(log n) lookup, O(n) insert. Used instead of std::unordered_map
// per project law (also guarantees reproducible output).
// ---------------------------------------------------------------------------
template <typename K, typename V>
class FlatMap {
public:
    using Entry = std::pair<K, V>;
    const V* find(const K& k) const {
        size_t i = lower(k);
        if (i < entries_.size() && entries_[i].first == k) return &entries_[i].second;
        return nullptr;
    }
    V* find(const K& k) {
        size_t i = lower(k);
        if (i < entries_.size() && entries_[i].first == k) return &entries_[i].second;
        return nullptr;
    }
    V& insert(const K& k, const V& v) {
        size_t i = lower(k);
        if (i < entries_.size() && entries_[i].first == k) { entries_[i].second = v; return entries_[i].second; }
        entries_.insert(entries_.begin() + i, Entry{k, v});
        return entries_[i].second;
    }
    V& operator[](const K& k) {
        size_t i = lower(k);
        if (i < entries_.size() && entries_[i].first == k) return entries_[i].second;
        entries_.insert(entries_.begin() + i, Entry{k, V{}});
        return entries_[i].second;
    }
    bool erase(const K& k) {
        size_t i = lower(k);
        if (i < entries_.size() && entries_[i].first == k) { entries_.erase(entries_.begin() + i); return true; }
        return false;
    }
    bool contains(const K& k) const { return find(k) != nullptr; }
    const std::vector<Entry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void clear() { entries_.clear(); }

private:
    size_t lower(const K& k) const {
        size_t lo = 0, hi = entries_.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (entries_[mid].first < k) lo = mid + 1; else hi = mid;
        }
        return lo;
    }
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// Arena: chunked monotonic bump allocator. Bulk-free on destruction; used for
// symbol interning storage, IR text dumps and other wholesale-freed artifacts.
// Allocation is pointer-bump; OOM aborts (no-exceptions policy).
// ---------------------------------------------------------------------------
class Arena {
public:
    Arena() { new_chunk(kDefaultChunk); }
    ~Arena() {
        for (Chunk& c : chunks_) ::operator delete(c.mem);
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(size_t bytes, size_t align = 8) {
        offset_ = (offset_ + align - 1) & ~(align - 1);
        if (offset_ + bytes > current_.size) new_chunk(bytes > kDefaultChunk ? bytes : kDefaultChunk);
        void* p = current_.mem + offset_;
        offset_ += bytes;
        return p;
    }
    template <typename T>
    T* create(size_t n = 1) { return static_cast<T*>(allocate(n * sizeof(T), alignof(T))); }

private:
    static constexpr size_t kDefaultChunk = 64 * 1024;
    struct Chunk { u8* mem; size_t size; };
    void new_chunk(size_t sz) {
        current_.mem = static_cast<u8*>(::operator new(sz));
        current_.size = sz;
        offset_ = 0;
        chunks_.push_back(current_);
    }
    Chunk current_{};
    size_t offset_ = 0;
    std::vector<Chunk> chunks_;
};

} // namespace jules
