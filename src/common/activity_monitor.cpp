#include <cassert>
#include <utility>

#include <mega/common/activity_monitor.h>

namespace mega
{
namespace common
{

Activity::Activity(ActivityMonitor& monitor)
  : mMonitor(&monitor)
{
    std::lock_guard<std::mutex> guard(monitor.mLock);

    ++monitor.mProcessing;

    assert(monitor.mProcessing);
}

Activity::Activity()
  : mMonitor(nullptr)
{
}

Activity::Activity(const Activity& other)
  : mMonitor(other.mMonitor)
{
    if (!mMonitor)
        return;

    std::lock_guard<std::mutex> guard(mMonitor->mLock);

    ++mMonitor->mProcessing;

    assert(mMonitor->mProcessing);
}

Activity::Activity(Activity&& other)
  : mMonitor(std::move(other.mMonitor))
{
    other.mMonitor = nullptr;
}

Activity::~Activity()
{
    // Activity isn't in progress.
    if (!mMonitor)
        return;

    // Convenience.
    using CallbackList = decltype(mMonitor->mCallbacks);

    // Figure out what callbacks we have to invoke, if any.
    auto callbacks = [&]()
    {
        // Acquire monitor lock.
        std::lock_guard guard(mMonitor->mLock);

        // Sanity: At least one activity should be in progress.
        assert(mMonitor->mProcessing);

        // One or more activites are still in progress.
        if (--mMonitor->mProcessing)
            return CallbackList();

        // Take ownership of our monitor's list of callbacks.
        auto callbacks = std::exchange(mMonitor->mCallbacks, {});

        // Wake any threads waiting for our monitor to become idle.
        mMonitor->mCompleted.notify_all();

        // Pass callbacks to our caller.
        return callbacks;
    }();

    // Invoke callbacks, if any.
    for (; !callbacks.empty(); callbacks.pop_front())
        callbacks.front()();
}

Activity& Activity::operator=(const Activity& rhs)
{
    Activity temp(rhs);

    swap(temp);

    return *this;
}

Activity& Activity::operator=(Activity&& rhs)
{
    Activity temp(std::move(rhs));

    swap(temp);

    return *this;
}

void Activity::swap(Activity& other)
{
    using std::swap;

    swap(mMonitor, other.mMonitor);
}

ActivityMonitor::ActivityMonitor()
  : mCompleted()
  , mLock()
  , mProcessing(0u)
{
}

ActivityMonitor::~ActivityMonitor()
{
    waitUntilIdle();
}

bool ActivityMonitor::active() const
{
    std::lock_guard<std::mutex> guard(mLock);

    return mProcessing > 0;
}

Activity ActivityMonitor::begin()
{
    return Activity(*this);
}

void ActivityMonitor::waitUntilIdle()
{
    std::unique_lock<std::mutex> lock(mLock);

    mCompleted.wait(lock, [&]() { return !mProcessing; });
}

void ActivityMonitor::whenIdle(std::function<void()> callback)
{
    // Sanity.
    assert(callback);

    // One or more activities are in progress.
    if (std::unique_lock lock(mLock); mProcessing)
        return mCallbacks.emplace_back(std::move(callback)), void();

    // No activities were in progress.
    callback();
}

} // common
} // mega

