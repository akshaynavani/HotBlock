#include "hotblock/lru_list.h"

#include <gtest/gtest.h>

#include <vector>

namespace hotblock {
namespace {

struct Item : LruNode {
  explicit Item(int v) : value(v) {}
  int value;
};

std::vector<int> BackToFront(const LruList<Item>& list) {
  std::vector<int> out;
  list.ForEachFromBack([&](Item* it) {
    out.push_back(it->value);
    return true;
  });
  return out;
}

TEST(LruListTest, EmptyList) {
  LruList<Item> list;
  EXPECT_TRUE(list.empty());
  EXPECT_EQ(list.size(), 0u);
  EXPECT_EQ(list.Back(), nullptr);
  EXPECT_EQ(list.PopBack(), nullptr);
}

TEST(LruListTest, PushFrontOrdersByRecency) {
  LruList<Item> list;
  Item a(1), b(2), c(3);
  list.PushFront(&a);
  list.PushFront(&b);
  list.PushFront(&c);
  EXPECT_EQ(list.size(), 3u);
  EXPECT_EQ(list.Front(), &c);
  EXPECT_EQ(list.Back(), &a);
  EXPECT_EQ(BackToFront(list), (std::vector<int>{1, 2, 3}));
}

TEST(LruListTest, MoveToFrontPromotes) {
  LruList<Item> list;
  Item a(1), b(2), c(3);
  list.PushFront(&a);
  list.PushFront(&b);
  list.PushFront(&c);
  list.MoveToFront(&a);
  EXPECT_EQ(BackToFront(list), (std::vector<int>{2, 3, 1}));
}

TEST(LruListTest, RemoveUnlinks) {
  LruList<Item> list;
  Item a(1), b(2), c(3);
  list.PushFront(&a);
  list.PushFront(&b);
  list.PushFront(&c);
  list.Remove(&b);
  EXPECT_FALSE(b.linked());
  EXPECT_EQ(list.size(), 2u);
  EXPECT_EQ(BackToFront(list), (std::vector<int>{1, 3}));
}

TEST(LruListTest, PopBackDrainsInLruOrder) {
  LruList<Item> list;
  Item a(1), b(2), c(3);
  list.PushFront(&a);
  list.PushFront(&b);
  list.PushFront(&c);
  EXPECT_EQ(list.PopBack(), &a);
  EXPECT_EQ(list.PopBack(), &b);
  EXPECT_EQ(list.PopBack(), &c);
  EXPECT_TRUE(list.empty());
}

TEST(LruListTest, ForEachAllowsUnlinkingCurrent) {
  LruList<Item> list;
  Item items[] = {Item(0), Item(1), Item(2), Item(3)};
  for (auto& it : items) list.PushFront(&it);
  list.ForEachFromBack([&](Item* it) {
    if (it->value % 2 == 0) list.Remove(it);
    return true;
  });
  EXPECT_EQ(BackToFront(list), (std::vector<int>{1, 3}));
}

}  // namespace
}  // namespace hotblock
