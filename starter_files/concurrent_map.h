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

#endif /* CONCURRENT_MAP_H */
