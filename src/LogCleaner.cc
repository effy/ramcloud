/* Copyright (c) 2010-2013 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <stdint.h>

#include "Common.h"
#include "Fence.h"
#include "Log.h"
#include "LogCleaner.h"
#include "ShortMacros.h"
#include "Segment.h"
#include "SegmentIterator.h"
#include "ServerConfig.h"
#include "WallTime.h"

namespace RAMCloud {

/**
 * Construct a new LogCleaner object. The cleaner will not perform any garbage
 * collection until the start() method is invoked.
 *
 * \param context
 *      Overall information about the RAMCloud server.
 * \param config
 *      Server configuration from which the cleaner will extract any runtime
 *      parameters that affect its operation.
 * \param segmentManager
 *      The SegmentManager to query for newly cleanable segments, allocate
 *      survivor segments from, and report cleaned segments to.
 * \param replicaManager
 *      The ReplicaManager to use in backing up segments written out by the
 *      cleaner.
 * \param entryHandlers
 *     Class responsible for entries stored in the log. The cleaner will invoke
 *     it when they are being relocated during cleaning, for example.
 */
LogCleaner::LogCleaner(Context* context,
                       const ServerConfig* config,
                       SegmentManager& segmentManager,
                       ReplicaManager& replicaManager,
                       LogEntryHandlers& entryHandlers)
    : context(context),
      segmentManager(segmentManager),
      replicaManager(replicaManager),
      entryHandlers(entryHandlers),
      writeCostThreshold(config->master.cleanerWriteCostThreshold),
      disableInMemoryCleaning(config->master.disableInMemoryCleaning),
      numThreads(config->master.cleanerThreadCount),
      candidates(),
      candidatesLock("LogCleaner::candidatesLock"),
      segletSize(config->segletSize),
      segmentSize(config->segmentSize),
      doWorkTicks(0),
      doWorkSleepTicks(0),
      inMemoryMetrics(),
      onDiskMetrics(),
      threadMetrics(numThreads),
      threadsShouldExit(false),
      threads()
{
    if (!segmentManager.initializeSurvivorReserve(numThreads *
                                                  SURVIVOR_SEGMENTS_TO_RESERVE))
        throw FatalError(HERE, "Could not reserve survivor segments");

    if (writeCostThreshold == 0)
        disableInMemoryCleaning = true;

    for (int i = 0; i < numThreads; i++)
        threads.push_back(NULL);
}

/**
 * Destroy the cleaner. Any running threads are stopped first.
 */
LogCleaner::~LogCleaner()
{
    stop();
    TEST_LOG("destroyed");
}

/**
 * Start the log cleaner, if it isn't already running. This spins a thread that
 * continually cleans if there's work to do until stop() is called.
 *
 * The cleaner will not do any work until explicitly enabled via this method.
 *
 * This method may be called any number of times, but it is not thread-safe.
 * That is, do not call start() and stop() in parallel.
 */
void
LogCleaner::start()
{
    for (int i = 0; i < numThreads; i++) {
        if (threads[i] == NULL)
            threads[i] = new std::thread(cleanerThreadEntry, this, context);
    }
}

/**
 * Halt the cleaner thread (if it is running). Once halted, it will do no more
 * work until start() is called again.
 *
 * This method may be called any number of times, but it is not thread-safe.
 * That is, do not call start() and stop() in parallel.
 */
void
LogCleaner::stop()
{
    threadsShouldExit = true;
    Fence::sfence();

    for (int i = 0; i < numThreads; i++) {
        if (threads[i] != NULL) {
            threads[i]->join();
            delete threads[i];
            threads[i] = NULL;
        }
    }

    threadsShouldExit = false;
}

/**
 * Fill in the provided protocol buffer with metrics, giving other modules and
 * servers insight into what's happening in the cleaner.
 */
void
LogCleaner::getMetrics(ProtoBuf::LogMetrics_CleanerMetrics& m)
{
    m.set_poll_usec(POLL_USEC);
    m.set_max_cleanable_memory_utilization(MAX_CLEANABLE_MEMORY_UTILIZATION);
    m.set_live_segments_per_disk_pass(MAX_LIVE_SEGMENTS_PER_DISK_PASS);
    m.set_survivor_segments_to_reserve(SURVIVOR_SEGMENTS_TO_RESERVE);
    m.set_min_memory_utilization(MIN_MEMORY_UTILIZATION);
    m.set_min_disk_utilization(MIN_DISK_UTILIZATION);
    m.set_do_work_ticks(doWorkTicks);
    m.set_do_work_sleep_ticks(doWorkSleepTicks);
    inMemoryMetrics.serialize(*m.mutable_in_memory_metrics());
    onDiskMetrics.serialize(*m.mutable_on_disk_metrics());
    threadMetrics.serialize(*m.mutable_thread_metrics());
}

/******************************************************************************
 * PRIVATE METHODS
 ******************************************************************************/

static volatile uint32_t threadCnt;

/**
 * Static entry point for the cleaner thread. This is invoked via the
 * std::thread() constructor. This thread performs continuous cleaning on an
 * as-needed basis.
 */
void
LogCleaner::cleanerThreadEntry(LogCleaner* logCleaner, Context* context)
{
    LOG(NOTICE, "LogCleaner thread started");

    CleanerThreadState state;
    state.threadNumber = __sync_fetch_and_add(&threadCnt, 1);
    try {
        while (1) {
            Fence::lfence();
            if (logCleaner->threadsShouldExit)
                break;

            logCleaner->doWork(&state);
        }
    } catch (const Exception& e) {
        DIE("Fatal error in cleaner thread: %s", e.what());
    }

    LOG(NOTICE, "LogCleaner thread stopping");
}

/**
 * Main cleaning loop, constantly invoked via cleanerThreadEntry(). If there
 * is cleaning to be done, do it now return. If no work is to be done, sleep for
 * a bit before returning (and getting called again), rather than banging on the
 * CPU.
 */
void
LogCleaner::doWork(CleanerThreadState* state)
{
    MetricCycleCounter _(&doWorkTicks);

    threadMetrics.noteThreadStart();

    // Update our list of candidates whether we need to clean or not (it's
    // better not to put off work until we really need to clean).
    candidatesLock.lock();
    segmentManager.cleanableSegments(candidates);
    candidatesLock.unlock();

    int memUtil = segmentManager.getAllocator().getMemoryUtilization();
    bool lowOnMemory = (segmentManager.getAllocator().getMemoryUtilization() >=
                        MIN_MEMORY_UTILIZATION);
    bool notKeepingUp = (segmentManager.getAllocator().getMemoryUtilization() >=
                         MEMORY_DEPLETED_UTILIZATION);
    bool lowOnDiskSpace = (segmentManager.getSegmentUtilization() >=
                           MIN_DISK_UTILIZATION);
    bool haveWorkToDo = (lowOnMemory || lowOnDiskSpace);

    if (haveWorkToDo) {
        if (state->threadNumber == 0) {
            if (lowOnDiskSpace || notKeepingUp)
                doDiskCleaning(lowOnDiskSpace);
            else
                doMemoryCleaning();
        } else {
            int threshold = 90 + 2 * static_cast<int>(state->threadNumber);
            if (memUtil >= std::min(99, threshold))
                doMemoryCleaning();
            else
                haveWorkToDo = false;
        }
    }

    threadMetrics.noteThreadStop();

    if (!haveWorkToDo) {
        MetricCycleCounter __(&doWorkSleepTicks);
        // Jitter the sleep delay a little bit (up to 10%). It's not a big deal
        // if we don't, but it can make some locks look artificially contended
        // when there's no cleaning to be done and threads manage to caravan
        // together.
        useconds_t r = downCast<useconds_t>(generateRandom() % POLL_USEC) / 10;
        usleep(POLL_USEC + r);
    }
}

/**
 * Perform an in-memory cleaning pass. This takes a segment and compacts it,
 * re-packing all live entries together sequentially, allowing us to reclaim
 * some of the dead space.
 */
uint64_t
LogCleaner::doMemoryCleaning()
{
    TEST_LOG("called");
    MetricCycleCounter _(&inMemoryMetrics.totalTicks);

    if (disableInMemoryCleaning)
        return 0;

    uint32_t freeableSeglets;
    LogSegment* segment = getSegmentToCompact(freeableSeglets);
    if (segment == NULL)
        return 0;

    // Allocate a survivor segment to write into. This call may block if one
    // is not available right now.
    MetricCycleCounter waitTicks(&inMemoryMetrics.waitForFreeSurvivorTicks);
    LogSegment* survivor = segmentManager.allocSideSegment(
            SegmentManager::FOR_CLEANING | SegmentManager::MUST_NOT_FAIL,
            segment);
    assert(survivor != NULL);
    waitTicks.stop();

    inMemoryMetrics.totalBytesInCompactedSegments +=
        segment->getSegletsAllocated() * segletSize;
    uint64_t entriesScanned[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint64_t liveEntriesScanned[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint64_t scannedEntryLengths[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint64_t liveScannedEntryLengths[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint32_t bytesAppended = 0;

    for (SegmentIterator it(*segment); !it.isDone(); it.next()) {
        LogEntryType type = it.getType();
        Buffer buffer;
        it.appendToBuffer(buffer);
        Log::Reference reference = segment->getReference(it.getOffset());

        RelocStatus s = relocateEntry(type,
                                      buffer,
                                      reference,
                                      survivor,
                                      inMemoryMetrics,
                                      &bytesAppended);
        if (expect_false(s == RELOCATION_FAILED))
            throw FatalError(HERE, "Entry didn't fit into survivor!");

        entriesScanned[type]++;
        scannedEntryLengths[type] += buffer.getTotalLength();
        if (expect_true(s == RELOCATED)) {
            liveEntriesScanned[type]++;
            liveScannedEntryLengths[type] += buffer.getTotalLength();
        }
    }

    // Be sure to update the usage statistics for the new segment. This
    // is done once here, rather than for each relocated entry because
    // it avoids the expense of atomically updating two separate fields.
    survivor->liveBytes += bytesAppended;

    for (size_t i = 0; i < TOTAL_LOG_ENTRY_TYPES; i++) {
        inMemoryMetrics.totalEntriesScanned[i] += entriesScanned[i];
        inMemoryMetrics.totalLiveEntriesScanned[i] += liveEntriesScanned[i];
        inMemoryMetrics.totalScannedEntryLengths[i] += scannedEntryLengths[i];
        inMemoryMetrics.totalLiveScannedEntryLengths[i] +=
            liveScannedEntryLengths[i];
    }

    // The survivor segment has at least as many seglets allocated as the one
    // we've compacted (it was freshly allocated, so it has the maximum number
    // of seglets). We can therefore free the difference (which ensures a 0 net
    // gain) plus the number extra seglets that getSegmentToCompact() told us
    // we could free. That value was carefully calculated to ensure that future
    // disk cleaning will make forward progress (not use more seglets after
    // cleaning than before).
    uint32_t segletsToFree = survivor->getSegletsAllocated() -
                             segment->getSegletsAllocated() +
                             freeableSeglets;

    survivor->close();
    bool r = survivor->freeUnusedSeglets(segletsToFree);
    assert(r);

    uint64_t bytesFreed = freeableSeglets * segletSize;
    inMemoryMetrics.totalBytesFreed += bytesFreed;
    inMemoryMetrics.totalBytesAppendedToSurvivors +=
        survivor->getAppendedLength();
    inMemoryMetrics.totalSegmentsCompacted++;

    MetricCycleCounter __(&inMemoryMetrics.compactionCompleteTicks);
    segmentManager.compactionComplete(segment, survivor);

    return static_cast<uint64_t>(
        static_cast<double>(bytesFreed) / Cycles::toSeconds(_.stop()));
}

/**
 * Perform a disk cleaning pass if possible. Doing so involves choosing segments
 * to clean, extracting entries from those segments, writing them out into new
 * "survivor" segments, and alerting the segment manager upon completion.
 */
uint64_t
LogCleaner::doDiskCleaning(bool lowOnDiskSpace)
{
    TEST_LOG("called");
    MetricCycleCounter _(&onDiskMetrics.totalTicks);

    // Obtain the segments we'll clean in this pass. We're guaranteed to have
    // the resources to clean what's returned.
    LogSegmentVector segmentsToClean;
    getSegmentsToClean(segmentsToClean);

    if (segmentsToClean.size() == 0)
        return 0;

    // Extract the currently live entries of the segments we're cleaning and
    // sort them by age.
    EntryVector entries;
    getSortedEntries(segmentsToClean, entries);

    uint64_t maxLiveBytes = 0;
    uint32_t segletsBefore = 0;
    foreach (LogSegment* segment, segmentsToClean) {
        uint64_t liveBytes = segment->liveBytes;
        if (liveBytes == 0)
            onDiskMetrics.totalEmptySegmentsCleaned++;
        maxLiveBytes += liveBytes;
        segletsBefore += segment->getSegletsAllocated();
    }

    // Relocate the live entries to survivor segments.
    LogSegmentVector survivors;
    uint64_t entryBytesAppended = relocateLiveEntries(entries, survivors);

    uint32_t segmentsAfter = downCast<uint32_t>(survivors.size());
    uint32_t segletsAfter = 0;
    foreach (LogSegment* segment, survivors)
        segletsAfter += segment->getSegletsAllocated();

    TEST_LOG("used %u seglets and %u segments", segletsAfter, segmentsAfter);

    // If this doesn't hold, then our statistics are wrong. Perhaps
    // MasterService is issuing a log->free(), but is leaving a reference in
    // the hash table.
    assert(entryBytesAppended <= maxLiveBytes);

    uint32_t segmentsBefore = downCast<uint32_t>(segmentsToClean.size());
    assert(segletsBefore >= segletsAfter);
    assert(segmentsBefore >= segmentsAfter);

    uint64_t memoryBytesFreed = (segletsBefore - segletsAfter) * segletSize;
    uint64_t diskBytesFreed = (segmentsBefore - segmentsAfter) * segmentSize;
    onDiskMetrics.totalMemoryBytesFreed += memoryBytesFreed;
    onDiskMetrics.totalDiskBytesFreed += diskBytesFreed;
    onDiskMetrics.totalSegmentsCleaned += segmentsToClean.size();
    onDiskMetrics.totalSurvivorsCreated += survivors.size();
    onDiskMetrics.totalRuns++;
    if (lowOnDiskSpace)
        onDiskMetrics.totalLowDiskSpaceRuns++;

    MetricCycleCounter __(&onDiskMetrics.cleaningCompleteTicks);
    segmentManager.cleaningComplete(segmentsToClean, survivors);

    return static_cast<uint64_t>(
        static_cast<double>(memoryBytesFreed) / Cycles::toSeconds(_.stop()));
}

/**
 * Choose the best segment to clean in memory. We greedily choose the segment
 * with the most freeable seglets. Care is taken to ensure that we determine the
 * number of freeable seglets that will keep the segment under our maximum
 * cleanable utilization after compaction. This ensures that we will always be
 * able to use the compacted version of this segment during disk cleaning.
 *
 * \param[out] outFreeableSeglets
 *      The maximum number of seglets the caller should free from this segment
 *      is returned here. Freeing any more may make it impossible to clean the
 *      resulting compacted segment on disk, which may deadlock the system if
 *      it prevents freeing up tombstones in other segments.
 */
LogSegment*
LogCleaner::getSegmentToCompact(uint32_t& outFreeableSeglets)
{
    MetricCycleCounter _(&inMemoryMetrics.getSegmentToCompactTicks);
    Lock guard(candidatesLock);

    size_t bestIndex = -1;
    uint32_t bestDelta = 0;
    for (size_t i = 0; i < candidates.size(); i++) {
        LogSegment* candidate = candidates[i];
        uint32_t liveBytes = candidate->liveBytes;
        uint32_t segletsNeeded = (100 * (liveBytes + segletSize - 1)) /
                                 segletSize / MAX_CLEANABLE_MEMORY_UTILIZATION;
        uint32_t segletsAllocated = candidate->getSegletsAllocated();
        uint32_t delta = segletsAllocated - segletsNeeded;
        if (segletsNeeded < segletsAllocated && delta > bestDelta) {
            bestIndex = i;
            bestDelta = delta;
        }
    }

    // If we don't think any memory can be safely freed then either we're full
    // up with live objects (in which case nothing's wrong and we can't actually
    // do anything), or we have a lot of dead tombstones sitting around that is
    // causing us to assume that we're full when we really aren't. This can
    // happen because we don't (and can't easily) keep track of which tombstones
    // are dead, so all are presumed to be alive. When objects are large and
    // tombstones are relatively small, this accounting error is usually
    // harmless. However, when storing very small objects, tombstones are
    // relatively large and our percentage error can be significant. Also, one
    // can imagine what'd happen if the cleaner somehow groups tombstones
    // together into their own segments that always appear to have 100% live
    // entries.
    //
    // So, to address this we'll try compacting the candidate segment with the
    // most tombstones that has not been compacted in a while. Hopefully this
    // will choose the one with the most dead tombstones and shake us out of
    // any deadlock.
    //
    // It's entirely possible that we'd want to do this earlier (before we run
    // out of candidates above, rather than as a last ditch effort). It's not
    // clear to me, however, when we'd decide and what would give the best
    // overall performance.
    //
    // Did I ever mention how much I hate tombstones?
    if (bestIndex == static_cast<size_t>(-1)) {
        __uint128_t bestGoodness = 0;
        for (size_t i = 0; i < candidates.size(); i++) {
            LogSegment* candidate = candidates[i];
            uint32_t tombstoneCount =
                candidate->getEntryCount(LOG_ENTRY_TYPE_OBJTOMB);
            uint64_t timeSinceLastCompaction = WallTime::secondsTimestamp() -
                                            candidate->lastCompactionTimestamp;
            __uint128_t goodness =
                (__uint128_t)tombstoneCount * timeSinceLastCompaction;
            if (goodness > bestGoodness) {
                bestIndex = i;
                bestGoodness = goodness;
            }
        }

        // Still no dice. Looks like we're just full of live data.
        if (bestIndex == static_cast<size_t>(-1))
            return NULL;

        // It's not safe for the compactor to free any memory this time around
        // (it could be that no tombstones were dead, or we will free too few to
        // allow us to free seglets while still guaranteeing forward progress
        // of the disk cleaner).
        //
        // If we do free enough memory, we'll get it back in a subsequent pass.
        // The alternative would be to have a separate algorithm that just
        // counts dead tombstones and updates the liveness counters. That is
        // slightly more complicated. Perhaps if this becomes frequently enough
        // we can optimise that case.
        bestDelta = 0;
    }

    LogSegment* best = candidates[bestIndex];
    candidates[bestIndex] = candidates.back();
    candidates.pop_back();

    outFreeableSeglets = bestDelta;
    return best;
}

void
LogCleaner::sortSegmentsByCostBenefit(LogSegmentVector& segments)
{
    MetricCycleCounter _(&onDiskMetrics.costBenefitSortTicks);

    // Sort segments so that the best candidates are at the front of the vector.
    // We could probably use a heap instead and go a little faster, but it's not
    // easy to say how many top candidates we'd want to track in the heap since
    // they could each have significantly different numbers of seglets.
    std::sort(segments.begin(), segments.end(), CostBenefitComparer());
}

/**
 * Compute the best segments to clean on disk and return a set of them that we
 * are guaranteed to be able to clean while consuming no more space in memory
 * than they currently take up.
 *
 * \param[out] outSegmentsToClean
 *      Vector in which segments chosen for cleaning are returned.
 * \return
 *      Returns the total number of seglets allocated in the segments chosen for
 *      cleaning.
 */
void
LogCleaner::getSegmentsToClean(LogSegmentVector& outSegmentsToClean)
{
    MetricCycleCounter _(&onDiskMetrics.getSegmentsToCleanTicks);
    Lock guard(candidatesLock);

    foreach (LogSegment* segment, candidates) {
        onDiskMetrics.allSegmentsDiskHistogram.storeSample(
            segment->getDiskUtilization());
    }

    sortSegmentsByCostBenefit(candidates);

    uint32_t totalSeglets = 0;
    uint64_t totalLiveBytes = 0;
    uint64_t maximumLiveBytes = MAX_LIVE_SEGMENTS_PER_DISK_PASS * segmentSize;
    vector<size_t> chosenIndices;

    for (size_t i = 0; i < candidates.size(); i++) {
        LogSegment* candidate = candidates[i];

        int utilization = candidate->getMemoryUtilization();
        if (utilization > MAX_CLEANABLE_MEMORY_UTILIZATION)
            continue;

        uint64_t liveBytes = candidate->liveBytes;
        if ((totalLiveBytes + liveBytes) > maximumLiveBytes)
            break;

        totalLiveBytes += liveBytes;
        totalSeglets += candidate->getSegletsAllocated();
        outSegmentsToClean.push_back(candidate);
        chosenIndices.push_back(i);
    }

    // Remove chosen segments from the list of candidates. At this point, we've
    // committed to cleaning what we chose and have guaranteed that we have the
    // necessary resources to complete the operation.
    reverse_foreach(size_t i, chosenIndices) {
        candidates[i] = candidates.back();
        candidates.pop_back();
    }

    TEST_LOG("%lu segments selected with %u allocated segments",
        chosenIndices.size(), totalSeglets);
}

/**
 * Sort the given segment entries by their timestamp. Used to sort the survivor
 * data that is written out to multiple segments during disk cleaning. This
 * helps to segregate data we expect to live longer from those likely to be
 * shorter lived, which in turn can reduce future cleaning costs.
 *
 * This happens to sort younger objects first, but the opposite should work just as
 * well.
 *
 * \param entries
 *      Vector containing the entries to sort.
 */
void
LogCleaner::sortEntriesByTimestamp(EntryVector& entries)
{
    MetricCycleCounter _(&onDiskMetrics.timestampSortTicks);
    std::sort(entries.begin(), entries.end(), TimestampComparer());
}

/**
 * Extract a complete list of entries from the given segments we're going to
 * clean and sort them by age.
 *
 * \param segmentsToClean
 *      Vector containing the segments to extract entries from.
 * \param[out] outEntries
 *      Vector containing sorted live entries in the segment.
 */
void
LogCleaner::getSortedEntries(LogSegmentVector& segmentsToClean,
                             EntryVector& outEntries)
{
    MetricCycleCounter _(&onDiskMetrics.getSortedEntriesTicks);

    foreach (LogSegment* segment, segmentsToClean) {
        for (SegmentIterator it(*segment); !it.isDone(); it.next()) {
            LogEntryType type = it.getType();
            Buffer buffer;
            it.appendToBuffer(buffer);
            uint32_t timestamp = entryHandlers.getTimestamp(type, buffer);
            outEntries.push_back(
                Entry(segment, it.getOffset(), timestamp));
        }
    }

    sortEntriesByTimestamp(outEntries);

    // TODO(Steve): Push all of this crap into LogCleanerMetrics. It already
    // knows about the various parts of cleaning, so why not have simple calls
    // into it at interesting points of cleaning and let it extract the needed
    // metrics?
    foreach (LogSegment* segment, segmentsToClean) {
        onDiskMetrics.totalMemoryBytesInCleanedSegments +=
            segment->getSegletsAllocated() * segletSize;
        onDiskMetrics.totalDiskBytesInCleanedSegments += segmentSize;
        onDiskMetrics.cleanedSegmentMemoryHistogram.storeSample(
            segment->getMemoryUtilization());
        onDiskMetrics.cleanedSegmentDiskHistogram.storeSample(
            segment->getDiskUtilization());
    }

    TEST_LOG("%lu entries extracted from %lu segments",
        outEntries.size(), segmentsToClean.size());
}

/**
 * Given a vector of entries from segments being cleaned, write them out to
 * survivor segments in order and alert their owning module (MasterService,
 * usually), that they've been relocated.
 *
 * \param entries 
 *      Vector the entries from segments being cleaned that may need to be
 *      relocated.
 * \param outSurvivors
 *      The new survivor segments created to hold the relocated live data are
 *      returned here.
 * \return
 *      The number of live bytes appended to survivors is returned. This value
 *      includes any segment metadata overhead. This makes it directly
 *      comparable to the per-segment liveness statistics that also include
 *      overhead.
 */
uint64_t
LogCleaner::relocateLiveEntries(EntryVector& entries,
                                LogSegmentVector& outSurvivors)
{
    MetricCycleCounter _(&onDiskMetrics.relocateLiveEntriesTicks);

    LogSegment* survivor = NULL;
    uint64_t currentSurvivorBytesAppended = 0;
    uint64_t entryBytesAppended = 0;
    uint64_t entriesScanned[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint64_t liveEntriesScanned[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint64_t scannedEntryLengths[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint64_t liveScannedEntryLengths[TOTAL_LOG_ENTRY_TYPES] = { 0 };
    uint32_t bytesAppended = 0;

    foreach (Entry& entry, entries) {
        Buffer buffer;
        LogEntryType type = entry.segment->getEntry(entry.offset, buffer);
        Log::Reference reference = entry.segment->getReference(entry.offset);

        RelocStatus s = relocateEntry(type,
                                      buffer,
                                      reference,
                                      survivor,
                                      onDiskMetrics,
                                      &bytesAppended);
        if (s == RELOCATION_FAILED) {
            if (survivor != NULL) {
                survivor->liveBytes += bytesAppended;
                bytesAppended = 0;
                closeSurvivor(survivor);
            }

            // Allocate a survivor segment to write into. This call may block if
            // one is not available right now.
            MetricCycleCounter waitTicks(
                &onDiskMetrics.waitForFreeSurvivorsTicks);
            survivor = segmentManager.allocSideSegment(
                SegmentManager::FOR_CLEANING | SegmentManager::MUST_NOT_FAIL,
                NULL);
            assert(survivor != NULL);
            waitTicks.stop();
            outSurvivors.push_back(survivor);
            currentSurvivorBytesAppended = survivor->getAppendedLength();

            s = relocateEntry(type,
                              buffer,
                              reference,
                              survivor,
                              onDiskMetrics,
                              &bytesAppended);
            if (s == RELOCATION_FAILED)
                throw FatalError(HERE, "Entry didn't fit into empty survivor!");
        }

        entriesScanned[type]++;
        scannedEntryLengths[type] += buffer.getTotalLength();
        if (s == RELOCATED) {
            liveEntriesScanned[type]++;
            liveScannedEntryLengths[type] += buffer.getTotalLength();
        }

        if (survivor != NULL) {
            uint32_t newSurvivorBytesAppended = survivor->getAppendedLength();
            entryBytesAppended += (newSurvivorBytesAppended -
                                   currentSurvivorBytesAppended);
            currentSurvivorBytesAppended = newSurvivorBytesAppended;
        }
    }

    if (survivor != NULL) {
        survivor->liveBytes += bytesAppended;
        closeSurvivor(survivor);
    }

    // Ensure that the survivors have been synced to backups before proceeding.
    foreach (survivor, outSurvivors) {
        MetricCycleCounter __(&onDiskMetrics.survivorSyncTicks);
        survivor->replicatedSegment->sync(survivor->getAppendedLength());
    }

    for (size_t i = 0; i < TOTAL_LOG_ENTRY_TYPES; i++) {
        onDiskMetrics.totalEntriesScanned[i] += entriesScanned[i];
        onDiskMetrics.totalLiveEntriesScanned[i] += liveEntriesScanned[i];
        onDiskMetrics.totalScannedEntryLengths[i] += scannedEntryLengths[i];
        onDiskMetrics.totalLiveScannedEntryLengths[i] +=
            liveScannedEntryLengths[i];
    }

    return entryBytesAppended;
}

/**
 * Close a survivor segment we've written data to as part of a disk cleaning
 * pass and tell the replicaManager to begin flushing it asynchronously to
 * backups. Any unused seglets in the survivor will will be freed for use in
 * new segments.
 *
 * \param survivor
 *      The new disk segment we've written survivor data to.
 */
void
LogCleaner::closeSurvivor(LogSegment* survivor)
{
    MetricCycleCounter _(&onDiskMetrics.closeSurvivorTicks);
    onDiskMetrics.totalBytesAppendedToSurvivors +=
        survivor->getAppendedLength();

    survivor->close();

    // Once the replicatedSegment is told that the segment is closed, it will
    // begin replicating the contents. By closing survivors as we go, we can
    // overlap backup writes with filling up new survivors.
    survivor->replicatedSegment->close();

    // Immediately free any unused seglets.
    bool r = survivor->freeUnusedSeglets(survivor->getSegletsAllocated() -
                                         survivor->getSegletsInUse());
    assert(r);
}

/******************************************************************************
 * LogCleaner::CostBenefitComparer inner class
 ******************************************************************************/

/**
 * Construct a new comparison functor that compares segments by cost-benefit.
 * Used when selecting among candidate segments by first sorting them.
 */
LogCleaner::CostBenefitComparer::CostBenefitComparer()
    : now(WallTime::secondsTimestamp()),
      version(Cycles::rdtsc())
{
}

/**
 * Calculate the cost-benefit ratio (benefit/cost) for the given segment.
 */
uint64_t
LogCleaner::CostBenefitComparer::costBenefit(LogSegment* s)
{
    // If utilization is 0, cost-benefit is infinity.
    uint64_t costBenefit = -1UL;

    int utilization = s->getDiskUtilization();
    if (utilization != 0) {
        uint64_t timestamp = s->creationTimestamp;

        // This generally shouldn't happen, but is possible due to:
        //  1) Unsynchronized TSCs across cores (WallTime uses rdtsc).
        //  2) Unsynchronized clocks and "newer" recovered data in the log.
        if (timestamp > now) {
            LOG(WARNING, "timestamp > now");
            timestamp = now;
        }

        uint64_t age = now - timestamp;
        costBenefit = ((100 - utilization) * age) / utilization;
    }

    return costBenefit;
}

/**
 * Compare two segment's cost-benefit ratios. Higher values (better cleaning
 * candidates) are better, so the less than comparison is inverted.
 */
bool
LogCleaner::CostBenefitComparer::operator()(LogSegment* a, LogSegment* b)
{
    // We must ensure that we maintain the weak strictly ordered constraint,
    // otherwise surprising things may happen in the stl algorithms when
    // segment statistics change and alter the computed cost-benefit of a
    // segment from one comparison to the next during the same sort operation.
    if (a->costBenefitVersion != version) {
        a->costBenefit = costBenefit(a);
        a->costBenefitVersion = version;
    }
    if (b->costBenefitVersion != version) {
        b->costBenefit = costBenefit(b);
        b->costBenefitVersion = version;
    }
    return a->costBenefit > b->costBenefit;
}


} // namespace
