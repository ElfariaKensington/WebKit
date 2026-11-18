/*
 * Subresource load scheduling ported from Chromium (Copyright The Chromium Authors; BSD-3-Clause),
 * chromium/chromium@25cdf5d33309850b64974c2f9fc617fe925746e7:
 *   third_party/blink/renderer/platform/loader/fetch/resource_load_scheduler.{h,cc}   (renderer side)
 *   third_party/blink/renderer/platform/loader/fetch/resource_loader.cc               (throttle options)
 *   third_party/blink/renderer/core/dom/document.cc, core/loader/frame_fetch_context.cc (tight until <body>)
 *   services/network/resource_scheduler/resource_scheduler.{h,cc}                      (network side)
 *   services/network/resource_scheduler/resource_scheduler_params_manager.cc          (4G parameters)
 */

#pragma once

#if PLATFORM(MAC)

#include <WebCore/FrameIdentifier.h>
#include <WebCore/ResourceLoadPriority.h>
#include <WebCore/ResourceLoaderIdentifier.h>
#include <wtf/FastMalloc.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/URL.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class CachedResource;
class ResourceLoader;
}

namespace WebKit {

class ChromiumResourceScheduler {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ChromiumResourceScheduler);
    WTF_MAKE_NONCOPYABLE(ChromiumResourceScheduler);
public:
    ChromiumResourceScheduler();

    // Returns true if the load can be sent to the network process now. Otherwise the scheduler keeps the loader
    // and calls `start` once Blink's renderer scheduler and the network ResourceScheduler both allow it.
    bool schedule(WebCore::ResourceLoader&, WebCore::CachedResource*, Function<void()>&& start);
    // The loader finished, failed or was cancelled (ResourceLoadScheduler::Release / request destruction).
    void remove(WebCore::ResourceLoader&);
    // ResourceFetcher::UpdateAllImageResourcePriorities: resources visible in the viewport are raised to kHigh.
    void boostToVisiblePriority(WebCore::ResourceLoader&);
    // HttpServerProperties::SetSupportsSpdy / QUIC: learned from the protocol each response was received over.
    void didReceiveProtocol(const URL&, const String& protocol);

    // The priority the load is scheduled with, including any visibility boost.
    WebCore::ResourceLoadPriority scheduledPriority(const WebCore::ResourceLoader&) const;

private:
    enum class Stage : uint8_t { RendererPending, NetworkPending, InFlight };
    struct Request {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Request);
        Ref<WebCore::ResourceLoader> loader;
        WebCore::FrameIdentifier client;
        WebCore::ResourceLoadPriority priority;
        uint64_t order;
        bool throttleable; // ResourceLoadScheduler::ThrottleOption::kThrottleable
        bool isHTTPFamily;
        String schemeHostPort;
        Stage stage { Stage::RendererPending };
        bool rendererRunning { false }; // In ResourceLoadScheduler::running_requests_.
        MonotonicTime networkQueueTime; // url_request.creation_time()
        Function<void()> start;
    };
    struct Client {
        bool tightPolicy { true }; // ResourceLoadScheduler::ThrottlingPolicy::kTight
    };

    void maybeRun(WebCore::FrameIdentifier);
    void maybeLoosenThrottlingPolicy(WebCore::FrameIdentifier, Client&);
    bool runRendererPendingRequests(WebCore::FrameIdentifier, Client&);
    void loadAnyStartablePendingRequests(WebCore::FrameIdentifier);
    enum class ShouldStart : uint8_t { StartRequest, DoNotStartAndKeepSearching, DoNotStartAndStopSearching };
    ShouldStart shouldStartRequest(const Request&, WebCore::FrameIdentifier, MonotonicTime now) const;
    Vector<Request*> sortedRequests(WebCore::FrameIdentifier, Stage) const;
    void scheduleLongQueuedRequestsTimer();
    void longQueuedRequestsTimerFired();

    HashMap<WebCore::ResourceLoaderIdentifier, std::unique_ptr<Request>> m_requests;
    HashMap<WebCore::FrameIdentifier, Client> m_clients;
    HashSet<String> m_serversSupportingPriority;
    uint64_t m_nextOrder { 0 };
    RunLoop::Timer m_longQueuedRequestsTimer;
};

} // namespace WebKit

#endif // PLATFORM(MAC)
