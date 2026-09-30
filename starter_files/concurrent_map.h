// concurrent_map.h -- Lab 1: Part 1 and Part 4.  YOU WRITE THIS FILE.
//
// Nothing compiles until CoarseMap exists, so it is the first thing
// to write.  What the harness and tests expect from this header:
//
//   template <typename K, typename V>
//   class CoarseMap;                               // Part 1
//
//   template <typename K, typename V,
//             class Lock = std::mutex, bool Padded = true>
//       requires BasicLock<Lock>
//   class ShardedMap;                              // Part 4
//       explicit ShardedMap(std::size_t nshards);
//       std::size_t shard_count() const;
//
// Both must satisfy ConcurrentMap<M, K, V> from interface.h:
//
//   bool insert(const K& key, const V& value);   // true if key was new
//   bool find  (const K& key, V& out) const;     // copy; false if absent
//   bool erase (const K& key);                   // true if was present
//   std::size_t size() const;
//
// Values come out as COPIES, never references or iterators into the
// container.  The report asks why.
//
// Flip HAVE_SHARDED in parts.h when ShardedMap compiles.

#ifndef CONCURRENT_MAP_H
#define CONCURRENT_MAP_H

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <stdexcept>

#include "interface.h"

template <typename K, typename V>
class CoarseMap {

public:
    CoarseMap() = default;
    CoarseMap(const CoarseMap&) = delete; //del to ensure thread safety
    CoarseMap& operator=(const CoarseMap&) = delete;
 
    // true if the key was new.  An existing key keeps its old value
    bool insert(const K& key, const V& value) {
        std::lock_guard<std::mutex> g(mu_); //we lock with lock_guard, RAII so will destruct on scope exit
        return map_.insert_or_assign(key, value).second; //second cal tells us if a new entry was actually created, or just overwrite (false)
    }
 
    // to lookup a a key and opies the value out while the lock is still held to ensure no conflict
    bool find(const K& key, V& out) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = map_.find(key); // iterator to find key

        if (it == map_.end()) 
            return false;

        out = it->second; //if found, copy
        return true;
    }
 
    //to remove node associated with a key    
    bool erase(const K& key) {
        std::lock_guard<std::mutex> g(mu_);
        return map_.erase(key) == 1; //keys are unique so 0 or 1 if actually was there and got removed
    }
 
    //must be locked in case its being modified     
    std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return map_.size();
    }
 
private:
    mutable std::mutex mu_;   // mutable because find() and size() are const
    std::map<K, V> map_;
};
 
// Part 4: ShardedMap 
template <typename K, typename V,
            class Lock = std::mutex, bool Padded = true>
    requires BasicLock<Lock>
class ShardedMap {

private:
    struct ShardData{
        mutable Lock mu; //mutable because find and size are const
        std::map<K, V> map;
    };

    //if padded, align each shard to a cache line so neighboring shards don't share
    struct alignas(Padded ? CACHE_LINE : alignof(ShardData)) 
    Shard: ShardData {};

    //check that a padded shard takes up a whole number of cache lines
    static_assert(!Padded || sizeof(Shard) % CACHE_LINE == 0);

    std::vector<Shard> shards_;

    //same key always goes to same shard
    std::size_t shard_index(const K& key) const {
        return std::hash<K>{}(key) % shards_.size();
    }

public:
    explicit ShardedMap(std::size_t nshards) : shards_(nshards) { //create all maps + locks at beginning, no resizing
        if (nshards == 0) {
            throw std::invalid_argument("shard count must be positive");
        }
    }

    ShardedMap( const ShardedMap&) = delete; //del because each shard owns a lock (ensure thread safety)
    ShardedMap& operator=(const ShardedMap&) = delete;

    std::size_t shard_count() const {
        return shards_.size(); //no lock needed since shard count doesn't change
    }
    
    //true if the key was new, otherwise update it's value and return false
    bool insert(const K& key, const V& value) {
        auto& shard = shards_[shard_index(key)];
        std::lock_guard<Lock> g(shard.mu); //only lock the shard the key belongs to

        return shard.map.insert_or_assign(key, value).second;
    }

    //lookup a key and copy value while shard's lock is still held
    bool find(const K& key, V& out) const {
        const auto& shard = shards_[shard_index(key)];
        ReadGuard<Lock> g(shard.mu); //shared lock if supported, otherwise exclusive lock
        auto it = shard.map.find(key);

        if (it == shard.map.end()) 
            return false;

        out = it->second; //if found, copy before releasing
        return true;
    }

    //remove ethe key while holding it's shard's lock
    bool erase(const K& key) {
        auto& shard = shards_[shard_index(key)];
        std::lock_guard<Lock> g(shard.mu);
        return shard.map.erase(key) == 1; //0 or 1 since keys are unique
    }

    //hold every lock at once so total is an exact snapshot
    std::size_t size() const {
        std::vector<std::unique_lock<Lock>> guards;
        guards.reserve(shards_.size()); //allocate space before acquiring locks

        //take locks in index order to avoid deadlock
        for (const auto& shard : shards_) {
            guards.emplace_back(shard.mu);
        }

        std::size_t total = 0;

        //all shards now locked, nothing can change while summing sizes
        for (const auto& shard : shards_) {
            total += shard.map.size();
        }
        return total; //guards destruct on scope exit and release all locks
    }
};
#endif /* CONCURRENT_MAP_H */
