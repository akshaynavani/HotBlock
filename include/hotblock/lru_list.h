#pragma once

#include <cassert>
#include <cstddef>

namespace hotblock {

// Hook embedded in any object that can live on an LruList.
struct LruNode {
  LruNode* prev = nullptr;
  LruNode* next = nullptr;

  bool linked() const { return next != nullptr; }
};

// Intrusive, non-owning doubly linked list ordered from most- to least-recently
// used. All operations are O(1) and allocation-free. Not thread-safe; callers
// (cache shards) hold their own lock.
//
// T must publicly derive from LruNode.
template <typename T>
class LruList {
 public:
  LruList() { head_.prev = head_.next = &head_; }

  LruList(const LruList&) = delete;
  LruList& operator=(const LruList&) = delete;

  bool empty() const { return head_.next == &head_; }
  std::size_t size() const { return size_; }

  // Inserts `item` as the most recently used element.
  void PushFront(T* item) {
    LruNode* n = item;
    assert(!n->linked());
    n->next = head_.next;
    n->prev = &head_;
    head_.next->prev = n;
    head_.next = n;
    ++size_;
  }

  void Remove(T* item) {
    LruNode* n = item;
    assert(n->linked());
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = nullptr;
    --size_;
  }

  void MoveToFront(T* item) {
    Remove(item);
    PushFront(item);
  }

  T* Front() const { return empty() ? nullptr : static_cast<T*>(head_.next); }
  // Least recently used element, or nullptr when empty.
  T* Back() const { return empty() ? nullptr : static_cast<T*>(head_.prev); }

  T* PopBack() {
    T* victim = Back();
    if (victim != nullptr) Remove(victim);
    return victim;
  }

  // Visits elements from LRU to MRU. `fn` returns false to stop early.
  template <typename Fn>
  void ForEachFromBack(Fn&& fn) const {
    for (LruNode* n = head_.prev; n != &head_;) {
      LruNode* prev = n->prev;  // allow fn to unlink the current node
      if (!fn(static_cast<T*>(n))) return;
      n = prev;
    }
  }

 private:
  mutable LruNode head_;  // sentinel
  std::size_t size_ = 0;
};

}  // namespace hotblock
