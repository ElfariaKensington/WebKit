/*
 * Urgent page discarding ported from Chromium's performance_manager (Copyright The Chromium Authors;
 * BSD-3-Clause), chromium/chromium@25cdf5d33309850b64974c2f9fc617fe925746e7:
 *   chrome/browser/performance_manager/policies/urgent_page_discarding_policy.cc
 *   chrome/browser/performance_manager/policies/page_discarding_helper.cc
 *   chrome/browser/performance_manager/policies/discard_eligibility_policy.{h,cc}
 */

#pragma once

#if PLATFORM(MAC)

#include <wtf/OSObjectPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>

namespace WebKit {

class WebPageProxy;

// Under critical system memory pressure, discards the least important hidden page (terminates its WebContent
// process; the page reloads from session history when shown again), then keeps discarding one page every 5 s
// while the pressure lasts.
class ChromiumPageDiscarding {
    WTF_MAKE_NONCOPYABLE(ChromiumPageDiscarding);
public:
    static ChromiumPageDiscarding& singleton();
    void initializeIfNeeded();

    static constexpr Seconds nonVisiblePagesUrgentProtectionTime { 10_min }; // kNonVisiblePagesUrgentProtectionTime
    static constexpr Seconds tabAudioProtectionTime { 1_min }; // kTabAudioProtectionTime
    static constexpr Seconds sustainedMemoryPressureInterval { 5_s }; // sustained_memory_pressure_timer_

private:
    friend class NeverDestroyed<ChromiumPageDiscarding>;
    ChromiumPageDiscarding();

    enum class CanDiscardResult : uint8_t { Eligible, Protected, Disallowed };
    static CanDiscardResult canDiscard(WebPageProxy&);
    void handleMemoryPressureEvent(); // UrgentPageDiscardingPolicy::HandleMemoryPressureEvent -> DiscardAPage
    void memoryPressureLevelChanged(bool isCritical);

    OSObjectPtr<dispatch_source_t> m_memoryPressureSource;
    RunLoop::Timer m_sustainedMemoryPressureTimer;
    bool m_initialized { false };
};

} // namespace WebKit

#endif // PLATFORM(MAC)
