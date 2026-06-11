#include "impl/tcp_context_pool.h"

#include <algorithm>
#include <iterator>

namespace mega
{

bool TcpContextPool::add(TcpContextPool::ContextPtr p)
{
    if (!p)
        return false;

    auto it = mContextList.emplace(mContextList.end(), std::move(p));

    [[maybe_unused]] auto [_, success] = mContextLookup.emplace(it->get(), it);

    // Already exists, restore the list
    if (!success)
    {
        mContextList.erase(it);
    }

    return success;
}

TcpContextPool::ContextPtr TcpContextPool::release(MegaTCPContext* p)
{
    const auto it = mContextLookup.find(p);
    if (it == mContextLookup.end())
        return nullptr;

    auto ptr = *(it->second);

    mContextList.erase(it->second);
    mContextLookup.erase(it);

    return ptr;
}

std::vector<MegaTCPContext*> TcpContextPool::copy() const
{
    std::vector<MegaTCPContext*> result;
    std::transform(std::begin(mContextList),
                   std::end(mContextList),
                   std::back_inserter(result),
                   [](const ContextPtr& p)
                   {
                       return p.get();
                   });
    return result;
}

MegaTCPContext* TcpContextPool::back() const
{
    return mContextList.empty() ? nullptr : mContextList.back().get();
}

} // namespace mega
