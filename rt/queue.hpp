// Bounded multi producer single consumer queue with explicit backpressure
// accounting. Mutex and condition variable based, batch APIs to keep lock
// traffic low at high frame rates.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rt {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : cap_(capacity), buf_(capacity) {}

  // Blocking push: waits for space, never drops. Returns false only if the
  // queue was closed while waiting.
  bool push_batch(const T* items, size_t n) {
    size_t pushed = 0;
    std::unique_lock<std::mutex> lk(mu_);
    while (pushed < n) {
      not_full_.wait(lk, [&] { return count_ < cap_ || closed_; });
      if (closed_) return false;
      size_t k = std::min(n - pushed, cap_ - count_);
      copy_in(items + pushed, k);
      pushed += k;
      accepted_.fetch_add(k, std::memory_order_relaxed);
      not_empty_.notify_one();
    }
    return true;
  }

  bool push(const T& item) { return push_batch(&item, 1); }

  // Non blocking push: accepts what fits, drops and counts the rest.
  // Returns the number accepted.
  size_t try_push_batch(const T* items, size_t n) {
    size_t k = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!closed_) {
        k = std::min(n, cap_ - count_);
        copy_in(items, k);
      }
    }
    accepted_.fetch_add(k, std::memory_order_relaxed);
    dropped_.fetch_add(n - k, std::memory_order_relaxed);
    if (k) not_empty_.notify_one();
    return k;
  }

  bool try_push(const T& item) { return try_push_batch(&item, 1) == 1; }

  // Pop up to max_items, appending to out. Blocks until data is available or
  // the queue is closed and drained. Returns the number popped, 0 at end.
  size_t pop_batch(std::vector<T>& out, size_t max_items) {
    std::unique_lock<std::mutex> lk(mu_);
    not_empty_.wait(lk, [&] { return count_ > 0 || closed_; });
    size_t k = std::min(count_, max_items);
    for (size_t i = 0; i < k; ++i) {
      out.push_back(buf_[head_]);
      head_ = (head_ + 1) % cap_;
    }
    count_ -= k;
    if (k) not_full_.notify_all();
    return k;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  uint64_t accepted() const { return accepted_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return count_;
  }

 private:
  void copy_in(const T* items, size_t k) {
    size_t tail = (head_ + count_) % cap_;
    for (size_t i = 0; i < k; ++i) {
      buf_[tail] = items[i];
      tail = (tail + 1) % cap_;
    }
    count_ += k;
  }

  size_t cap_;
  std::vector<T> buf_;
  size_t head_ = 0;
  size_t count_ = 0;
  bool closed_ = false;
  mutable std::mutex mu_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace rt
