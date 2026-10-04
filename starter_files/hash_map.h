// hash_map.h -- Lab 1: Part 7.  YOU WRITE THIS FILE.
//
// What the harness and tests expect from this header:
//
//   template <typename K, typename V,
//             class Lock = std::mutex, bool Padded = true>
//       requires BasicLock<Lock>
//   class StripedHashMap;
//       explicit StripedHashMap(std::size_t nbuckets,
//                               std::size_t nstripes);
//       std::size_t bucket_count() const;
//       std::size_t stripe_count() const;
//
// satisfying ConcurrentMap<M, K, V> from interface.h.  The bucket
// count is fixed at construction: there is no resizing.  The tests
// construct one with 7 buckets and 3 stripes and fill it with
// thousands of keys, so long chains must work, just slowly.
//
// Flip HAVE_HASHED in parts.h when it compiles.

#ifndef HASH_MAP_H
#define HASH_MAP_H

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

#include "interface.h"

template <typename K, typename V,class Lock = std::mutex, bool Padded = true> requires BasicLock<Lock>

class StripedHashMap {
 
private:
    struct Node {
        K     key;
        V     value;
        Node* next;
    };


    struct StripeData {
        mutable Lock mu;           
        std::size_t  count = 0;     //protected by mu
    };

    //padding like we did before
    struct alignas(Padded ? CACHE_LINE : alignof(StripeData))
    Stripe : StripeData {};
 
    static_assert(!Padded || sizeof(Stripe) % CACHE_LINE == 0);
 
    std::vector<Node*>  buckets_;   //chain heads
    std::vector<Stripe> stripes_;

    //creating indices below
 
    std::size_t bucket_index(const K& key) const { //which bucket we fall into
        return std::hash<K>{}(key) % buckets_.size();
    }
 
    //contiguous range mapping aka what buckets fall into what stripes
    std::size_t stripe_of(std::size_t b) const {
        return b * stripes_.size() / buckets_.size();
    }
 
public:
    //setup and teardown stuff

    StripedHashMap(std::size_t nbuckets, std::size_t nstripes)
        : buckets_(nbuckets, nullptr), stripes_(nstripes) {
        if (nbuckets == 0 || nstripes == 0) {
            throw std::invalid_argument("bucket and stripe counts must be positive");
        }
    }
 
    StripedHashMap(const StripedHashMap&) = delete;            //each stripe owns a lock
    StripedHashMap& operator=(const StripedHashMap&) = delete;
 
    //nodes are heap-allocated, so free every chain
    ~StripedHashMap() {
        for (Node* head : buckets_) {
            while (head) {
                Node* next = head->next;
                delete head;
                head = next;
            }
        }
    }
 
    std::size_t bucket_count() const { return buckets_.size(); }  //fixed and therefore no lock needed
    std::size_t stripe_count() const { return stripes_.size(); }
 
    //true if the key was new, but otherwise overwrite its value and return false
    bool insert(const K& key, const V& value) {

        const std::size_t b = bucket_index(key); //which bucket is the key
        Stripe& s = stripes_[stripe_of(b)]; //which stripe does this bucket belong to 
        std::lock_guard<Lock> g(s.mu);         //and so we pn;y lock this bucket's stripe
 
        for (Node* n = buckets_[b]; n; n = n->next) {
            if (n->key == key) {
                n->value = value; //existing key: overwrite
                return false;
            }
        }

        buckets_[b] = new Node{key, value, buckets_[b]};   //for new keys, we push at chain head
        ++s.count;
        return true;
    }
 
    //copy the value out while the stripe's lock is still held
    bool find(const K& key, V& out) const {

        const std::size_t b = bucket_index(key);
        const Stripe& s = stripes_[stripe_of(b)];
        ReadGuard<Lock> g(s.mu); //shared if the lock supports it
 
        for (const Node* n = buckets_[b]; n; n = n->next) {
            if (n->key == key) {
                out = n->value;
                return true;
            }
        }
        return false;
    }
 
    //unlink and free the node if its present
    bool erase(const K& key) {

        const std::size_t b = bucket_index(key);
        Stripe& s = stripes_[stripe_of(b)];
        std::lock_guard<Lock> g(s.mu);
 
        for (Node** link = &buckets_[b]; *link; link = &(*link)->next) {
            if ((*link)->key == key) {
                Node* dead = *link;
                *link = dead->next;
                delete dead;
                --s.count;
                return true;
            }
        }
        return false;
    }
 
    //exact: hold every stripe's lock at once, taken in index order to prevent deadlock
    std::size_t size() const {
        std::vector<std::unique_lock<Lock>> guards;
        guards.reserve(stripes_.size()); //allocate before taking any lock
 
        for (const auto& s : stripes_) {
            guards.emplace_back(s.mu);
        }
 
        std::size_t total = 0;
        for (const auto& s : stripes_) {
            total += s.count;
        }
        return total; //guards release every lock on return
    }

};

#endif /* HASH_MAP_H */
