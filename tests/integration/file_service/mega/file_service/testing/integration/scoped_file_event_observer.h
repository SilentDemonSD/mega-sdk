#pragma once

#include <mega/common/expected_forward.h>
#include <mega/common/testing/utility.h>
#include <mega/file_service/file_event_observer.h>
#include <mega/file_service/file_event_observer_id.h>
#include <mega/file_service/file_event_observer_result.h>
#include <mega/file_service/file_event_vector.h>
#include <mega/file_service/type_traits.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <type_traits>

namespace mega
{
namespace file_service
{
namespace testing
{
namespace detail
{

// Convenience.
using common::Expected;

template<typename T>
struct IsFileEventObserverID: std::false_type
{}; // IsFileEventObserverID<T>

template<>
struct IsFileEventObserverID<FileEventObserverID>: std::true_type
{}; // IsFileEventObserverID<FileEventObserverID>

template<typename E>
struct IsFileEventObserverID<Expected<E, FileEventObserverID>>: std::true_type
{}; // IsFileEventObserverID<Expected<E, FileEventObserverID>>

template<typename T>
constexpr auto IsFileEventObserverIDV = IsFileEventObserverID<T>::value;

template<typename T>
using DetectAddObserver =
    decltype(std::declval<T>().addObserver(std::declval<FileEventObserver>()));

template<typename T>
using DetectRemoveObserver =
    decltype(std::declval<T>().removeObserver(std::declval<FileEventObserverID>()));

template<typename T>
using HasAddObserver = IsFileEventObserverID<DetectedT<DetectAddObserver, T>>;

template<typename T>
using HasRemoveObserver = IsNotNoneSuch<DetectedT<DetectRemoveObserver, T>>;

template<typename T>
using IsFileEventSource = std::conjunction<HasAddObserver<T>, HasRemoveObserver<T>>;

template<typename T>
constexpr auto IsFileEventSourceV = IsFileEventSource<T>::value;

} // detail

// Convenience.
using detail::IsFileEventSource;
using detail::IsFileEventSourceV;

template<typename Source>
class ScopedFileEventObserver
{
    // Extract the observer's ID from an Expected<E, T>.
    template<typename E>
    auto extract(common::Expected<E, FileEventObserverID> id)
    {
        return id.value();
    }

    auto extract(FileEventObserverID id)
    {
        return id;
    }

    // So observe(S&) can instantiate an instance of this class.
    template<typename S>
    friend auto observe(S& source)
        -> std::enable_if_t<IsFileEventSourceV<S>, ScopedFileEventObserver<S>>;

    // Called when we've received an event.
    FileEventObserverResult onEvent(const FileEvent& event)
    {
        // Make sure no one is accessing mEvents or mExpected.
        std::lock_guard guard(mLock);

        // Remember that we received this event.
        mEvents.emplace_back(event);

        // We are not expecting any events.
        if (mExpected.empty())
            return FILE_EVENT_OBSERVER_KEEP;

        // Satisfy any expectations for this event.
        mExpected.erase(std::remove(mExpected.begin(), mExpected.end(), event), mExpected.end());

        // Notify any waiters that all expectations have been satisfied.
        if (mExpected.empty())
            mCV.notify_all();

        // Let our source know we want to keep receiving events.
        return FILE_EVENT_OBSERVER_KEEP;
    }

    ScopedFileEventObserver(Source& source):
        mCV(),
        mEvents(),
        mExpected(),
        mID(),
        mLock(),
        mSource(&source)
    {
        // So we can use our onEvent(...) method as a callback.
        auto callback = std::bind(&ScopedFileEventObserver::onEvent, this, std::placeholders::_1);

        // So we will receive events from source.
        mID = extract(source.addObserver(std::move(callback)));
    }

    // Signalled when mExpected becomes empty.
    mutable std::condition_variable mCV;

    // What events has this observer received?
    FileEventVector mEvents;

    // What events do we expect to receive?
    FileEventVector mExpected;

    // The ID of our event observer.
    FileEventObserverID mID;

    // Serializes access to mEvents and mExpected.
    mutable std::mutex mLock;

    // The event source that our observer is observing.
    Source* mSource;

public:
    ScopedFileEventObserver(ScopedFileEventObserver&& other) = delete;

    ~ScopedFileEventObserver()
    {
        if (mSource)
            mSource->removeObserver(mID);
    }

    ScopedFileEventObserver& operator=(ScopedFileEventObserver&& rhs) = delete;

    // Retrieve the events this observer has received.
    FileEventVector events() const
    {
        // Make sure we aren't processing any events.
        std::lock_guard guard(mLock);

        // Return the events we've currently received.
        return mEvents;
    }

    // Specify that we expect to receive a specific event.
    void expect(const FileEvent& event)
    {
        // Make sure we aren't processing any events.
        std::lock_guard guard(mLock);

        // Have we already received this event?
        auto i = std::find(mEvents.begin(), mEvents.end(), event);

        // Expectation isn't satisfied by any event we've already received.
        if (i == mEvents.end())
            mExpected.emplace_back(event);
    }

    // Wait for all of our expectations to be satisfied.
    template<typename Rep, typename Period>
    bool satisfied(std::chrono::duration<Rep, Period> period) const
    {
        auto satisfied = [&]()
        {
            return mExpected.empty();
        }; // satisfied

        // Acquire lock.
        std::unique_lock lock(mLock);

        // Let our caller know if our expectations were satisfied.
        return mCV.wait_for(lock, period, satisfied);
    }

    // True if all our expectations have been satisfied.
    bool satisfied() const
    {
        // Make sure we aren't processing any events.
        std::lock_guard guard(mLock);

        // Let our caller know if our expections are satisfied.
        return mExpected.empty();
    }
}; // ScopedFileEventObserver

// Check if T is a scoped file event observer.
template<typename T>
struct IsScopedFileEventObserver: std::false_type
{}; // IsScopedFileEventObserver<T>

template<typename S>
struct IsScopedFileEventObserver<ScopedFileEventObserver<S>>: std::true_type
{}; // IsScopedFileEventObserver<ScopedFileEventObserver<S>>

// Specify that we expect each observer to recieve a particular event.
template<typename Observer, typename... Observers>
auto expect(const FileEvent& event, Observer& observer, Observers&... observers)
    -> std::enable_if_t<std::conjunction_v<IsScopedFileEventObserver<Observer>,
                                           IsScopedFileEventObserver<Observers>...>>
{
    observer.expect(event);
    (observers.expect(event), ...);
}

// Return a scoped event observer for the specified source.
template<typename Source>
auto observe(Source& source)
    -> std::enable_if_t<IsFileEventSourceV<Source>, ScopedFileEventObserver<Source>>
{
    return ScopedFileEventObserver(source);
}

// True if the expectations of each observer have been satisfied.
template<typename Rep, typename Period, typename... Observers>
auto satisfied(std::chrono::duration<Rep, Period> period, const Observers&... observers)
    -> std::enable_if_t<std::conjunction_v<IsScopedFileEventObserver<Observers>...>, bool>
{
    return waitFor(
        [&]()
        {
            return (observers.satisfied() && ...);
        },
        period);
}

} // testing
} // file_service
} // mega
