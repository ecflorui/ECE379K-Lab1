// locks.h -- Lab 1: Part 5 and Part 6.  YOU WRITE THIS FILE.
//
// What the harness and tests expect from this header:
//
//   Part 5 (BasicLock):     TASLock  TTASLock  TicketLock  ParkingLock
//   Part 6 (SharedLock):    RWLock   RWLockWP
//
// Every lock is a class with a default constructor and
//
//   void lock();
//   void unlock();
//
// and the two reader-writer locks additionally
//
//   void lock_shared();
//   void unlock_shared();
//
// so that std::lock_guard, std::unique_lock, std::shared_lock, and
// the lab's ReadGuard all work on them.  A lock must not be copyable
// or movable (std::atomic already sees to that).
//
// Everything in here is built from std::atomic.  No std::mutex, no
// std::shared_mutex, no OS primitives except the ones behind
// std::atomic::wait / notify_one, which ParkingLock uses.
//
// Flip HAVE_LOCKS (Part 5) and HAVE_RW (Part 6) in parts.h as each
// set compiles.

#ifndef LOCKS_H
#define LOCKS_H

#include <atomic>
#include <cstdint>
#include <thread>

#include "interface.h"

//processor hint for spin loops, doesn't give up the thread's timeslice
inline void lab_spin_pause(){
    #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
    #elif defined(__aarch64__)
       asm volatile("yield");
    #else
       std::atomic_signal_fence(std::memory_order_seq_cst);
    #endif
}

//part 5: TASLock
class TASLock{
    private:
        std::atomic<bool> held_{false};

    public:
       void lock() {
    //exchange returns the old value, false means we acquired the lock
    while (held_.exchange(true, std::memory_order_acquire)) {
        lab_spin_pause(); //processor hint while waiting for the lock
    }
}

        void unlock(){ 
            //release makes protected writes visible to other threads
            held_.store(false, std::memory_order_release);
        }
};

//part 5: TTAS Lock
class TTASLock{
    private:
        std::atomic<bool> held_{false};

    public:
        void lock(){
            unsigned backoff = 1;
            constexpr unsigned max_backoff = 64;

            for(;;){
                //read while held insead of repeatedly writing to the lock's cache line
                while(held_.load(std::memory_order_relaxed)){
                    lab_spin_pause();
                }

                //looks free, try to acquire
                if(!held_.exchange(true, std::memory_order_acquire)){
                    return;
                }

                //another thread won, pause before trying again
                for(unsigned i = 0; i < backoff; ++i){
                    lab_spin_pause();
                }

                if(backoff < max_backoff){
                    backoff *= 2; //double delay up to the cap
                }

            }
        }

        void unlock(){
            held_.store(false, std::memory_order_release);
        }
};

//part 5: Ticket Lock
class TicketLock{
    private:
        std::atomic<std::uint32_t> next_{0};
        std::atomic<std::uint32_t> serving_{0};

    public:
        void lock(){
            //reserve our place in the line, doesn't actually grant access yet
            const auto ticket = next_.fetch_add(1, std::memory_order_relaxed);

            unsigned spins = 0;
            constexpr unsigned spin_limit = 64;

            //only enter when our ticket is being served
            while(serving_.load(std::memory_order_acquire) != ticket){
                lab_spin_pause();

                if(++spins == spin_limit){
                    //yield so the thread whose ticket is next has a chance to run
                    std::this_thread::yield();
                    spins = 0;
            }
        }
    }

        void unlock(){
            //increment the serving counter to allow the next ticket to enter
            serving_.fetch_add(1, std::memory_order_release);
        }
};

//part 5:  Parking Lock
class ParkingLock{
    private:
        //0 = free, 1 = held with no waiters, 2 = held with possible sleepers
        std::atomic<int> state_{0};

    public:
        void lock(){
            int expected = 0;

        //fast path, acquire immmediately if free
        if(state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)){
            return;
        }

        //spin briefly before paying cost of sleep
        constexpr unsigned spin_limit = 64;

        for (unsigned i = 0; i<spin_limit; ++i){
            lab_spin_pause();

            //only checking if free here, the successful compare_exchange below provides acquire ordering
            if(state_.load(std::memory_order_relaxed) == 0){
                expected = 0;
                if(state_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)){
                    return;
                }
            }
        }
        //mark possible sleepers, if old state was 0, we just acquired the lock, otherwise we are now in the "sleepers" state
        while (state_.exchange(2, std::memory_order_acquire) != 0){
            state_.wait(2, std::memory_order_relaxed);
            //waking up doesn't guarantee we acquired the lock, so try again
        }
    }

    void unlock(){
        //release the lock, if there are sleepers, wake one up
        if(state_.exchange(0, std::memory_order_release) == 2){
            state_.notify_one();
        }
    }
};


class RWLock{
    private:
        std::atomic<int> state_{0};
    public:

        //reader: CAS n -> n+1, but only while no writer holds it (n >= 0)

        void lock_shared(){ //reader enters

            unsigned backoff = 1;
            constexpr unsigned max_backoff = 64;

            for(;;){
                int s = state_.load(std::memory_order_relaxed); //check the current state
 
                //a  writer holds the state, wait for now until it's done 
                while(s < 0){
                    lab_spin_pause();
                    s = state_.load(std::memory_order_relaxed);
                }
 
                //if it's still s, add one for us and we are in
                if(state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed)){
                    return;
                }
 
                //we lost the race, not the expected s(another reader moved the count, or a writer got in) so we wait
                for(unsigned i = 0; i < backoff; ++i){
                    lab_spin_pause();
                }
                if(backoff < max_backoff){
                    backoff *= 2; //wait a bit longer each time
                }
            }
        }
 
        void unlock_shared(){ //reader leaves
            //release: this reader's reads finish before a writer can enter
            state_.fetch_sub(1, std::memory_order_release);
        }
 
        //writer: CAS 0 -> -1, only when nobody (reader or writer) holds it
        void lock(){
            unsigned backoff = 1;
            constexpr unsigned max_backoff = 64;
 
            for(;;){
                //wait until the lock looks completely free
                while(state_.load(std::memory_order_relaxed) != 0){
                    lab_spin_pause();
                }
 
                int expected = 0; //got to the lock
                if(state_.compare_exchange_weak(expected, -1, std::memory_order_acquire, std::memory_order_relaxed)){
                    return;
                }
 
                for(unsigned i = 0; i < backoff; ++i){
                    lab_spin_pause();
                }
                if(backoff < max_backoff){
                    backoff *= 2;
                }
            }
        }
 
        void unlock(){ //writer leaves
            state_.store(0, std::memory_order_release);
        }
};

class RWLockWP{ //this is writer preferring
    private:
        std::atomic<int> state_{0};             //-1 = writer, n >= 0 means n readers
        std::atomic<int> waiting_writers_{0};   //writers that want in but don't hold the lock just yet
 
    public:
        void lock_shared(){
            unsigned backoff = 1;
            constexpr unsigned max_backoff = 64;
 
            for(;;){
                //defer to waiting writers but don't even try while one is queued
                while(waiting_writers_.load(std::memory_order_relaxed) > 0){
                    lab_spin_pause();
                }
 
                int s = state_.load(std::memory_order_relaxed);
                if(s >= 0 &&
                   state_.compare_exchange_weak(s, s + 1, std::memory_order_acquire, std::memory_order_relaxed)){
                    return;
                }
 
                for(unsigned i = 0; i < backoff; ++i){
                    lab_spin_pause();
                }
                if(backoff < max_backoff){
                    backoff *= 2;
                }
            }
        }
 
        void unlock_shared(){
            state_.fetch_sub(1, std::memory_order_release);
        }
 
        void lock(){
            //announce ourselves first, so new readers stop entering
            waiting_writers_.fetch_add(1, std::memory_order_relaxed);
 
            unsigned backoff = 1;
            constexpr unsigned max_backoff = 64;
 
            for(;;){
                //existing readers drain; no new ones arrive while we are waiting
                while(state_.load(std::memory_order_relaxed) != 0){
                    lab_spin_pause();
                }
 
                int expected = 0;
                if(state_.compare_exchange_weak(expected, -1, std::memory_order_acquire, std::memory_order_relaxed)){
                    break;
                }
 
                for(unsigned i = 0; i < backoff; ++i){
                    lab_spin_pause();
                }
                if(backoff < max_backoff){
                    backoff *= 2;
                }
            }
 
            //we hold it now; stop counting as waiting
            waiting_writers_.fetch_sub(1, std::memory_order_relaxed);
        }
 
        void unlock(){
            state_.store(0, std::memory_order_release);
        }
};


#endif /* LOCKS_H */
