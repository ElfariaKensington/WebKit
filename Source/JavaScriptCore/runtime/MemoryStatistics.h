/* 
 * Copyright (C) 2010 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <cstdint>

class VM;

namespace JSC {

struct GlobalMemoryStatistics {
    size_t stackBytes;
    size_t JITBytes = 0;
};

JS_EXPORT_PRIVATE GlobalMemoryStatistics globalMemoryStatistics();

#if PLATFORM(MAC)
class VM;

// Embedder interface to the V8 heap-limit port in Heap.cpp (v8/src/heap/heap-controller.cc and heap.cc).
// The setters are process-wide, like the isolate-level V8 notifications they mirror.
JS_EXPORT_PRIVATE void setHeapIsBackgrounded(bool); // v8::Isolate::SetPriority(Priority::kBestEffort)
JS_EXPORT_PRIVATE void setHeapHighMemoryPressure(bool); // v8::Isolate::MemoryPressureNotification(kCritical / kNone)

// What V8's MemoryReducer::TimerTask reads from the heap; taking it also samples the allocation rate.
struct HeapGrowingStatistics {
    size_t sizeOfObjects { 0 };
    size_t committedMemory { 0 };
    unsigned fullCollectionCount { 0 };
    bool isCollecting { false };
    bool hasLowAllocationRate { false }; // Heap::HasLowAllocationRate
    bool shouldOptimizeForMemoryUsage { false }; // Heap::ShouldOptimizeForMemoryUsage
    bool hasHighFragmentation { false }; // Heap::HasHighFragmentation
};
JS_EXPORT_PRIVATE HeapGrowingStatistics sampleHeapGrowingStatistics(VM&);
// Heap::StartIncrementalMarking(GCFlag::kReduceMemoryFootprint, kMemoryReducer): a concurrent full collection
// whose new limit uses HeapGrowingMode::kMinimal.
JS_EXPORT_PRIVATE void startMemoryReducerCollection(VM&);
JS_EXPORT_PRIVATE void setHeapMemoryReducerIsDone(VM&, bool); // MemoryReducer::ShouldGrowHeapSlowly
// v8::Isolate::ContextDisposedNotification(kNoDependants) -> Heap::NotifyContextDisposed: the main frame's global
// object went away (navigation or reload), so the allocation limit returns to the initial limit, which then shrinks
// toward the live size on every young collection and brings the old page's garbage to a full collection quickly.
JS_EXPORT_PRIVATE void notifyHeapContextDisposed(VM&);
#endif

} // namespace JSC
