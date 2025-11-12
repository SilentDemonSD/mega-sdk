#include <mega/file_service/file_event.h>
#include <mega/file_service/file_event_notifier.h>
#include <mega/file_service/file_event_observer_result.h>
#include <mega/file_service/file_id.h>
#include <mega/file_service/logger.h>
#include <mega/scoped_helpers.h>

#include <optional>

namespace mega
{
namespace file_service
{

struct FileEventNotifier::TransmitContext
{
    TransmitContext(FileEventObserverID currentObserverID,
                    FileEventObserverMap& currentObserverMap,
                    FileEventNotifier& notifier):
        mCurrentObserverID(currentObserverID),
        mCurrentObserverMap(&currentObserverMap),
        mNotifier(notifier),
        mHasDeferredRemove(false),
        mHasDeferredRemoveAll(false)
    {
        mNotifier.mTransmitContext = this;
    }

    ~TransmitContext()
    {
        mNotifier.mTransmitContext = nullptr;
    }

    // What observer are we currently executing?
    FileEventObserverID mCurrentObserverID;

    // What map are we currently processing?
    FileEventObserverMap* mCurrentObserverMap;

    // What notifier owns this context?
    FileEventNotifier& mNotifier;

    // True if we should remove the current observer.
    bool mHasDeferredRemove;

    // True if we should remove all observers in the current observer map.
    bool mHasDeferredRemoveAll;
}; // TransmitContext

thread_local FileEventNotifier::TransmitContext* FileEventNotifier::mTransmitContext = nullptr;

FileEventObserverID FileEventNotifier::addObserver(FileEventObserver observer,
                                                   FileEventObserverMap& observers)
{
    // Used to generate unique observer IDs.
    static std::uint64_t nextID = 0;

    // Sanity.
    assert(observer);

    // Add the observer to our map.
    auto result = observers.emplace(std::piecewise_construct,
                                    std::forward_as_tuple(nullptr, nextID++),
                                    std::forward_as_tuple(std::move(observer)));

    // Sanity.
    assert(result.second);

    // Return observer ID to our caller.
    return result.first->first;
}

void FileEventNotifier::loop()
{
    // Wake the worker if we have events or have been told to terminate.
    auto shouldWake = [&]()
    {
        return !mPendingEvents.empty() || mTerminate;
    }; // shouldWake

    // Process file events as they are queued.
    while (true)
    {
        // Acquire events lock.
        std::unique_lock lock(mPendingEventsLock);

        // Wait until an event has been queued or its time to terminate.
        mCV.wait(lock, shouldWake);

        // Worker's been told to terminate.
        if (mTerminate)
            return;

        // Latch queued events.
        auto events = std::exchange(mPendingEvents, {});

        // Release events lock.
        lock.unlock();

        // Transmit pending events.
        for (; !events.empty(); events.pop_front())
            transmit(events.front());
    }
}

bool FileEventNotifier::shouldDeferRemove(FileEventObserverID id)
{
    // We aren't executing within an observer callback.
    if (!mTransmitContext)
        return false;

    // Observer is removing a different observer.
    if (mTransmitContext->mCurrentObserverID != id)
        return false;

    // Observer is removing itself.
    mTransmitContext->mHasDeferredRemove = true;

    // Let our caller know the observer will be removed higher in the stack.
    return true;
}

bool FileEventNotifier::shouldDeferRemoveAll(FileEventObserverMap& map)
{
    // We aren't executing within an observer callback.
    if (!mTransmitContext)
        return false;

    // Observer is contained by a different observer map.
    if (mTransmitContext->mCurrentObserverMap != &map)
        return false;

    // Observer is removing the map that contains it.
    mTransmitContext->mHasDeferredRemoveAll = true;

    // Let our caller know the observer map will be removed higher in the stack.
    return true;
}

void FileEventNotifier::transmit(const FileEvent& event, FileEventObserverMap& map)
{
    // Transmit event to each observer in map.
    for (auto i = map.begin(); i != map.end();)
    {
        // Instantiate transmit context.
        TransmitContext context(i->first, map, *this);

        // Transmit event to the current observer.
        auto result = i->second(event);

        // Observer removed all observers in this map.
        if (context.mHasDeferredRemoveAll)
            return map.clear();

        // Observer removed itself from this map.
        if (context.mHasDeferredRemove)
            result = FILE_EVENT_OBSERVER_REMOVE;

        // Observer is no longer interested in receiving events.
        if (result == FILE_EVENT_OBSERVER_REMOVE)
        {
            // Remove the observer from the map.
            i = map.erase(i);

            // Step to the next observer.
            continue;
        }

        // Step the next observer.
        ++i;
    }
}

void FileEventNotifier::transmit(const FileEvent& event)
{
    // Retrieve the ID of the file that has changed.
    auto id = std::visit(
        [](auto& event)
        {
            return event.mID;
        },
        event);

    // Make sure no other thread is messing with our observer maps.
    std::lock_guard guard(mObserversLock);

    // Transmit our event to any service-wide observers.
    transmit(event, mServiceObservers);

    // Are any observers monitoring this specific file?
    auto iterator = mFileObservers.find(id);

    // No observers are monitoring this specific file.
    if (iterator == mFileObservers.end())
        return;

    // Transmit our event to all observers monitoring this file.
    transmit(event, iterator->second);

    // No observers are monitoring this file anymore.
    if (iterator->second.empty())
        mFileObservers.erase(iterator);
}

FileEventNotifier::FileEventNotifier():
    mInstanceLogger("FileEventNotifier", *this, logger()),
    mCV(),
    mFileObservers(),
    mObserversLock(),
    mPendingEvents(),
    mPendingEventsLock(),
    mServiceObservers(),
    mTerminate{false},
    mWorker(std::bind(&FileEventNotifier::loop, this))
{}

FileEventNotifier::~FileEventNotifier()
{
    // Let our worker know it's time to terminate.
    mTerminate = true;

    // Make sure our worker's awake.
    mCV.notify_one();

    // Wait for our worker to terminate.
    mWorker.join();
}

FileEventObserverID FileEventNotifier::addObserver(FileID id, FileEventObserver observer)
{
    // Make sure no one else is messing with our observer maps.
    std::lock_guard guard(mObserversLock);

    // Add the observer to the file's observer map.
    return addObserver(std::move(observer), mFileObservers[id]);
}

FileEventObserverID FileEventNotifier::addObserver(FileEventObserver observer)
{
    // Make sure no one else is messing with our observer maps.
    std::lock_guard guard(mObserversLock);

    // Add the observer to our service-wide observers map.
    return addObserver(std::move(observer), mServiceObservers);
}

void FileEventNotifier::notify(FileEvent event)
{
    // Make sure no one is manipulating our event queue.
    std::lock_guard guard(mPendingEventsLock);

    // Queue the event.
    mPendingEvents.emplace_back(std::move(event));

    // Let our worker know it has events to notify.
    mCV.notify_one();
}

void FileEventNotifier::removeObserver(FileID id, FileEventObserverID observerID)
{
    // Make sure no one else is changing our observer maps.
    std::lock_guard guard(mObserversLock);

    // Get a reference to this file's observer map.
    auto iterator = mFileObservers.find(id);

    // No observers are watching this file.
    if (iterator == mFileObservers.end())
        return;

    // An observer is trying to remove itself.
    if (shouldDeferRemove(observerID))
    {
        // Observer will be removed higher in the call stack.
        return;
    }

    // Remove the observer from this file's observer map.
    iterator->second.erase(observerID);

    // File no longer has any observers.
    if (iterator->second.empty())
        mFileObservers.erase(iterator);
}

void FileEventNotifier::removeObserver(FileEventObserverID id)
{
    // Make sure no one is changing our observer maps.
    std::lock_guard guard(mObserversLock);

    // An observer is trying to remove itself.
    if (shouldDeferRemove(id))
    {
        // Observer will be removed higher in the call stack.
        return;
    }

    // Remove the observer from the service-wide observer map.
    mServiceObservers.erase(id);
}

void FileEventNotifier::removeObservers(FileID id)
{
    // Make sure no one is changing our observer maps.
    std::lock_guard guard(mObserversLock);

    // Check if any observers are watching the specified file.
    auto iterator = mFileObservers.find(id);

    // No observers are watching the file.
    if (iterator == mFileObservers.end())
        return;

    // An observer is trying to remove the map that contains it.
    if (shouldDeferRemoveAll(iterator->second))
    {
        // Map will be removed higher in the call stack.
        return;
    }

    // Remove all observers watching this file.
    mFileObservers.erase(iterator);
}

void FileEventNotifier::removeObservers()
{
    // Make sure no one is changing our observer maps.
    std::lock_guard guard(mObserversLock);

    // An observer is trying to clear the map that contains it.
    if (shouldDeferRemoveAll(mServiceObservers))
    {
        // Map will be cleared higher in the call stack.
        return;
    }

    // Remove all service-wide observers.
    mServiceObservers.clear();
}

} // file_service
} // mega
