// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// This file contains tests for the BucketManager, and higher-level operations
// concerning the lifecycle of buckets, their ownership and (re)creation, and
// integration into ledgers.

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketManagerImpl.h"
#include "bucket/BucketTests.h"
#include "ledger/LedgerTxn.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Math.h"
#include "util/Timer.h"

using namespace stellar;
using namespace BucketTests;

namespace BucketManagerTests
{

class LedgerManagerForBucketTests : public LedgerManagerImpl
{
    bool mUseTestEntries{false};
    std::vector<LedgerEntry> mTestInitEntries;
    std::vector<LedgerEntry> mTestLiveEntries;
    std::vector<LedgerKey> mTestDeadEntries;

  protected:
    void
    transferLedgerEntriesToBucketList(AbstractLedgerTxn& ltx,
                                      uint32_t ledgerSeq,
                                      uint32_t ledgerVers) override
    {
        if (mUseTestEntries)
        {
            // Seal the ltx but throw its entries away.
            std::vector<LedgerEntry> init, live;
            std::vector<LedgerKey> dead;
            ltx.getAllEntries(init, live, dead);
            // Use the testing values.
            mApp.getBucketManager().addBatch(mApp, ledgerSeq, ledgerVers,
                                             mTestInitEntries, mTestLiveEntries,
                                             mTestDeadEntries);
            mUseTestEntries = false;
        }
        else
        {
            LedgerManagerImpl::transferLedgerEntriesToBucketList(ltx, ledgerSeq,
                                                                 ledgerVers);
        }
    }

  public:
    void
    setNextLedgerEntryBatchForBucketTesting(
        std::vector<LedgerEntry> const& initEntries,
        std::vector<LedgerEntry> const& liveEntries,
        std::vector<LedgerKey> const& deadEntries)
    {
        mUseTestEntries = true;
        mTestInitEntries = initEntries;
        mTestLiveEntries = liveEntries;
        mTestDeadEntries = deadEntries;
    }

    LedgerManagerForBucketTests(Application& app) : LedgerManagerImpl(app)
    {
    }
};

class LedgerManagerTestApplication : public TestApplication
{
  public:
    LedgerManagerTestApplication(VirtualClock& clock, Config const& cfg)
        : TestApplication(clock, cfg)
    {
    }

    virtual LedgerManagerForBucketTests&
    getLedgerManager() override
    {
        auto& lm = ApplicationImpl::getLedgerManager();
        return static_cast<LedgerManagerForBucketTests&>(lm);
    }

  private:
    virtual std::unique_ptr<LedgerManager>
    createLedgerManager() override
    {
        return std::make_unique<LedgerManagerForBucketTests>(*this);
    }
};

static void
clearFutures(Application::pointer app, BucketList& bl)
{

    // First go through the BL and mop up all the FutureBuckets.
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        bl.getLevel(i).getNext().clear();
    }

    // Then go through all the _worker threads_ and mop up any work they
    // might still be doing (that might be "dropping a shared_ptr<Bucket>").

    size_t n = (size_t)app->getConfig().WORKER_THREADS;
    std::mutex mutex;
    std::condition_variable cv, cv2;
    size_t waiting = 0, finished = 0;
    for (size_t i = 0; i < n; ++i)
    {
        app->postOnBackgroundThread(
            [&] {
                std::unique_lock<std::mutex> lock(mutex);
                if (++waiting == n)
                {
                    cv.notify_all();
                }
                else
                {
                    cv.wait(lock, [&] { return waiting == n; });
                }
                ++finished;
                cv2.notify_one();
            },
            "BucketTests: clearFutures");
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv2.wait(lock, [&] { return finished == n; });
    }
}

static Hash
closeLedger(Application& app)
{
    auto& lm = app.getLedgerManager();
    auto lcl = lm.getLastClosedLedgerHeader();
    uint32_t ledgerNum = lcl.header.ledgerSeq + 1;
    CLOG(INFO, "Bucket")
        << "Artificially closing ledger " << ledgerNum
        << " with lcl=" << hexAbbrev(lcl.hash) << ", buckets="
        << hexAbbrev(app.getBucketManager().getBucketList().getHash());
    auto txSet = std::make_shared<TxSetFrame>(lcl.hash);
    StellarValue sv(txSet->getContentsHash(), lcl.header.scpValue.closeTime,
                    emptyUpgradeSteps, STELLAR_VALUE_BASIC);
    LedgerCloseData lcd(ledgerNum, txSet, sv);
    lm.valueExternalized(lcd);
    return lm.getLastClosedLedgerHeader().hash;
}
}

using namespace BucketManagerTests;

TEST_CASE("skip list", "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    Application::pointer app = createTestApplication(clock, cfg);

    class BucketManagerTest : public BucketManagerImpl
    {
      public:
        BucketManagerTest(Application& app) : BucketManagerImpl(app)
        {
        }
        void
        test()
        {
            Hash h0;
            Hash h1 = HashUtils::random();
            Hash h2 = HashUtils::random();
            Hash h3 = HashUtils::random();
            Hash h4 = HashUtils::random();
            Hash h5 = HashUtils::random();
            Hash h6 = HashUtils::random();
            Hash h7 = HashUtils::random();

            // up first entry
            LedgerHeader header;
            header.ledgerSeq = 5;
            header.bucketListHash = h1;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h0);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_1;
            header.bucketListHash = h2;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h2);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_1 * 2;
            header.bucketListHash = h3;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h3);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_1 * 2 + 1;
            header.bucketListHash = h2;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h3);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_2;
            header.bucketListHash = h4;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h4);
            REQUIRE(header.skipList[1] == h0);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_2 + SKIP_1;
            header.bucketListHash = h5;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h5);
            REQUIRE(header.skipList[1] == h4);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_3 + SKIP_2;
            header.bucketListHash = h6;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h6);
            REQUIRE(header.skipList[1] == h4);
            REQUIRE(header.skipList[2] == h0);
            REQUIRE(header.skipList[3] == h0);

            header.ledgerSeq = SKIP_3 + SKIP_2 + SKIP_1;
            header.bucketListHash = h7;
            calculateSkipValues(header);
            REQUIRE(header.skipList[0] == h7);
            REQUIRE(header.skipList[1] == h6);
            REQUIRE(header.skipList[2] == h4);
            REQUIRE(header.skipList[3] == h0);
        }
    };

    BucketManagerTest btest(*app);
    btest.test();
}

TEST_CASE("bucketmanager ownership", "[bucket][bucketmanager]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();
    for_versions_with_differing_bucket_logic(cfg, [&](Config const& cfg) {
        Application::pointer app = createTestApplication(clock, cfg);

        std::vector<LedgerEntry> live(
            LedgerTestUtils::generateValidLedgerEntries(10));
        std::vector<LedgerKey> dead{};

        std::shared_ptr<Bucket> b1;

        {
            std::shared_ptr<Bucket> b2 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            b1 = b2;

            // Bucket is referenced by b1, b2 and the BucketManager.
            CHECK(b1.use_count() == 3);

            std::shared_ptr<Bucket> b3 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            std::shared_ptr<Bucket> b4 =
                Bucket::fresh(app->getBucketManager(), getAppLedgerVersion(app),
                              {}, live, dead, /*countMergeEvents=*/true);
            // Bucket is referenced by b1, b2, b3, b4 and the BucketManager.
            CHECK(b1.use_count() == 5);
        }

        // Bucket is now only referenced by b1 and the BucketManager.
        CHECK(b1.use_count() == 2);

        // Drop bucket ourselves then purge bucketManager.
        std::string filename = b1->getFilename();
        CHECK(fs::exists(filename));
        b1.reset();
        app->getBucketManager().forgetUnreferencedBuckets();
        CHECK(!fs::exists(filename));

        // Try adding a bucket to the BucketManager's bucketlist
        auto& bl = app->getBucketManager().getBucketList();
        bl.addBatch(*app, 1, getAppLedgerVersion(app), {}, live, dead);
        clearFutures(app, bl);
        b1 = bl.getLevel(0).getCurr();

        // Bucket should be referenced by bucketlist itself, BucketManager cache
        // and b1.
        CHECK(b1.use_count() == 3);

        // This shouldn't change if we forget unreferenced buckets since it's
        // referenced by bucketlist.
        app->getBucketManager().forgetUnreferencedBuckets();
        CHECK(b1.use_count() == 3);

        // But if we mutate the curr bucket of the bucketlist, it should.
        live[0] = LedgerTestUtils::generateValidLedgerEntry(10);
        bl.addBatch(*app, 1, getAppLedgerVersion(app), {}, live, dead);
        clearFutures(app, bl);
        CHECK(b1.use_count() == 2);

        // Drop it again.
        filename = b1->getFilename();
        CHECK(fs::exists(filename));
        b1.reset();
        app->getBucketManager().forgetUnreferencedBuckets();
        CHECK(!fs::exists(filename));
    });
}

// Running one of these tests involves comparing three timelines with different
// application lifecycles for identical outcomes.
//
// A single initial 'control' timeline runs through to the end of the time
// window without interruption, surveying various points along the way. At the
// end, a complete set of the live ledger entries as represented by the bucket
// list is collected.
//
// Then a second timeline runs in which the application is stopped and restarted
// at each of the survey points in the control timeline, comparing each such
// survey for equal outcomes (same ledger hash, bucket hashes, same bucket-list
// hash, etc.) and confirming that the merge started before the application
// stops is restarted when the application is restarted.
//
// Finally, a third timeline runs that starts and stops at all the same places,
// but _switches protocol_ on one of the boundaries, between the protocol the
// test was constructed with, and the next protocol. In this timeline the
// surveys are not expected to match (as the bucket list will behave differently
// after the protocol switch) but the final live ledger entry set should be the
// same.
//
// In all cases, we are focusing on a given "designated level" of the bucket
// list, and a few "designated ledgers" at key values before and after ledgers
// when that designated level is perturbed (either by incoming or outgoing
// spills, or snapshots).
//
// For example, if we run the test with designated level 5, level 5 snaps/spills
// once at every multiple of 2048 ledgers, and prepares (merging a level 4 spill
// into its curr) once every multiple of 512 ledgers. So we calculate a set of
// designated ledgers (+/- a few ledgers each way) in the vicinity of ledgers
// 1024, 1536, 2048, 2560, and 3072 (and so on for a few other multiples of
// 2048).
class StopAndRestartBucketMergesTest
{
    static void
    resolveAllMerges(BucketList& bl)
    {
        for (uint32 i = 0; i < BucketList::kNumLevels; ++i)
        {
            auto& level = bl.getLevel(i);
            auto& next = level.getNext();
            if (next.isMerging())
            {
                next.resolve();
            }
        }
    }

    struct Survey
    {
        Hash mCurrBucketHash;
        Hash mSnapBucketHash;
        Hash mBucketListHash;
        Hash mLedgerHeaderHash;
        MergeCounters mMergeCounters;

        void
        dumpMergeCounters(std::string const& label, uint32_t level) const
        {
            CLOG(INFO, "Bucket") << "MergeCounters: " << label
                                 << " (designated level: " << level << ")";
            CLOG(INFO, "Bucket") << "PreInitEntryProtocolMerges: "
                                 << mMergeCounters.mPreInitEntryProtocolMerges;
            CLOG(INFO, "Bucket") << "PostInitEntryProtocolMerges: "
                                 << mMergeCounters.mPostInitEntryProtocolMerges;
            CLOG(INFO, "Bucket")
                << "NewMetaEntries: " << mMergeCounters.mNewMetaEntries;
            CLOG(INFO, "Bucket")
                << "NewInitEntries: " << mMergeCounters.mNewInitEntries;
            CLOG(INFO, "Bucket")
                << "NewLiveEntries: " << mMergeCounters.mNewLiveEntries;
            CLOG(INFO, "Bucket")
                << "NewDeadEntries: " << mMergeCounters.mNewDeadEntries;
            CLOG(INFO, "Bucket")
                << "OldMetaEntries: " << mMergeCounters.mOldMetaEntries;
            CLOG(INFO, "Bucket")
                << "OldInitEntries: " << mMergeCounters.mOldInitEntries;
            CLOG(INFO, "Bucket")
                << "OldLiveEntries: " << mMergeCounters.mOldLiveEntries;
            CLOG(INFO, "Bucket")
                << "OldDeadEntries: " << mMergeCounters.mOldDeadEntries;
            CLOG(INFO, "Bucket") << "OldEntriesDefaultAccepted: "
                                 << mMergeCounters.mOldEntriesDefaultAccepted;
            CLOG(INFO, "Bucket") << "NewEntriesDefaultAccepted: "
                                 << mMergeCounters.mNewEntriesDefaultAccepted;
            CLOG(INFO, "Bucket")
                << "NewInitEntriesMergedWithOldDead: "
                << mMergeCounters.mNewInitEntriesMergedWithOldDead;
            CLOG(INFO, "Bucket")
                << "OldInitEntriesMergedWithNewLive: "
                << mMergeCounters.mOldInitEntriesMergedWithNewLive;
            CLOG(INFO, "Bucket")
                << "OldInitEntriesMergedWithNewDead: "
                << mMergeCounters.mOldInitEntriesMergedWithNewDead;
            CLOG(INFO, "Bucket")
                << "NewEntriesMergedWithOldNeitherInit: "
                << mMergeCounters.mNewEntriesMergedWithOldNeitherInit;
            CLOG(INFO, "Bucket")
                << "ShadowScanSteps: " << mMergeCounters.mShadowScanSteps;
            CLOG(INFO, "Bucket") << "MetaEntryShadowElisions: "
                                 << mMergeCounters.mMetaEntryShadowElisions;
            CLOG(INFO, "Bucket") << "LiveEntryShadowElisions: "
                                 << mMergeCounters.mLiveEntryShadowElisions;
            CLOG(INFO, "Bucket") << "InitEntryShadowElisions: "
                                 << mMergeCounters.mInitEntryShadowElisions;
            CLOG(INFO, "Bucket") << "DeadEntryShadowElisions: "
                                 << mMergeCounters.mDeadEntryShadowElisions;
            CLOG(INFO, "Bucket")
                << "OutputIteratorTombstoneElisions: "
                << mMergeCounters.mOutputIteratorTombstoneElisions;
            CLOG(INFO, "Bucket") << "OutputIteratorBufferUpdates: "
                                 << mMergeCounters.mOutputIteratorBufferUpdates;
            CLOG(INFO, "Bucket") << "OutputIteratorActualWrites: "
                                 << mMergeCounters.mOutputIteratorActualWrites;
        }

        void
        checkSensiblePostInitEntryMergeCounters() const
        {
            CHECK(mMergeCounters.mPostInitEntryProtocolMerges != 0);

            CHECK(mMergeCounters.mNewMetaEntries == 0);
            CHECK(mMergeCounters.mNewInitEntries != 0);
            CHECK(mMergeCounters.mNewLiveEntries != 0);
            CHECK(mMergeCounters.mNewDeadEntries != 0);

            CHECK(mMergeCounters.mOldMetaEntries == 0);
            CHECK(mMergeCounters.mOldInitEntries != 0);
            CHECK(mMergeCounters.mOldLiveEntries != 0);
            CHECK(mMergeCounters.mOldDeadEntries != 0);

            CHECK(mMergeCounters.mOldEntriesDefaultAccepted != 0);
            CHECK(mMergeCounters.mNewEntriesDefaultAccepted != 0);
            CHECK(mMergeCounters.mNewInitEntriesMergedWithOldDead != 0);
            CHECK(mMergeCounters.mOldInitEntriesMergedWithNewLive != 0);
            CHECK(mMergeCounters.mOldInitEntriesMergedWithNewDead != 0);
            CHECK(mMergeCounters.mNewEntriesMergedWithOldNeitherInit != 0);

            CHECK(mMergeCounters.mShadowScanSteps != 0);
            CHECK(mMergeCounters.mMetaEntryShadowElisions == 0);
            CHECK(mMergeCounters.mLiveEntryShadowElisions != 0);
            CHECK(mMergeCounters.mInitEntryShadowElisions == 0);
            CHECK(mMergeCounters.mDeadEntryShadowElisions == 0);

            CHECK(mMergeCounters.mOutputIteratorBufferUpdates != 0);
            CHECK(mMergeCounters.mOutputIteratorActualWrites != 0);
            CHECK(mMergeCounters.mOutputIteratorBufferUpdates >=
                  mMergeCounters.mOutputIteratorActualWrites);
        }

        void
        checkSensiblePreInitEntryMergeCounters() const
        {
            CHECK(mMergeCounters.mPreInitEntryProtocolMerges != 0);

            CHECK(mMergeCounters.mNewMetaEntries == 0);
            CHECK(mMergeCounters.mNewInitEntries == 0);
            CHECK(mMergeCounters.mNewLiveEntries != 0);
            CHECK(mMergeCounters.mNewDeadEntries != 0);

            CHECK(mMergeCounters.mOldMetaEntries == 0);
            CHECK(mMergeCounters.mOldInitEntries == 0);
            CHECK(mMergeCounters.mOldLiveEntries != 0);
            CHECK(mMergeCounters.mOldDeadEntries != 0);

            CHECK(mMergeCounters.mOldEntriesDefaultAccepted != 0);
            CHECK(mMergeCounters.mNewEntriesDefaultAccepted != 0);
            CHECK(mMergeCounters.mNewInitEntriesMergedWithOldDead == 0);
            CHECK(mMergeCounters.mOldInitEntriesMergedWithNewLive == 0);
            CHECK(mMergeCounters.mOldInitEntriesMergedWithNewDead == 0);
            CHECK(mMergeCounters.mNewEntriesMergedWithOldNeitherInit != 0);

            CHECK(mMergeCounters.mShadowScanSteps != 0);
            CHECK(mMergeCounters.mMetaEntryShadowElisions == 0);
            CHECK(mMergeCounters.mLiveEntryShadowElisions != 0);
            CHECK(mMergeCounters.mInitEntryShadowElisions == 0);
            CHECK(mMergeCounters.mDeadEntryShadowElisions != 0);

            CHECK(mMergeCounters.mOutputIteratorBufferUpdates != 0);
            CHECK(mMergeCounters.mOutputIteratorActualWrites != 0);
            CHECK(mMergeCounters.mOutputIteratorBufferUpdates >=
                  mMergeCounters.mOutputIteratorActualWrites);
        }

        void
        checkEqualMergeCounters(Survey const& other) const
        {
            CHECK(mMergeCounters.mPreInitEntryProtocolMerges ==
                  other.mMergeCounters.mPreInitEntryProtocolMerges);
            CHECK(mMergeCounters.mPostInitEntryProtocolMerges ==
                  other.mMergeCounters.mPostInitEntryProtocolMerges);

            CHECK(mMergeCounters.mNewMetaEntries ==
                  other.mMergeCounters.mNewMetaEntries);
            CHECK(mMergeCounters.mNewInitEntries ==
                  other.mMergeCounters.mNewInitEntries);
            CHECK(mMergeCounters.mNewLiveEntries ==
                  other.mMergeCounters.mNewLiveEntries);
            CHECK(mMergeCounters.mNewDeadEntries ==
                  other.mMergeCounters.mNewDeadEntries);
            CHECK(mMergeCounters.mOldMetaEntries ==
                  other.mMergeCounters.mOldMetaEntries);
            CHECK(mMergeCounters.mOldInitEntries ==
                  other.mMergeCounters.mOldInitEntries);
            CHECK(mMergeCounters.mOldLiveEntries ==
                  other.mMergeCounters.mOldLiveEntries);
            CHECK(mMergeCounters.mOldDeadEntries ==
                  other.mMergeCounters.mOldDeadEntries);

            CHECK(mMergeCounters.mOldEntriesDefaultAccepted ==
                  other.mMergeCounters.mOldEntriesDefaultAccepted);
            CHECK(mMergeCounters.mNewEntriesDefaultAccepted ==
                  other.mMergeCounters.mNewEntriesDefaultAccepted);
            CHECK(mMergeCounters.mNewInitEntriesMergedWithOldDead ==
                  other.mMergeCounters.mNewInitEntriesMergedWithOldDead);
            CHECK(mMergeCounters.mOldInitEntriesMergedWithNewLive ==
                  other.mMergeCounters.mOldInitEntriesMergedWithNewLive);
            CHECK(mMergeCounters.mOldInitEntriesMergedWithNewDead ==
                  other.mMergeCounters.mOldInitEntriesMergedWithNewDead);
            CHECK(mMergeCounters.mNewEntriesMergedWithOldNeitherInit ==
                  other.mMergeCounters.mNewEntriesMergedWithOldNeitherInit);

            CHECK(mMergeCounters.mShadowScanSteps ==
                  other.mMergeCounters.mShadowScanSteps);
            CHECK(mMergeCounters.mMetaEntryShadowElisions ==
                  other.mMergeCounters.mMetaEntryShadowElisions);
            CHECK(mMergeCounters.mLiveEntryShadowElisions ==
                  other.mMergeCounters.mLiveEntryShadowElisions);
            CHECK(mMergeCounters.mInitEntryShadowElisions ==
                  other.mMergeCounters.mInitEntryShadowElisions);
            CHECK(mMergeCounters.mDeadEntryShadowElisions ==
                  other.mMergeCounters.mDeadEntryShadowElisions);

            CHECK(mMergeCounters.mOutputIteratorTombstoneElisions ==
                  other.mMergeCounters.mOutputIteratorTombstoneElisions);
            CHECK(mMergeCounters.mOutputIteratorBufferUpdates ==
                  other.mMergeCounters.mOutputIteratorBufferUpdates);
            CHECK(mMergeCounters.mOutputIteratorActualWrites ==
                  other.mMergeCounters.mOutputIteratorActualWrites);
        }
        void
        checkEqual(Survey const& other) const
        {
            CHECK(mCurrBucketHash == other.mCurrBucketHash);
            CHECK(mSnapBucketHash == other.mSnapBucketHash);
            CHECK(mBucketListHash == other.mBucketListHash);
            CHECK(mLedgerHeaderHash == other.mLedgerHeaderHash);
            checkEqualMergeCounters(other);
        }
        Survey(Application& app, uint32_t level)
        {
            LedgerManager& lm = app.getLedgerManager();
            BucketManager& bm = app.getBucketManager();
            BucketList& bl = bm.getBucketList();
            // Complete those merges we're about to inspect.
            resolveAllMerges(bl);

            mMergeCounters = bm.readMergeCounters();
            mLedgerHeaderHash = lm.getLastClosedLedgerHeader().hash;
            mBucketListHash = bl.getHash();
            BucketLevel& blv = bl.getLevel(level);
            mCurrBucketHash = blv.getCurr()->getHash();
            mSnapBucketHash = blv.getSnap()->getHash();
        }
    };

    uint32_t mProtocol;
    uint32_t mDesignatedLevel;
    std::set<uint32_t> mDesignatedLedgers;
    std::map<uint32_t, Survey> mControlSurveys;
    std::map<LedgerKey, LedgerEntry> mFinalEntries;
    std::vector<std::vector<LedgerEntry>> mInitEntryBatches;
    std::vector<std::vector<LedgerEntry>> mLiveEntryBatches;
    std::vector<std::vector<LedgerKey>> mDeadEntryBatches;

    void
    collectLedgerEntries(Application& app,
                         std::map<LedgerKey, LedgerEntry>& entries)
    {
        auto bl = app.getBucketManager().getBucketList();
        for (uint32_t i = BucketList::kNumLevels; i > 0; --i)
        {
            BucketLevel const& level = bl.getLevel(i - 1);
            for (auto bucket : {level.getSnap(), level.getCurr()})
            {
                for (BucketInputIterator bi(bucket); bi; ++bi)
                {
                    BucketEntry const& e = *bi;
                    if (e.type() == LIVEENTRY || e.type() == INITENTRY)
                    {
                        auto le = e.liveEntry();
                        entries[LedgerEntryKey(le)] = le;
                    }
                    else
                    {
                        assert(e.type() == DEADENTRY);
                        entries.erase(e.deadEntry());
                    }
                }
            }
        }
    }

    void
    collectFinalLedgerEntries(Application& app)
    {
        collectLedgerEntries(app, mFinalEntries);
        CLOG(INFO, "Bucket") << "Collected final ledger state with " << std::dec
                             << mFinalEntries.size() << " entries.";
    }

    void
    checkAgainstFinalLedgerEntries(Application& app)
    {
        std::map<LedgerKey, LedgerEntry> testEntries;
        collectLedgerEntries(app, testEntries);
        CLOG(INFO, "Bucket") << "Collected test ledger state with " << std::dec
                             << testEntries.size() << " entries.";
        CHECK(testEntries.size() == mFinalEntries.size());
        for (auto const& pair : testEntries)
        {
            CHECK(mFinalEntries[pair.first] == pair.second);
        }
    }

    void
    calculateDesignatedLedgers()
    {
        uint32_t spillFreq = BucketList::levelHalf(mDesignatedLevel);
        uint32_t prepFreq = (mDesignatedLevel == 0
                                 ? 1
                                 : BucketList::levelHalf(mDesignatedLevel - 1));

        uint32_t const SPILLCOUNT = 5;
        uint32_t const PREPCOUNT = 5;
        uint32_t const STEPCOUNT = 5;

        for (uint32_t nSpill = 0; nSpill < SPILLCOUNT; ++nSpill)
        {
            for (uint32_t nPrep = 0; nPrep < PREPCOUNT; ++nPrep)
            {
                for (uint32_t nStep = 0; nStep < STEPCOUNT; ++nStep)
                {
                    // For each spill we want to look in the vicinity of 2
                    // prepares before and after it (as well as _at_ the spill)
                    // and for each vicinity we want to look 2 ledgers before
                    // and after the event.
                    uint32_t target = (nSpill * spillFreq);
                    target += ((PREPCOUNT / 2) * prepFreq);
                    for (uint32_t i = 0; i < nPrep && target > prepFreq; ++i)
                    {
                        target -= prepFreq;
                    }
                    target += (STEPCOUNT / 2);
                    for (uint32_t i = 0; i < nStep && target > 1; ++i)
                    {
                        target -= 1;
                    }
                    mDesignatedLedgers.insert(target);
                }
            }
        }
        CLOG(INFO, "Bucket")
            << "Collected " << mDesignatedLedgers.size()
            << " designated ledgers for level " << mDesignatedLevel;
        for (auto d : mDesignatedLedgers)
        {
            CLOG(INFO, "Bucket") << "Designated ledger: " << std::dec << d
                                 << " = 0x" << std::hex << d;
        }
    }

    // Designated ledgers are where stop/restart events will occur. We further
    // _survey_ ledgers +/- 1 on each side of _designated_ ledgers.
    bool
    shouldSurveyLedger(uint32_t ledger)
    {
        if (mDesignatedLedgers.find(ledger + 1) != mDesignatedLedgers.end())
        {
            return true;
        }
        if (mDesignatedLedgers.find(ledger) != mDesignatedLedgers.end())
        {
            return true;
        }
        if (ledger > 0 &&
            mDesignatedLedgers.find(ledger - 1) != mDesignatedLedgers.end())
        {
            return true;
        }
        return false;
    }

    void
    collectControlSurveys()
    {
        VirtualClock clock;
        Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE));
        cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
        cfg.ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING = true;
        cfg.LEDGER_PROTOCOL_VERSION = mProtocol;
        assert(!mDesignatedLedgers.empty());
        uint32_t finalLedger = (*mDesignatedLedgers.rbegin()) + 1;
        CLOG(INFO, "Bucket")
            << "Collecting control surveys in ledger range 2.." << std::dec
            << finalLedger << " = 0x" << std::hex << finalLedger;
        auto app =
            createTestApplication<LedgerManagerTestApplication>(clock, cfg);
        app->start();

        std::vector<LedgerKey> allKeys;
        std::map<LedgerKey, LedgerEntry> currLive;
        std::map<LedgerKey, LedgerEntry> currDead;

        for (uint32_t i = 2;
             !app->getClock().getIOContext().stopped() && i < finalLedger; ++i)
        {
            size_t nEntriesInBatch = 10;
            std::vector<LedgerEntry> initEntries;
            std::vector<LedgerEntry> liveEntries;
            std::vector<LedgerKey> deadEntries;
            if (mInitEntryBatches.size() > 2)
            {
                std::set<LedgerKey> changedEntries;
                for (size_t j = 0; j < nEntriesInBatch / 2; ++j)
                {
                    auto const& existingKey = rand_element(allKeys);
                    if (changedEntries.find(existingKey) !=
                        changedEntries.end())
                        continue;
                    changedEntries.insert(existingKey);
                    auto liveIter = currLive.find(existingKey);
                    auto deadIter = currDead.find(existingKey);
                    assert(liveIter == currLive.end() ||
                           deadIter == currDead.end());
                    assert(liveIter != currLive.end() ||
                           deadIter != currDead.end());
                    auto& existingEntry =
                        (liveIter == currLive.end() ? deadIter->second
                                                    : liveIter->second);
                    if (rand_flip())
                    {
                        // Try to do a to-live transition
                        LedgerTestUtils::randomlyModifyEntry(existingEntry);
                        if (liveIter == currLive.end())
                        {
                            // Currently dead: revive.
                            initEntries.emplace_back(existingEntry);
                            currLive.insert(*deadIter);
                            currDead.erase(deadIter);
                        }
                        else
                        {
                            // Already live: stays alive.
                            liveEntries.emplace_back(existingEntry);
                        }
                    }
                    else
                    {
                        // Try to do a to-dead transition
                        if (liveIter == currLive.end())
                        {
                            // Already dead: we tried!
                        }
                        else
                        {
                            // Currently alive: kill.
                            deadEntries.emplace_back(existingKey);
                            currDead.insert(*liveIter);
                            currLive.erase(liveIter);
                        }
                    }
                }
            }
            auto nInits =
                nEntriesInBatch - (liveEntries.size() + deadEntries.size());
            auto newRandom =
                LedgerTestUtils::generateValidLedgerEntries(nInits);
            for (auto const& e : newRandom)
            {
                auto k = LedgerEntryKey(e);
                initEntries.emplace_back(e);
                allKeys.emplace_back(k);
                currLive.emplace(std::make_pair(k, e));
            }
            mInitEntryBatches.emplace_back(initEntries);
            mLiveEntryBatches.emplace_back(liveEntries);
            mDeadEntryBatches.emplace_back(deadEntries);
            LedgerManagerForBucketTests& lm = app->getLedgerManager();
            lm.setNextLedgerEntryBatchForBucketTesting(
                mInitEntryBatches.back(), mLiveEntryBatches.back(),
                mDeadEntryBatches.back());
            closeLedger(*app);
            assert(i == lm.getLastClosedLedgerHeader().header.ledgerSeq);
            if (shouldSurveyLedger(i))
            {
                CLOG(INFO, "Bucket") << "Taking survey at " << std::dec << i
                                     << " = 0x" << std::hex << i;
                mControlSurveys.insert(
                    std::make_pair(i, Survey(*app, mDesignatedLevel)));
            }
        }

        collectFinalLedgerEntries(*app);
    }

    void
    runStopAndRestartTest(uint32_t firstProtocol, uint32_t secondProtocol)
    {
        std::unique_ptr<VirtualClock> clock = std::make_unique<VirtualClock>();
        Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
        cfg.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;
        cfg.ARTIFICIALLY_REDUCE_MERGE_COUNTS_FOR_TESTING = true;
        cfg.LEDGER_PROTOCOL_VERSION = firstProtocol;
        assert(!mDesignatedLedgers.empty());
        uint32_t finalLedger = (*mDesignatedLedgers.rbegin()) + 1;
        uint32_t currProtocol = firstProtocol;

        // If firstProtocol != secondProtocol, we will switch protocols at
        // protocolSwitchLedger. At this point the surveys are expected to
        // diverge, but the set of live ledger entries at the end of the run --
        // "what the state of the bucket list means" -- should still be
        // identical.
        uint32_t protocolSwitchLedger = *(std::next(
            mDesignatedLedgers.begin(), mDesignatedLedgers.size() / 2));

        auto app =
            createTestApplication<LedgerManagerTestApplication>(*clock, cfg);
        app->start();
        CLOG(INFO, "Bucket")
            << "Running stop/restart test in ledger range 2.." << std::dec
            << finalLedger << " = 0x" << std::hex << finalLedger;
        for (uint32_t i = 2;
             !app->getClock().getIOContext().stopped() && i < finalLedger; ++i)
        {
            LedgerManagerForBucketTests& lm = app->getLedgerManager();
            lm.setNextLedgerEntryBatchForBucketTesting(
                mInitEntryBatches[i - 2], mLiveEntryBatches[i - 2],
                mDeadEntryBatches[i - 2]);
            resolveAllMerges(app->getBucketManager().getBucketList());
            auto countersBeforeClose =
                app->getBucketManager().readMergeCounters();
            closeLedger(*app);

            assert(i == app->getLedgerManager()
                            .getLastClosedLedgerHeader()
                            .header.ledgerSeq);
            auto j = mControlSurveys.find(i);
            if (j != mControlSurveys.end())
            {
                if (BucketList::levelShouldSpill(i, mDesignatedLevel - 1))
                {
                    // Confirm that there's a merge-in-progress at this level
                    // (closing ledger i should have provoked a spill from
                    // mDesignatedLevel-1 to mDesignatedLevel)
                    BucketList& bl = app->getBucketManager().getBucketList();
                    BucketLevel& blv = bl.getLevel(mDesignatedLevel);
                    REQUIRE(blv.getNext().isMerging());
                }

                if (currProtocol == firstProtocol)
                {
                    // Check that the survey matches expectations.
                    Survey s(*app, mDesignatedLevel);
                    s.checkEqual(j->second);
                }

                // Stop the application.
                CLOG(INFO, "Bucket")
                    << "Stopping application after closing ledger " << std::dec
                    << i;
                app.reset();

                if (firstProtocol != secondProtocol &&
                    i == protocolSwitchLedger)
                {
                    CLOG(INFO, "Bucket")
                        << "Switching protocol at ledger " << std::dec << i
                        << " from protocol " << firstProtocol << " to protocol "
                        << secondProtocol;
                    cfg.LEDGER_PROTOCOL_VERSION = secondProtocol;
                }

                // Restart the application.
                CLOG(INFO, "Bucket")
                    << "Restarting application at ledger " << std::dec << i;
                clock = std::make_unique<VirtualClock>();
                app = createTestApplication<LedgerManagerTestApplication>(
                    *clock, cfg, false);
                app->start();
                if (BucketList::levelShouldSpill(i, mDesignatedLevel - 1))
                {
                    // Confirm that the merge-in-progress was restarted.
                    BucketList& bl = app->getBucketManager().getBucketList();
                    BucketLevel& blv = bl.getLevel(mDesignatedLevel);
                    REQUIRE(blv.getNext().isMerging());
                }

                // If there are restarted merges, we need to reset the counters
                // to the values they had _before_ the ledger-close so the
                // restarted merges don't count twice.
                app->getBucketManager().incrMergeCounters(countersBeforeClose);

                if (currProtocol == firstProtocol)
                {
                    // Re-check that the survey matches expectations.
                    Survey s2(*app, mDesignatedLevel);
                    s2.checkEqual(j->second);
                }
            }
        }
        checkAgainstFinalLedgerEntries(*app);
    }

  public:
    StopAndRestartBucketMergesTest(uint32_t protocol, uint32_t designatedLevel)
        : mProtocol(protocol), mDesignatedLevel(designatedLevel)
    {
    }

    void
    run()
    {
        calculateDesignatedLedgers();
        collectControlSurveys();
        assert(!mControlSurveys.empty());
        if (mProtocol >=
            Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY)
        {
            mControlSurveys.rbegin()->second.dumpMergeCounters(
                "control, Post-INITENTRY", mDesignatedLevel);
            mControlSurveys.rbegin()
                ->second.checkSensiblePostInitEntryMergeCounters();
        }
        else
        {
            mControlSurveys.rbegin()->second.dumpMergeCounters(
                "control, Pre-INITENTRY", mDesignatedLevel);
            mControlSurveys.rbegin()
                ->second.checkSensiblePreInitEntryMergeCounters();
        }
        runStopAndRestartTest(mProtocol, mProtocol);
        runStopAndRestartTest(mProtocol, mProtocol + 1);
    }
};

TEST_CASE("bucket persistence over app restart with initentry",
          "[bucket][bucketmanager][bp-initentry][!hide]")
{
    for (uint32_t protocol :
         {Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY - 1,
          Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY})
    {
        for (uint32_t level : {2, 3})
        {
            StopAndRestartBucketMergesTest t(protocol, level);
            t.run();
        }
    }
}

// Same as previous test, but runs a long time; too long to run in CI.
TEST_CASE("bucket persistence over app restart with initentry - extended",
          "[bucket][bucketmanager][bp-initentry-ext][!hide]")
{
    for (uint32_t protocol :
         {Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY - 1,
          Bucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY})
    {
        for (uint32_t level : {2, 3, 4, 5})
        {
            StopAndRestartBucketMergesTest t(protocol, level);
            t.run();
        }
    }
}

TEST_CASE("bucket persistence over app restart",
          "[bucket][bucketmanager][bucketpersist]")
{
    std::vector<stellar::LedgerKey> emptySet;
    std::vector<stellar::LedgerEntry> emptySetEntry;

    VirtualClock clock;
    Config cfg0(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    for_versions_with_differing_bucket_logic(cfg0, [&](Config const& cfg0) {

        Config cfg1(getTestConfig(1, Config::TESTDB_ON_DISK_SQLITE));
        cfg1.LEDGER_PROTOCOL_VERSION = cfg0.LEDGER_PROTOCOL_VERSION;
        cfg1.ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING = true;

        std::vector<std::vector<LedgerEntry>> batches;
        for (uint32_t i = 0; i < 110; ++i)
        {
            batches.push_back(LedgerTestUtils::generateValidLedgerEntries(1));
        }

        // Inject a common object at the first batch we're going to run
        // (batch #2) and at the pause-merge threshold; this makes the
        // pause-merge (#64, where we stop and serialize) sensitive to
        // shadowing, and requires shadows be reconstituted when the merge
        // is restarted.
        auto alice = LedgerTestUtils::generateValidLedgerEntry(1);
        uint32_t pause = 65;
        batches[2].push_back(alice);
        batches[pause - 2].push_back(alice);

        Hash Lh1, Lh2;
        Hash Blh1, Blh2;

        // First, run an application through two ledger closes, picking up
        // the bucket and ledger closes at each.
        {
            Application::pointer app = createTestApplication(clock, cfg0);
            app->start();
            BucketList& bl = app->getBucketManager().getBucketList();

            uint32_t i = 2;
            while (i < pause)
            {
                CLOG(INFO, "Bucket") << "Adding setup phase 1 batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            Lh1 = closeLedger(*app);
            Blh1 = bl.getHash();
            REQUIRE(!isZero(Lh1));
            REQUIRE(!isZero(Blh1));

            while (i < 100)
            {
                CLOG(INFO, "Bucket") << "Adding setup phase 2 batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            Lh2 = closeLedger(*app);
            Blh2 = bl.getHash();
            REQUIRE(!isZero(Blh2));
            REQUIRE(!isZero(Lh2));
        }

        // Next run a new app with a disjoint config one ledger close, and
        // stop it. It should have acquired the same state and ledger.
        {
            Application::pointer app = createTestApplication(clock, cfg1);
            app->start();
            BucketList& bl = app->getBucketManager().getBucketList();

            uint32_t i = 2;
            while (i < pause)
            {
                CLOG(INFO, "Bucket") << "Adding prefix-batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            REQUIRE(hexAbbrev(Lh1) == hexAbbrev(closeLedger(*app)));
            REQUIRE(hexAbbrev(Blh1) == hexAbbrev(bl.getHash()));

            // Confirm that there are merges-in-progress in this checkpoint.
            HistoryArchiveState has(i, bl);
            REQUIRE(!has.futuresAllResolved());
        }

        // Finally *restart* an app on the same config, and see if it can
        // pick up the bucket list correctly.
        cfg1.FORCE_SCP = false;
        {
            Application::pointer app = Application::create(clock, cfg1, false);
            app->start();
            BucketList& bl = app->getBucketManager().getBucketList();

            // Confirm that we re-acquired the close-ledger state.
            REQUIRE(
                hexAbbrev(Lh1) ==
                hexAbbrev(
                    app->getLedgerManager().getLastClosedLedgerHeader().hash));
            REQUIRE(hexAbbrev(Blh1) == hexAbbrev(bl.getHash()));

            uint32_t i = pause;

            // Confirm that merges-in-progress were restarted.
            HistoryArchiveState has(i, bl);
            REQUIRE(!has.futuresAllResolved());

            while (i < 100)
            {
                CLOG(INFO, "Bucket") << "Adding suffix-batch " << i;
                bl.addBatch(*app, i, getAppLedgerVersion(app), {}, batches[i],
                            emptySet);
                i++;
            }

            // Confirm that merges-in-progress finished with expected
            // results.
            REQUIRE(hexAbbrev(Lh2) == hexAbbrev(closeLedger(*app)));
            REQUIRE(hexAbbrev(Blh2) == hexAbbrev(bl.getHash()));
        }
    });
}
