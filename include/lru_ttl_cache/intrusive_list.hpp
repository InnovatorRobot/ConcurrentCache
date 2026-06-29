#ifndef LRU_TTL_CACHE_INTRUSIVE_LIST_HPP
#define LRU_TTL_CACHE_INTRUSIVE_LIST_HPP

namespace lru_ttl_cache
{
namespace detail
{

/**
 * @brief Minimal non-owning intrusive doubly-linked list.
 *
 * The list threads through @p Node objects via two member pointers selected at
 * compile time, so a single @p Node can participate in several independent
 * lists (e.g. an LRU list and an expiry list) without extra allocations. The
 * list never owns or destroys nodes; it only maintains the link fields and the
 * head/tail pointers. All operations are O(1).
 *
 * @tparam Node Node type containing the link fields.
 * @tparam Prev Pointer-to-member of the "previous" link field.
 * @tparam Next Pointer-to-member of the "next" link field.
 */
template <typename Node, Node *Node::*Prev, Node *Node::*Next>
class IntrusiveList
{
public:
    /// @return The front node (or nullptr if empty).
    Node *head() const noexcept { return head_; }

    /// @return The back node (or nullptr if empty).
    Node *tail() const noexcept { return tail_; }

    /// @return Whether the list currently has no nodes.
    bool empty() const noexcept { return head_ == nullptr; }

    /// Insert @p n at the front of the list.
    void pushFront(Node *n) noexcept
    {
        n->*Prev = nullptr;
        n->*Next = head_;
        if (head_ != nullptr)
        {
            head_->*Prev = n;
        }
        head_ = n;
        if (tail_ == nullptr)
        {
            tail_ = n;
        }
    }

    /// Insert @p n at the back of the list.
    void pushBack(Node *n) noexcept
    {
        n->*Next = nullptr;
        n->*Prev = tail_;
        if (tail_ != nullptr)
        {
            tail_->*Next = n;
        }
        tail_ = n;
        if (head_ == nullptr)
        {
            head_ = n;
        }
    }

    /// Remove @p n from the list. The node must currently be linked.
    void unlink(Node *n) noexcept
    {
        if (n->*Prev != nullptr)
        {
            (n->*Prev)->*Next = n->*Next;
        }
        else
        {
            head_ = n->*Next;
        }
        if (n->*Next != nullptr)
        {
            (n->*Next)->*Prev = n->*Prev;
        }
        else
        {
            tail_ = n->*Prev;
        }
        n->*Prev = nullptr;
        n->*Next = nullptr;
    }

    /// Move an already-linked node to the front.
    void moveToFront(Node *n) noexcept
    {
        if (head_ == n)
        {
            return;
        }
        unlink(n);
        pushFront(n);
    }

    /// Move an already-linked node to the back.
    void moveToBack(Node *n) noexcept
    {
        if (tail_ == n)
        {
            return;
        }
        unlink(n);
        pushBack(n);
    }

    /// Drop all links (nodes are not destroyed; the caller owns them).
    void clear() noexcept
    {
        head_ = nullptr;
        tail_ = nullptr;
    }

private:
    Node *head_{nullptr};
    Node *tail_{nullptr};
};

}  // namespace detail
}  // namespace lru_ttl_cache
#endif  // LRU_TTL_CACHE_INTRUSIVE_LIST_HPP
