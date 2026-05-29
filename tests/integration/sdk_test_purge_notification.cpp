/**
 * @file sdk_test_purge_notification.cpp
 * @brief Tests for the purge notification feature (EVENT_LAST_PURGE / ^!lpack).
 *
 * User data may be deleted after a prolonged period of account inactivity or for other
 * policy reasons. The SDK surfaces this via EVENT_LAST_PURGE and allows clients to
 * acknowledge the notification cross-device via ATTR_LAST_PURGE_ACKNOWLEDGED (^!lpack).
 */

#include "mega/types.h"
#include "megaapi.h"
#include "SdkTest_test.h"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace
{

// Fixed test timestamp (2025-07-27)
constexpr int64_t TEST_PURGE_TS = 1756332687;

// A timestamp far in the future (2100-01-01). No real node uploaded during the test can have a
// creation time newer than this, so it is used to exercise the "no node newer than X" branch.
constexpr int64_t FUTURE_PURGE_TS = 4102444800;

// Build the ^!devopt value that makes the server inject lastpurge into ug.
// The server expects the raw JSON string; putua handles the wire encoding.
// warningTs/lastActiveTs are the optional trailing offsets, emitted only when > 0.
std::string
    buildDevOptForPurge(int64_t ts, int reason, int64_t warningTs = 0, int64_t lastActiveTs = 0)
{
    std::string arr = std::to_string(ts) + "," + std::to_string(reason);
    if (warningTs > 0)
    {
        arr += "," + std::to_string(warningTs);
        if (lastActiveTs > 0)
            arr += "," + std::to_string(lastActiveTs);
    }
    return "{\"lastpurge\":[" + arr + "]}";
}

// Captures the EVENT_LAST_PURGE payload (ts + reason) as it is delivered to the app, so a test
// can assert that the event carries the injected values. Reads from a copy() of the event (as the
// language bindings do, since the SDK deletes the original after the callback returns); this also
// guards that the copy constructor preserves the keyed number map.
class PurgeEventListener: public MegaListener
{
public:
    void onEvent(MegaApi*, MegaEvent* original) override
    {
        if (!original || original->getType() != MegaEvent::EVENT_LAST_PURGE)
        {
            return;
        }

        std::unique_ptr<MegaEvent> event(original->copy());
        const auto ts = event->getNumber("ts");
        const auto reason = event->getNumber("reason");
        if (!ts || !reason)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mMutex);
        mTs = *ts;
        mReason = static_cast<int>(*reason);
        mWarningTs = event->getNumber("warningTs");
        mLastActiveTs = event->getNumber("lastActiveTs");
        mFired = true;
    }

    bool fired() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mFired;
    }

    int64_t ts() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mTs;
    }

    int reason() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mReason;
    }

    std::optional<int64_t> warningTs() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mWarningTs;
    }

    std::optional<int64_t> lastActiveTs() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mLastActiveTs;
    }

private:
    mutable std::mutex mMutex;
    bool mFired = false;
    int64_t mTs = 0;
    int mReason = 0;
    std::optional<int64_t> mWarningTs;
    std::optional<int64_t> mLastActiveTs;
};

// Registers a listener for the scope and detaches it on exit. Needed because a fatal gtest
// assertion returns from the test body; a stack listener destroyed while still registered would
// be called back on freed memory and crash. The guard's destructor always runs, even on early
// return.
struct ScopedListener
{
    MegaApi* mApi;
    MegaListener* mListener;

    ScopedListener(MegaApi* api, MegaListener* listener):
        mApi(api),
        mListener(listener)
    {
        mApi->addListener(mListener);
    }

    ~ScopedListener()
    {
        mApi->removeListener(mListener);
    }
};

} // namespace

class PurgeNotificationTest: public SdkTest
{
protected:
    void SetUp() override
    {
        SdkTest::SetUp();
        ASSERT_NO_FATAL_FAILURE(getAccountsForTest(1));
        // Best-effort cleanup from any previous test/run.
        // Do not propagate failures — a missing/identical attribute returns API_OK anyway.
        clearLastPurgeAcknowledged();
        clearLastPurgeInjection();
    }

    void TearDown() override
    {
        // Clear the injected ^!devopt while still logged in, so the next test's SetUp login starts
        // without a pending lastpurge. ^!devopt persists on the account between tests, and the SDK
        // now fires EVENT_LAST_PURGE as soon as the node tree becomes current (fetchnodes in
        // SetUp), which would pre-fire and dedup the event before the test body runs.
        clearLastPurgeInjection();
        clearLastPurgeAcknowledged();
        SdkTest::TearDown();
    }

    // Clear ^!lpack (best-effort, uses EXPECT so failures don't abort the fixture).
    void clearLastPurgeAcknowledged()
    {
        RequestTracker tracker(megaApi[0].get());
        megaApi[0]->setLastPurgeAcknowledged(0, &tracker);
        EXPECT_EQ(tracker.waitForResult(), API_OK) << "Failed to clear ^!lpack";
    }

    // Remove any injected lastpurge by setting ^!devopt to ts=0, which fails the `ts > 0` guard and
    // is treated as "no purge" (best-effort).
    void clearLastPurgeInjection()
    {
        RequestTracker tracker(megaApi[0].get());
        megaApi[0]->setUserAttribute(MegaApi::USER_ATTR_DEV_OPT,
                                     buildDevOptForPurge(0, 0).c_str(),
                                     &tracker);
        EXPECT_EQ(tracker.waitForResult(), API_OK) << "Failed to clear injected lastpurge";
    }

    // Inject lastpurge into ug response via ^!devopt (staging API test hook).
    void injectLastPurge(int64_t ts,
                         int reason = ::mega::PURGE_REASON_INACTIVE,
                         int64_t warningTs = 0,
                         int64_t lastActiveTs = 0)
    {
        RequestTracker tracker(megaApi[0].get());
        megaApi[0]->setUserAttribute(
            MegaApi::USER_ATTR_DEV_OPT,
            buildDevOptForPurge(ts, reason, warningTs, lastActiveTs).c_str(),
            &tracker);
        ASSERT_EQ(tracker.waitForResult(), API_OK);
    }

    // Trigger a ug refresh and wait for it to complete.
    void refreshUserData()
    {
        RequestTracker tracker(megaApi[0].get());
        megaApi[0]->getUserData(&tracker);
        ASSERT_EQ(tracker.waitForResult(), API_OK);
    }

    // Upload a small file to the account root, giving the account content with a server-assigned
    // creation time (ctime) close to "now".
    void uploadFileToRoot(const std::string& fileName)
    {
        ASSERT_TRUE(createFile(fileName, false));
        std::unique_ptr<MegaNode> rootnode{megaApi[0]->getRootNode()};
        ASSERT_TRUE(rootnode);
        MegaHandle uploadedNode = INVALID_HANDLE;
        ASSERT_EQ(MegaError::API_OK,
                  doStartUpload(0,
                                &uploadedNode,
                                fileName.c_str(),
                                rootnode.get(),
                                nullptr /*fileName*/,
                                MegaApi::INVALID_CUSTOM_MOD_TIME,
                                nullptr /*appData*/,
                                false /*isSourceTemporary*/,
                                false /*startFirst*/,
                                nullptr /*cancelToken*/))
            << "Cannot upload test file";
        ASSERT_NE(uploadedNode, INVALID_HANDLE);
    }

    // Re-login and fetch nodes. Resets the in-session dedup (mLastPurgeNotifiedTs back to 0) and
    // reloads the node tree, so the next refreshUserData re-evaluates the purge notification with
    // statecurrent == true.
    void reloginAndFetch()
    {
        ASSERT_NO_FATAL_FAILURE(logout(0, false, maxTimeout));
        ASSERT_NO_FATAL_FAILURE(login(0));
        ASSERT_NO_FATAL_FAILURE(fetchnodes(0));
    }
};

/**
 * @brief setLastPurgeAcknowledged stores the timestamp in ^!lpack and it round-trips correctly.
 *
 * After setting ^!lpack, refreshUserData() forces a new ug response which includes the
 * newly stored attribute. getUserAttribute then returns the cached value.
 */
TEST_F(PurgeNotificationTest, AcknowledgementRoundTrip)
{
    RequestTracker setTracker(megaApi[0].get());
    megaApi[0]->setLastPurgeAcknowledged(TEST_PURGE_TS, &setTracker);
    ASSERT_EQ(setTracker.waitForResult(), API_OK);

    RequestTracker getTracker(megaApi[0].get());
    megaApi[0]->getLastPurgeAcknowledged(&getTracker);
    ASSERT_EQ(getTracker.waitForResult(), API_OK);
    EXPECT_EQ(getTracker.request->getNumber(), TEST_PURGE_TS);
}

/**
 * @brief EVENT_LAST_PURGE delivers the injected timestamp and reason to the app.
 */
TEST_F(PurgeNotificationTest, EventCarriesTimestampAndReason)
{
    PurgeEventListener listener;
    ScopedListener scoped(megaApi[0].get(), &listener);

    // FUTURE_PURGE_TS so the newer-node rule can never suppress the event on a shared account.
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS, ::mega::PURGE_REASON_INACTIVE));
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    ASSERT_TRUE(WaitFor(
        [&listener]()
        {
            return listener.fired();
        },
        defaultTimeoutMs));
    EXPECT_EQ(listener.ts(), FUTURE_PURGE_TS);
    EXPECT_EQ(listener.reason(), static_cast<int>(::mega::PURGE_REASON_INACTIVE));
}

/**
 * @brief For PURGE_REASON_INACTIVE, the optional warningTs/lastActiveTs offsets are delivered.
 */
TEST_F(PurgeNotificationTest, EventCarriesWarningAndLastActiveForInactive)
{
    constexpr int64_t WARNING_TS = 4102444000;
    constexpr int64_t LAST_ACTIVE_TS = 4102443000;

    PurgeEventListener listener;
    ScopedListener scoped(megaApi[0].get(), &listener);

    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS,
                                            ::mega::PURGE_REASON_INACTIVE,
                                            WARNING_TS,
                                            LAST_ACTIVE_TS));
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    ASSERT_TRUE(WaitFor(
        [&listener]()
        {
            return listener.fired();
        },
        defaultTimeoutMs));
    ASSERT_TRUE(listener.warningTs());
    EXPECT_EQ(*listener.warningTs(), WARNING_TS);
    ASSERT_TRUE(listener.lastActiveTs());
    EXPECT_EQ(*listener.lastActiveTs(), LAST_ACTIVE_TS);
}

/**
 * @brief When the purge reason is not inactivity, the optional offsets are absent.
 */
TEST_F(PurgeNotificationTest, EventOmitsWarningAndLastActiveForOtherReason)
{
    PurgeEventListener listener;
    ScopedListener scoped(megaApi[0].get(), &listener);

    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS, ::mega::PURGE_REASON_BLOCKED));
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    ASSERT_TRUE(WaitFor(
        [&listener]()
        {
            return listener.fired();
        },
        defaultTimeoutMs));
    EXPECT_FALSE(listener.warningTs());
    EXPECT_FALSE(listener.lastActiveTs());
}

/**
 * @brief EVENT_LAST_PURGE fires when lastpurge is present and not yet acknowledged.
 */
TEST_F(PurgeNotificationTest, EventFiresWhenNotAcknowledged)
{
    // FUTURE_PURGE_TS so the newer-node rule can never suppress the event on a shared account.
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS));

    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        defaultTimeoutMs));
}

/**
 * @brief EVENT_LAST_PURGE does not re-fire on repeated getUserData calls in the same session.
 */
TEST_F(PurgeNotificationTest, EventFiresOnlyOncePerSession)
{
    // FUTURE_PURGE_TS so the newer-node rule can never suppress the event on a shared account.
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS));

    // First call — event should fire
    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(refreshUserData());
    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        defaultTimeoutMs));

    // Second call in same session — event must NOT re-fire
    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(refreshUserData());
    WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        3000);
    EXPECT_FALSE(mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE));
}

/**
 * @brief EVENT_LAST_PURGE does not fire after the user has acknowledged the purge
 *        (cross-session suppression via ^!lpack).
 */
TEST_F(PurgeNotificationTest, EventSuppressedAfterAcknowledgement)
{
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(TEST_PURGE_TS));

    // Acknowledge
    RequestTracker ackTracker(megaApi[0].get());
    megaApi[0]->setLastPurgeAcknowledged(TEST_PURGE_TS, &ackTracker);
    ASSERT_EQ(ackTracker.waitForResult(), API_OK);

    // Re-login to reset in-session dedup (mLastPurgeNotifiedTs resets to 0 on new session)
    ASSERT_NO_FATAL_FAILURE(logout(0, false, maxTimeout));
    ASSERT_NO_FATAL_FAILURE(login(0));
    ASSERT_NO_FATAL_FAILURE(fetchnodes(0));

    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        3000);
    EXPECT_FALSE(mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE));
}

/**
 * @brief EVENT_LAST_PURGE is suppressed when the account contains a node newer than the purge.
 *
 * A file uploaded during the test has a server-assigned ctime close to "now", which is newer than
 * the (past) purge timestamp. That fresh content makes the notification obsolete, so it must not
 * fire even though the purge is present and unacknowledged.
 */
TEST_F(PurgeNotificationTest, EventSuppressedWhenNewerNodeExists)
{
    ASSERT_NO_FATAL_FAILURE(uploadFileToRoot("purge_newer_node.txt"));
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(TEST_PURGE_TS));

    // Reset in-session dedup and reload nodes so the only possible suppression cause is the
    // newer node (statecurrent becomes true after fetchnodes).
    ASSERT_NO_FATAL_FAILURE(reloginAndFetch());

    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        3000);
    EXPECT_FALSE(mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE))
        << "Event should be suppressed when a node newer than the purge exists";
}

/**
 * @brief EVENT_LAST_PURGE still fires when no node is newer than the purge.
 *
 * Same uploaded file as the suppression test, but the purge timestamp is in the future, so the
 * file's ctime is older than X. The "no node newer than X" condition holds and the event fires.
 */
TEST_F(PurgeNotificationTest, EventFiresWhenNoNewerNode)
{
    ASSERT_NO_FATAL_FAILURE(uploadFileToRoot("purge_older_node.txt"));
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS));

    // Reset before relogin: the deferred purge fires as soon as the node tree becomes current
    // during reloginAndFetch (nodes_current). The trailing getUserData is a harmless fallback; do
    // not reset between them or it would discard the event already delivered at nodes_current.
    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(reloginAndFetch());
    ASSERT_NO_FATAL_FAILURE(refreshUserData());

    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        defaultTimeoutMs))
        << "Event should fire when no node is newer than the purge";
}

/**
 * @brief In a fast-login (session resume), a getUserData that completes before fetchnodes must not
 *        fire the event prematurely; it is deferred until the node tree is current and then fires.
 *
 * After resumeSession the node tree is not loaded (statecurrent == false), so the "no node newer
 * than X" condition cannot be evaluated. The notification must be deferred (not fired, not marked
 * as notified) and then fire on a later getUserData once fetchnodes has made the tree current.
 */
TEST_F(PurgeNotificationTest, EventDeferredUntilNodesCurrentOnFastLogin)
{
    // FUTURE_PURGE_TS guarantees no node is newer than the purge, so the only thing that can hold
    // the event back is the node tree not being current yet — which is exactly what this test
    // exercises.
    ASSERT_NO_FATAL_FAILURE(injectLastPurge(FUTURE_PURGE_TS));

    // Capture a session token, then locally log out (server-side session and local cache survive).
    std::unique_ptr<char[]> session(megaApi[0]->dumpSession());
    ASSERT_TRUE(session);
    ASSERT_NO_FATAL_FAILURE(locallogout());

    // Fast login: resume the session. fetchnodes has not run yet, so statecurrent is false.
    ASSERT_NO_FATAL_FAILURE(resumeSession(session.get()));

    // Early getUserData (before fetchnodes): statecurrent is false, so the purge must be deferred.
    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(refreshUserData());
    WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        3000);
    EXPECT_FALSE(mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE))
        << "Event must be deferred before the node tree is current";

    // Completing fetchnodes makes the node tree current; the deferred purge must fire at that point
    // (via nodes_current), without needing another getUserData.
    mApi[0].resetlastEvent();
    ASSERT_NO_FATAL_FAILURE(fetchnodes(0));
    ASSERT_TRUE(WaitFor(
        [this]()
        {
            return mApi[0].lastEventsContain(MegaEvent::EVENT_LAST_PURGE);
        },
        defaultTimeoutMs))
        << "Event should fire once the node tree becomes current after fast login";
}
