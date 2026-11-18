/*
 * Urgent page discarding ported from Chromium. See ChromiumPageDiscarding.h for the upstream files, revision and
 * license.
 */

#import "config.h"
#import "ChromiumPageDiscarding.h"

#if PLATFORM(MAC)

#import "Logging.h"
#import "WebFrameProxy.h"
#import "WebInspectorUIProxy.h"
#import "WebPageProxy.h"
#import "WebProcessPool.h"
#import "WebProcessProxy.h"
#import <dispatch/dispatch.h>
#import <wtf/MonotonicTime.h>
#import <wtf/NeverDestroyed.h>

namespace WebKit {

ChromiumPageDiscarding& ChromiumPageDiscarding::singleton()
{
    static NeverDestroyed<ChromiumPageDiscarding> discarding;
    return discarding;
}

ChromiumPageDiscarding::ChromiumPageDiscarding()
    : m_sustainedMemoryPressureTimer(RunLoop::mainSingleton(), "ChromiumPageDiscarding::SustainedMemoryPressureTimer"_s, [this] { handleMemoryPressureEvent(); })
{
}

void ChromiumPageDiscarding::initializeIfNeeded()
{
    if (m_initialized)
        return;
    m_initialized = true;

    // base::mac::MemoryPressureMonitor: DISPATCH_MEMORYPRESSURE_CRITICAL maps to MEMORY_PRESSURE_LEVEL_CRITICAL.
    m_memoryPressureSource = adoptOSObject(dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, dispatch_get_main_queue()));
    dispatch_source_t source = m_memoryPressureSource.get();
    dispatch_source_set_event_handler(source, ^{
        auto status = dispatch_source_get_data(source);
        singleton().memoryPressureLevelChanged(status & DISPATCH_MEMORYPRESSURE_CRITICAL);
    });
    dispatch_resume(source);
}

void ChromiumPageDiscarding::memoryPressureLevelChanged(bool isCritical)
{
    // UrgentPageDiscardingPolicy::OnReleaseMemory acts only at the critical pressure threshold.
    if (!isCritical) {
        m_sustainedMemoryPressureTimer.stop();
        return;
    }
    handleMemoryPressureEvent();
    if (!m_sustainedMemoryPressureTimer.isActive())
        m_sustainedMemoryPressureTimer.startRepeating(sustainedMemoryPressureInterval);
}

ChromiumPageDiscarding::CanDiscardResult ChromiumPageDiscarding::canDiscard(WebPageProxy& page)
{
    // DiscardEligibilityPolicy::CanDiscardWithCustomRecentVisibilityWindow, URGENT reason.
    if (page.isClosed() || page.isDiscarded() || !page.hasRunningProcess() || !page.hasCommittedAnyProvisionalLoads())
        return CanDiscardResult::Disallowed;
    Ref process = page.legacyMainFrameProcess();
    if (process->pageCount() != 1)
        return CanDiscardResult::Disallowed;

    auto now = MonotonicTime::now();
    if (page.isViewVisible())
        return CanDiscardResult::Protected; // kVisible / kActiveTab
    if (now - page.lastVisibilityChangeTimeForDiscarding() < nonVisiblePagesUrgentProtectionTime)
        return CanDiscardResult::Protected; // kRecentlyVisible
    if (page.isPlayingAudio())
        return CanDiscardResult::Protected; // kAudible
    if (page.lastAudibleTimeForDiscarding() && now - page.lastAudibleTimeForDiscarding() < tabAudioProtectionTime)
        return CanDiscardResult::Protected; // kRecentlyAudible
    if (page.isPlayingVideoInPictureInPicture())
        return CanDiscardResult::Protected; // kPictureInPicture
    if (RefPtr mainFrame = page.mainFrame(); mainFrame && mainFrame->mimeType() == "application/pdf"_s)
        return CanDiscardResult::Protected; // kPdf
    URL url { page.currentURL() };
    if (!url.isValid() || url.isEmpty())
        return CanDiscardResult::Protected; // kInvalidURL
    if (page.isCapturingAudio() || page.isCapturingVideo())
        return CanDiscardResult::Protected; // kCapturingAudio / kCapturingVideo
    if (RefPtr inspector = page.inspector(); inspector && inspector->isConnected())
        return CanDiscardResult::Protected; // kDevToolsOpen
    // kFormInteractions / kHadUserEdits are checked by WebPageProxy::discardForUrgentMemoryPressure, which refuses
    // such pages so the next candidate is tried.
    return CanDiscardResult::Eligible;
}

void ChromiumPageDiscarding::handleMemoryPressureEvent()
{
    // PageDiscardingHelper::DiscardAPage(URGENT): protected pages are skipped (discard_protected_tabs = false); of
    // the rest, the least important goes first, and among hidden unprotected pages that is the one whose visibility
    // changed longest ago (PageNodeSortProxy::operator<).
    Vector<Ref<WebPageProxy>> candidates;
    for (Ref pool : WebProcessPool::allProcessPools()) {
        for (Ref process : pool->processes()) {
            for (Ref page : process->pages()) {
                if (canDiscard(page) == CanDiscardResult::Eligible)
                    candidates.append(WTF::move(page));
            }
        }
    }
    std::ranges::sort(candidates, [](auto& a, auto& b) {
        return a->lastVisibilityChangeTimeForDiscarding() < b->lastVisibilityChangeTimeForDiscarding();
    });
    for (auto& candidate : candidates) {
        if (candidate->discardForUrgentMemoryPressure()) {
            RELEASE_LOG(MemoryPressure, "ChromiumPageDiscarding: critical memory pressure, discarded page %" PRIu64, candidate->identifier().toUInt64());
            return;
        }
    }
    RELEASE_LOG(MemoryPressure, "ChromiumPageDiscarding: critical memory pressure, no discardable page");
}

} // namespace WebKit

#endif // PLATFORM(MAC)
