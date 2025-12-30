#pragma once

#include <list>
#include <map>
#include <memory>

namespace mega
{

/**
 * @brief A pool for managing TCP context connections with efficient lookup and removal.
 *
 * This class provides a container for managing active TCP connections with O(log n) insertion,
 * O(log n) removal by pointer, and O(n) iteration. It maintains both insertion order and
 * provides fast lookup capabilities.
 */
class MegaTCPContext;

class TcpContextPool
{
public:
    using ContextPtr = std::shared_ptr<MegaTCPContext>;

    using ContextList = std::list<ContextPtr>;

    /**
     * @brief Get the number of TCP contexts currently in the pool.
     *
     * @return The number of active connections in the pool
     */
    ContextList::size_type size() const
    {
        return mContextList.size();
    };

    /**
     * @brief Add a TCP context to the pool.
     *
     * Adds the given TCP context to the end of the connection list and updates
     * the lookup map for efficient retrieval. The context is stored as a shared_ptr
     * to ensure proper lifetime management.
     *
     * @param p Shared pointer to the TCP context to add. Must not be null.
     * @return True if the context was added, false otherwise (e.g. already exists or on error)
     * @note The same context pointer couldn't be added multiple times
     * @note The context is added to the back of the list (most recently added).
     */
    bool add(ContextPtr p);

    /**
     * @brief Remove and return a TCP context from the pool.
     *
     * Searches for the specified TCP context in the pool and removes it if found.
     * The context is removed from both the list and the lookup map.
     *
     * @param p Raw pointer to the TCP context to remove
     * @return Shared pointer to the removed context, or nullptr if not found
     *
     * @note This operation is O(log n) due to the lookup map
     * @note The returned shared_ptr maintains a reference to prevent immediate destruction
     */
    ContextPtr release(MegaTCPContext* p);

    /**
     * @brief Get a copy of all TCP context raw pointers in insertion order.
     *
     * Creates a vector containing raw pointers to all TCP contexts currently
     * in the pool, ordered from first-added to last-added.
     *
     * @return Vector of raw TCP context pointers in insertion order
     *
     * @note The returned pointers are only valid while the contexts remain in the pool
     * @note This operation is O(n) where n is the number of contexts
     */
    std::vector<MegaTCPContext*> copy() const;

    /**
     * @brief Get the most recently added TCP context.
     *
     * Returns a raw pointer to the TCP context that was most recently added
     * to the pool, or nullptr if the pool is empty.
     *
     * @return Raw pointer to the last-added context, or nullptr if pool is empty
     *
     * @note This operation is O(1)
     * @note The returned pointer is only valid while the context remains in the pool
     */
    MegaTCPContext* back() const;

private:
    ContextList mContextList;

    std::map<MegaTCPContext*, ContextList::iterator> mContextLookup;
};

} // namespace mega
