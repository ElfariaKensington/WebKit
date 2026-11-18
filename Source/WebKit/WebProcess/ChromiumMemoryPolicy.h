/*
 * Ports of Chromium renderer memory policies to the WebContent process.
 *
 * V8MemoryReducer is ported from v8/src/heap/memory-reducer.{h,cc}
 *   (Copyright 2015 the V8 project authors; BSD-3-Clause), v8/v8@5688b11b27330c4c3148c8d0fa535b4110561622.
 * BlinkMemoryPurgeManager is ported from
 *   third_party/blink/renderer/platform/scheduler/main_thread/memory_purge_manager.{h,cc}
 *   (Copyright 2019 The Chromium Authors; BSD-3-Clause), chromium/chromium@25cdf5d33309850b64974c2f9fc617fe925746e7.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the conditions of the BSD 3-Clause License in Chromium's and V8's LICENSE files are met.
 */

#pragma once

#if PLATFORM(MAC)

#include <JavaScriptCore/HeapObserver.h>
#include <wtf/FastMalloc.h>
#include <wtf/Function.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/ThreadSafeWeakPtr.h>

namespace JSC {
class VM;
}

namespace WebKit {

// V8's MemoryReducer (the default, non-limit-based one): after a navigation, after the tab goes to the
// background, or after the heap grew, wait until the mutator is idle and then run up to two memory-reducing
// full collections.
class V8MemoryReducer final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<V8MemoryReducer>, public JSC::HeapObserver {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(V8MemoryReducer);
public:
    static Ref<V8MemoryReducer> create(JSC::VM& vm) { return adoptRef(*new V8MemoryReducer(vm)); }
    ~V8MemoryReducer();

    void notifyPossibleGarbage(); // MemoryReducer::NotifyPossibleGarbage
    void activateIfNeeded(); // Heap::ActivateMemoryReducerIfNeededOnMainThread, on backgrounding

    enum class Id : uint8_t { Uninit, Done, Wait, Run };
    struct State {
        Id id { Id::Uninit };
        int startedGCs { 0 };
        double nextGCStartMs { 0 };
        double lastGCTimeMs { 0 };
        size_t committedMemoryAtLastRun { 0 };

        static State createUninitialized() { return { Id::Uninit, 0, 0, 0, 0 }; }
        static State createDone(double lastGCTimeMs, size_t committedMemory) { return { Id::Done, 0, 0, lastGCTimeMs, committedMemory }; }
        static State createWait(int startedGCs, double nextGCTimeMs, double lastGCTimeMs) { return { Id::Wait, startedGCs, nextGCTimeMs, lastGCTimeMs, 0 }; }
        static State createRun(int startedGCs) { return { Id::Run, startedGCs, 0, 0, 0 }; }
    };
    enum class EventType : uint8_t { Timer, MarkCompact, PossibleGarbage };
    struct Event {
        EventType type;
        double timeMs;
        size_t committedMemory;
        bool nextGCLikelyToCollectMore;
        bool shouldStartIncrementalGC;
        bool canStartIncrementalGC;
    };
    static State step(const State&, const Event&); // MemoryReducer::Step

private:
    explicit V8MemoryReducer(JSC::VM&);

    // JSC::HeapObserver; may be called on the collector thread.
    void willGarbageCollect() final { }
    void didGarbageCollect(JSC::CollectionScope) final;

    void notifyMarkCompact(); // MemoryReducer::NotifyMarkCompact
    void notifyTimer(const Event&); // MemoryReducer::NotifyTimer
    void timerFired(); // MemoryReducer::TimerTask::RunInternal
    void scheduleTimer(double delayMs);
    void didChangeState();
    void maybeNotifyOldGenerationExpansion(); // Heap::NotifyOldGenerationExpansion (memory_reducer_for_small_heaps)
    void trimCachesWhileIdle();

    const Ref<JSC::VM> m_vm;
    RunLoop::Timer m_timer;
    State m_state { State::createUninitialized() };
    size_t m_capacityAfterBootstrap { 0 };
    bool m_didFullCollection { false };
};

// Blink's MemoryPurgeManager: once every page of the renderer is in the background, purge memory as if
// under critical memory pressure after a random delay of 1 to 4 minutes, unless the renderer is
// foregrounded first. (Page freezing, the other trigger, has no WebKit equivalent.)
class BlinkMemoryPurgeManager {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BlinkMemoryPurgeManager);
    WTF_MAKE_NONCOPYABLE(BlinkMemoryPurgeManager);
public:
    explicit BlinkMemoryPurgeManager(Function<void()>&& purge);

    void onPageCreated();
    void onPageDestroyed();
    void setRendererBackgrounded(bool);

    static constexpr Seconds defaultMinTimeToPurgeAfterBackgrounded { 60_s }; // kMemoryPurgeInBackgroundMinDelay
    static constexpr Seconds defaultMaxTimeToPurgeAfterBackgrounded { 240_s }; // kDefaultMaxTimeToPurgeAfterBackgrounded

private:
    void onRendererBackgrounded();
    void onRendererForegrounded();
    void requestMemoryPurgeWithDelay(Seconds);
    void performMemoryPurge();
    bool canPurge() const;
    Seconds timeToPurgeAfterBackgrounded() const;

    Function<void()> m_purge;
    RunLoop::Timer m_purgeTimer;
    unsigned m_totalPageCount { 0 };
    bool m_rendererBackgrounded { false };
    bool m_backgroundedPurgePending { false };
};

} // namespace WebKit

#endif // PLATFORM(MAC)
