/*
 * Ports of Chromium renderer memory policies to the WebContent process. See ChromiumMemoryPolicy.h for the
 * upstream files, revisions and license.
 */

#include "config.h"
#include "ChromiumMemoryPolicy.h"

#if PLATFORM(MAC)

#include "Logging.h"
#include <JavaScriptCore/CollectionScope.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/MemoryStatistics.h>
#include <JavaScriptCore/VM.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/MemoryRelease.h>
#include <wtf/FastMalloc.h>
#include <wtf/MemoryFootprint.h>
#include <wtf/MemoryPressureHandler.h>
#include <wtf/MonotonicTime.h>
#include <wtf/CryptographicallyRandomNumber.h>

namespace WebKit {

// v8/src/heap/memory-reducer.cc and src/flags/flag-definitions.h.
static constexpr double kShortDelayMs = 500;
static constexpr double kWatchdogDelayMs = 100000;
static constexpr double kCommittedMemoryFactor = 1.1;
static constexpr size_t kCommittedMemoryDelta = 10 * MB;
static constexpr double kMemoryReducerDelayMs = 8000; // memory_reducer_delay_ms
static constexpr double kGCMemoryReducerStartDelayMs = 30000; // gc_memory_reducer_start_delay_ms
static constexpr int kMaxNumberOfGCs = 2; // memory_reducer_gc_count
static constexpr double kSlackMs = 100; // MemoryReducer::ScheduleTimer
// Heap::ActivateMemoryReducerIfNeededOnMainThread: "2 pages for the old, code, and map space + 1 page for new
// space", NormalPage::kPageSize = 256 KB.
static constexpr size_t kMinCommittedMemoryToActivate = 7 * 256 * KB;
static constexpr size_t kMemoryReducerActivationThreshold = 1 * MB; // Heap::NotifyOldGenerationExpansion

static double nowMs()
{
    return MonotonicTime::now().secondsSinceEpoch().milliseconds();
}

V8MemoryReducer::V8MemoryReducer(JSC::VM& vm)
    : m_vm(vm)
    , m_timer(RunLoop::mainSingleton(), "V8MemoryReducer::Timer"_s, this, &V8MemoryReducer::timerFired)
{
    JSC::JSLockHolder lock(vm);
    m_capacityAfterBootstrap = vm.heap.capacity();
    vm.heap.addObserver(this);
}

V8MemoryReducer::~V8MemoryReducer()
{
    JSC::JSLockHolder lock(m_vm.get());
    m_vm->heap.removeObserver(this);
}

static bool watchdogGC(const V8MemoryReducer::State& state, const V8MemoryReducer::Event& event)
{
    return state.lastGCTimeMs && event.timeMs > state.lastGCTimeMs + kWatchdogDelayMs;
}

V8MemoryReducer::State V8MemoryReducer::step(const State& state, const Event& event)
{
    switch (state.id) {
    case Id::Uninit:
    case Id::Done:
        if (event.type == EventType::Timer)
            return state;
        if (event.type == EventType::MarkCompact) {
            if (event.committedMemory < std::max(static_cast<size_t>(state.committedMemoryAtLastRun * kCommittedMemoryFactor), state.committedMemoryAtLastRun + kCommittedMemoryDelta))
                return state;
            return State::createWait(0, event.timeMs + kMemoryReducerDelayMs, event.timeMs);
        }
        ASSERT(event.type == EventType::PossibleGarbage);
        return State::createWait(0, event.timeMs + kGCMemoryReducerStartDelayMs, state.lastGCTimeMs);
    case Id::Wait:
        switch (event.type) {
        case EventType::PossibleGarbage:
            return state;
        case EventType::Timer:
            if (state.startedGCs >= kMaxNumberOfGCs)
                return State::createDone(state.lastGCTimeMs, event.committedMemory);
            if (event.canStartIncrementalGC && (event.shouldStartIncrementalGC || watchdogGC(state, event))) {
                if (state.nextGCStartMs <= event.timeMs)
                    return State::createRun(state.startedGCs + 1);
                return state;
            }
            return State::createWait(state.startedGCs, event.timeMs + kMemoryReducerDelayMs, state.lastGCTimeMs);
        case EventType::MarkCompact:
            return State::createWait(state.startedGCs, event.timeMs + kMemoryReducerDelayMs, event.timeMs);
        }
        break;
    case Id::Run:
        if (event.type == EventType::MarkCompact) {
            if (state.startedGCs < kMaxNumberOfGCs && (event.nextGCLikelyToCollectMore || state.startedGCs == 1))
                return State::createWait(state.startedGCs, event.timeMs + kShortDelayMs, event.timeMs);
            return State::createDone(event.timeMs, event.committedMemory);
        }
        return state;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void V8MemoryReducer::scheduleTimer(double delayMs)
{
    if (m_vm->heap.isShuttingDown())
        return;
    // "Leave some room for precision error in task scheduler."
    m_timer.startOneShot(Seconds::fromMilliseconds(std::max(delayMs, 0.0) + kSlackMs));
}

void V8MemoryReducer::didChangeState()
{
    // MemoryReducer::ShouldGrowHeapSlowly feeds Heap::CurrentHeapGrowingMode.
    JSC::setHeapMemoryReducerIsDone(m_vm.get(), m_state.id == Id::Done);
}

void V8MemoryReducer::timerFired()
{
    auto statistics = JSC::sampleHeapGrowingStatistics(m_vm.get());
    // "The memory reducer will start incremental marking if
    //  1) mutator is likely idle: js call rate is low and allocation rate is low.
    //  2) mutator is in background: optimize for memory flag is set."
    Event event {
        EventType::Timer,
        nowMs(),
        statistics.committedMemory,
        false,
        statistics.hasLowAllocationRate || statistics.shouldOptimizeForMemoryUsage,
        !statistics.isCollecting,
    };
    RELEASE_LOG(MemoryPressure, "V8MemoryReducer: timer, %" PUBLIC_LOG_STRING " alloc, %" PUBLIC_LOG_STRING, statistics.hasLowAllocationRate ? "low" : "high", statistics.shouldOptimizeForMemoryUsage ? "background" : "foreground");
    notifyTimer(event);
}

void V8MemoryReducer::notifyTimer(const Event& event)
{
    if (m_state.id != Id::Wait)
        return;
    ASSERT(event.type == EventType::Timer);
    m_state = step(m_state, event);
    didChangeState();
    if (m_state.id == Id::Run) {
        RELEASE_LOG(MemoryPressure, "V8MemoryReducer: started GC #%d", m_state.startedGCs);
        JSC::startMemoryReducerCollection(m_vm.get());
        if (m_state.startedGCs == 1)
            trimCachesWhileIdle();
    } else if (m_state.id == Id::Wait) {
        // Re-schedule the timer.
        scheduleTimer(m_state.nextGCStartMs - event.timeMs);
    }
}

void V8MemoryReducer::trimCachesWhileIdle()
{
    // Beyond Chromium, at the user's request: the reducer only runs once the page has gone quiet, so also drop what
    // WebKit otherwise keeps until memory pressure: unreferenced cached resources, decoded image data (re-decoded on
    // the next paint), font/style/layout caches, and the allocator's free pages. JIT code and the back/forward cache
    // are kept so resuming stays fast.
    auto footprintBefore = WTF::memoryFootprint();
    WebCore::releaseMemory(Critical::No, Synchronous::No, WebCore::MaintainBackForwardCache::Yes, WebCore::MaintainMemoryCache::No);
    WebCore::MemoryCache::singleton().pruneLiveResourcesToSize(0, true);
    WTF::releaseFastMallocFreeMemory();
    auto footprintAfter = WTF::memoryFootprint();
    RELEASE_LOG(MemoryPressure, "V8MemoryReducer: idle cache trim %zu MB -> %zu MB", footprintBefore / MB, footprintAfter / MB);
}

void V8MemoryReducer::didGarbageCollect(JSC::CollectionScope scope)
{
    RunLoop::mainSingleton().dispatch([weakThis = ThreadSafeWeakPtr { *this }, scope] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (scope == JSC::CollectionScope::Full) {
            protectedThis->m_didFullCollection = true;
            protectedThis->notifyMarkCompact();
        } else
            protectedThis->maybeNotifyOldGenerationExpansion();
    });
}

void V8MemoryReducer::notifyMarkCompact()
{
    size_t sizeBefore = 0;
    size_t sizeAfter = 0;
    {
        JSC::JSLockHolder lock(m_vm.get());
        sizeBefore = m_vm->heap.sizeBeforeLastFullCollection();
        sizeAfter = m_vm->heap.sizeAfterLastFullCollection();
    }
    auto statistics = JSC::sampleHeapGrowingStatistics(m_vm.get());
    // "Trigger one more GC if
    //  - this GC decreased committed memory,
    //  - there is high fragmentation,"
    Event event {
        EventType::MarkCompact,
        nowMs(),
        statistics.committedMemory,
        sizeBefore > sizeAfter + MB || statistics.hasHighFragmentation,
        false,
        false,
    };
    auto oldState = m_state;
    m_state = step(m_state, event);
    didChangeState();
    // If we are transitioning to the WAIT state, start the timer.
    if (oldState.id != Id::Wait && m_state.id == Id::Wait)
        scheduleTimer(m_state.nextGCStartMs - event.timeMs);
    if (oldState.id == Id::Run)
        RELEASE_LOG(MemoryPressure, "V8MemoryReducer: finished GC #%d (%" PUBLIC_LOG_STRING "), committed %zu KB", oldState.startedGCs, m_state.id == Id::Wait ? "will do more" : "done", statistics.committedMemory / KB);
}

void V8MemoryReducer::notifyPossibleGarbage()
{
    Event event { EventType::PossibleGarbage, nowMs(), 0, false, false, false };
    auto oldId = m_state.id;
    m_state = step(m_state, event);
    didChangeState();
    // If we are transitioning to the WAIT state, start the timer.
    if (oldId != Id::Wait && m_state.id == Id::Wait)
        scheduleTimer(m_state.nextGCStartMs - event.timeMs);
}

void V8MemoryReducer::activateIfNeeded()
{
    // "Activate memory reducer when switching to background if
    //  - there was no mark compact since the start.
    //  - the committed memory can be potentially reduced."
    if (m_didFullCollection)
        return;
    size_t committedMemory = 0;
    {
        JSC::JSLockHolder lock(m_vm.get());
        committedMemory = m_vm->heap.capacity();
    }
    if (committedMemory > kMinCommittedMemoryToActivate)
        notifyPossibleGarbage();
}

void V8MemoryReducer::maybeNotifyOldGenerationExpansion()
{
    if (m_didFullCollection || !m_capacityAfterBootstrap)
        return;
    size_t capacity = 0;
    {
        JSC::JSLockHolder lock(m_vm.get());
        capacity = m_vm->heap.capacity();
    }
    if (capacity >= m_capacityAfterBootstrap + kMemoryReducerActivationThreshold)
        notifyPossibleGarbage();
}

BlinkMemoryPurgeManager::BlinkMemoryPurgeManager(Function<void()>&& purge)
    : m_purge(WTF::move(purge))
    , m_purgeTimer(RunLoop::mainSingleton(), "BlinkMemoryPurgeManager::PurgeTimer"_s, [this] { performMemoryPurge(); })
{
}

void BlinkMemoryPurgeManager::onPageCreated()
{
    m_totalPageCount++;
    if (!canPurge())
        m_purgeTimer.stop();
}

void BlinkMemoryPurgeManager::onPageDestroyed()
{
    ASSERT(m_totalPageCount);
    if (m_totalPageCount)
        m_totalPageCount--;
    if (!canPurge())
        m_purgeTimer.stop();
}

void BlinkMemoryPurgeManager::setRendererBackgrounded(bool backgrounded)
{
    if (m_rendererBackgrounded == backgrounded)
        return;
    m_rendererBackgrounded = backgrounded;
    if (backgrounded)
        onRendererBackgrounded();
    else
        onRendererForegrounded();
}

void BlinkMemoryPurgeManager::onRendererBackgrounded()
{
    // Do not purge memory on an empty renderer (e.g. spare renderer).
    if (!m_totalPageCount)
        return;
    m_backgroundedPurgePending = true;
    requestMemoryPurgeWithDelay(timeToPurgeAfterBackgrounded());
}

void BlinkMemoryPurgeManager::onRendererForegrounded()
{
    m_backgroundedPurgePending = false;
    m_purgeTimer.stop();
}

void BlinkMemoryPurgeManager::requestMemoryPurgeWithDelay(Seconds delay)
{
    if (!m_purgeTimer.isActive())
        m_purgeTimer.startOneShot(delay);
}

void BlinkMemoryPurgeManager::performMemoryPurge()
{
    if (!canPurge())
        return;
    if (m_backgroundedPurgePending) {
        RELEASE_LOG(MemoryPressure, "BlinkMemoryPurgeManager: purging backgrounded renderer");
        // base::SetMemoryPressureLevelFromAnyThread(MEMORY_PRESSURE_LEVEL_CRITICAL).
        m_purge();
    }
    m_backgroundedPurgePending = false;
}

bool BlinkMemoryPurgeManager::canPurge() const
{
    if (!m_totalPageCount)
        return false;
    if (m_backgroundedPurgePending)
        return true;
    return m_rendererBackgrounded;
}

Seconds BlinkMemoryPurgeManager::timeToPurgeAfterBackgrounded() const
{
    // base::RandIntInclusive(min seconds, max seconds).
    auto minSeconds = static_cast<unsigned>(defaultMinTimeToPurgeAfterBackgrounded.seconds());
    auto maxSeconds = static_cast<unsigned>(defaultMaxTimeToPurgeAfterBackgrounded.seconds());
    unsigned seconds = minSeconds + static_cast<unsigned>(cryptographicallyRandomUnitInterval() * (maxSeconds - minSeconds + 1));
    return Seconds(std::min(seconds, maxSeconds));
}

} // namespace WebKit

#endif // PLATFORM(MAC)
