/*
 * Subresource load scheduling ported from Chromium. See ChromiumResourceScheduler.h for the upstream files,
 * revision and license.
 */

#include "config.h"
#include "ChromiumResourceScheduler.h"

#if PLATFORM(MAC)

#include <WebCore/CachedResource.h>
#include <WebCore/Document.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/HTMLBodyElement.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/ResourceLoader.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
using namespace WebCore;

// resource_load_scheduler.cc
static constexpr size_t tightLimitForRendererSideResourceScheduler = 2;
static constexpr size_t tightMediumLimitForRendererSideResourceScheduler = 2;
static constexpr size_t limitForRendererSideResourceScheduler = 1024;
// resource_scheduler.cc / resource_scheduler_params_manager.cc, parameters for EFFECTIVE_CONNECTION_TYPE_4G:
// kDefaultMaxNumDelayableRequestsPerClient, non_delayable_weight 0, delay_requests_on_multiplexed_connections false,
// max_queuing_time = clamp(30 * typical 4G HTTP RTT, 15 s, 120 s) = 15 s.
static constexpr size_t maxNumDelayableRequestsPerClient = 10;
static constexpr size_t maxNumDelayableRequestsPerHostPerClient = 6;
static constexpr Seconds maxQueuingTime = 15_s;
static constexpr Seconds queuedRequestsDispatchPeriodicity = 100_ms; // queued_requests_dispatch_periodicity_ms

// net::RequestPriority below MEDIUM (kDelayablePriorityThreshold). Blink maps kHigh to net::MEDIUM, so every
// priority under High is delayable.
static bool isDelayable(ResourceLoadPriority priority)
{
    return priority < ResourceLoadPriority::High;
}

ChromiumResourceScheduler::ChromiumResourceScheduler()
    : m_longQueuedRequestsTimer(RunLoop::mainSingleton(), "ChromiumResourceScheduler::LongQueuedRequestsTimer"_s, [this] { longQueuedRequestsTimerFired(); })
{
}

static String schemeHostPort(const URL& url)
{
    return makeString(url.protocol(), "://"_s, url.host(), ':', url.port().value_or(defaultPortForProtocol(url.protocol()).value_or(0)));
}

bool ChromiumResourceScheduler::schedule(ResourceLoader& loader, CachedResource* resource, Function<void()>&& start)
{
    auto identifier = loader.identifier();
    RefPtr frameLoader = loader.frameLoader();
    if (!identifier || !frameLoader)
        return true;

    const URL& url = loader.request().url();
    bool isHTTPFamily = url.protocolIsInHTTPFamily();
    // ResourceLoader::Start: synchronous, keepalive and non-HTTP(S) requests can not be stopped or throttled, and
    // "aka long-polling" request contexts (fetch, XHR, EventSource, media) are only stoppable. Main resources are
    // browser-initiated in Chromium and bypass both schedulers.
    if (!resource || resource->type() == CachedResource::Type::MainResource)
        return true;
    bool throttleable = isHTTPFamily && !loader.options().keepAlive;
    switch (resource->type()) {
    case CachedResource::Type::RawResource:
    case CachedResource::Type::MediaResource:
    case CachedResource::Type::Beacon:
    case CachedResource::Type::Ping:
        throttleable = false;
        break;
    default:
        break;
    }

    auto client = frameLoader->frameID();
    auto request = makeUnique<Request>(Request {
        loader,
        client,
        loader.request().priority(),
        m_nextOrder++,
        throttleable,
        isHTTPFamily,
        schemeHostPort(url),
        Stage::RendererPending,
        false,
        { },
        WTF::move(start),
    });
    if (!throttleable) {
        // ResourceLoadScheduler::Request: !IsClientDelayable(option) -> Run() immediately.
        request->rendererRunning = true;
        request->stage = Stage::NetworkPending;
        request->networkQueueTime = MonotonicTime::now();
    }
    m_clients.ensure(client, [] { return Client { }; });
    m_requests.set(*identifier, WTF::move(request));

    maybeRun(client);

    // If maybeRun() started this request synchronously, its start function was already called; report that the
    // caller does not need to start it.
    return false;
}

void ChromiumResourceScheduler::remove(ResourceLoader& loader)
{
    auto identifier = loader.identifier();
    if (!identifier)
        return;
    auto request = m_requests.take(*identifier);
    if (!request)
        return;
    auto client = request->client;
    bool hasOtherRequests = std::ranges::any_of(m_requests.values(), [&](auto& other) { return other->client == client; });
    if (!hasOtherRequests) {
        m_clients.remove(client);
        return;
    }
    // ResourceLoadScheduler::Release(kReleaseAndSchedule) and ResourceScheduler::RemoveRequest.
    maybeRun(client);
}

ResourceLoadPriority ChromiumResourceScheduler::scheduledPriority(const ResourceLoader& loader) const
{
    if (auto identifier = loader.identifier()) {
        if (auto it = m_requests.find(*identifier); it != m_requests.end())
            return it->value->priority;
    }
    return loader.request().priority();
}

void ChromiumResourceScheduler::boostToVisiblePriority(ResourceLoader& loader)
{
    auto identifier = loader.identifier();
    if (!identifier)
        return;
    auto it = m_requests.find(*identifier);
    if (it == m_requests.end() || it->value->stage == Stage::InFlight)
        return;
    // ComputeLoadPriority: "Visible resources (images in practice) get a boost to High priority."
    if (it->value->priority >= ResourceLoadPriority::High)
        return;
    it->value->priority = ResourceLoadPriority::High;
    // ResourceLoadScheduler::SetPriority / ResourceScheduler::ReprioritizeRequest.
    maybeRun(it->value->client);
}

void ChromiumResourceScheduler::didReceiveProtocol(const URL& url, const String& protocol)
{
    // HttpServerProperties::SupportsRequestPriority: true for servers known to speak HTTP/2 or QUIC.
    if (!url.protocolIsInHTTPFamily())
        return;
    if (protocol == "h2"_s || protocol == "h3"_s || protocol.startsWith("h3-"_s) || protocol == "quic"_s)
        m_serversSupportingPriority.add(schemeHostPort(url));
}

Vector<ChromiumResourceScheduler::Request*> ChromiumResourceScheduler::sortedRequests(FrameIdentifier client, Stage stage) const
{
    Vector<Request*> requests;
    for (auto& request : m_requests.values()) {
        if (request->client == client && request->stage == stage)
            requests.append(request.get());
    }
    // ClientIdWithPriority::Compare / RequestQueue: higher priority first, then FIFO.
    std::ranges::sort(requests, [](auto* a, auto* b) {
        if (a->priority != b->priority)
            return a->priority > b->priority;
        return a->order < b->order;
    });
    return requests;
}

void ChromiumResourceScheduler::maybeLoosenThrottlingPolicy(FrameIdentifier client, Client& clientState)
{
    if (!clientState.tightPolicy)
        return;
    // Document::WillInsertBody -> ResourceFetcher::LoosenLoadThrottlingPolicy. Documents that never get a <body>
    // (XML, SVG, image documents) are loosened once parsing ends so their low-priority loads can not stall.
    for (auto& request : m_requests.values()) {
        if (request->client != client)
            continue;
        RefPtr frame = request->loader->frame();
        RefPtr document = frame ? frame->document() : nullptr;
        if (document && (document->bodyOrFrameset() || !document->parsing()))
            clientState.tightPolicy = false;
        return;
    }
}

bool ChromiumResourceScheduler::runRendererPendingRequests(FrameIdentifier client, Client& clientState)
{
    size_t runningThrottleable = 0;
    size_t runningMedium = 0;
    for (auto& request : m_requests.values()) {
        if (request->client != client || !request->rendererRunning)
            continue;
        if (request->throttleable)
            runningThrottleable++;
        if (request->priority == ResourceLoadPriority::Medium)
            runningMedium++;
    }

    bool releasedAny = false;
    // ResourceLoadScheduler::MaybeRun / GetNextPendingRequest: only the head of the priority queue is considered.
    for (auto* request : sortedRequests(client, Stage::RendererPending)) {
        // GetOutstandingLimit (kNotThrottled frame).
        size_t limit = clientState.tightPolicy && request->priority < ResourceLoadPriority::High ? tightLimitForRendererSideResourceScheduler : limitForRendererSideResourceScheduler;
        // IsRunningThrottleableRequestsLessThanOutStandingLimit.
        bool canRun = (request->priority == ResourceLoadPriority::Medium && runningMedium < tightMediumLimitForRendererSideResourceScheduler) || runningThrottleable < limit;
        if (!canRun)
            break;
        request->rendererRunning = true;
        request->stage = Stage::NetworkPending;
        request->networkQueueTime = MonotonicTime::now();
        runningThrottleable++;
        if (request->priority == ResourceLoadPriority::Medium)
            runningMedium++;
        releasedAny = true;
    }
    return releasedAny;
}

ChromiumResourceScheduler::ShouldStart ChromiumResourceScheduler::shouldStartRequest(const Request& request, FrameIdentifier client, MonotonicTime now) const
{
    // Non-HTTP[S] requests.
    if (!request.isHTTPFamily)
        return ShouldStart::StartRequest;
    if (now - request.networkQueueTime >= maxQueuingTime)
        return ShouldStart::StartRequest;
    // Requests on a connection that supports prioritization and multiplexing.
    bool supportsPriority = m_serversSupportingPriority.contains(request.schemeHostPort);
    if (supportsPriority)
        return ShouldStart::StartRequest;
    // Non-delayable requests.
    if (!isDelayable(request.priority))
        return ShouldStart::StartRequest;

    size_t inFlightDelayable = 0;
    size_t sameHostCount = 0;
    for (auto& other : m_requests.values()) {
        if (other->client != client || other->stage != Stage::InFlight)
            continue;
        if (isDelayable(other->priority))
            inFlightDelayable++;
        if (other->schemeHostPort == request.schemeHostPort)
            sameHostCount++;
    }
    // Delayable requests per client limit (10); non_delayable_weight is 0 on 4G.
    if (inFlightDelayable >= maxNumDelayableRequestsPerClient)
        return ShouldStart::DoNotStartAndStopSearching;
    // Delayable requests per host limit (6).
    if (sameHostCount >= maxNumDelayableRequestsPerHostPerClient)
        return ShouldStart::DoNotStartAndKeepSearching;
    return ShouldStart::StartRequest;
}

void ChromiumResourceScheduler::loadAnyStartablePendingRequests(FrameIdentifier client)
{
    auto now = MonotonicTime::now();
    Vector<ResourceLoaderIdentifier> toStart;
    for (auto* request : sortedRequests(client, Stage::NetworkPending)) {
        auto result = shouldStartRequest(*request, client, now);
        if (result == ShouldStart::StartRequest) {
            request->stage = Stage::InFlight;
            toStart.append(*request->loader->identifier());
            continue;
        }
        if (result == ShouldStart::DoNotStartAndKeepSearching)
            continue;
        break;
    }
    // Start after the scan: a start function may synchronously fail the load and re-enter remove().
    for (auto identifier : toStart) {
        auto it = m_requests.find(identifier);
        if (it == m_requests.end())
            continue;
        if (auto start = std::exchange(it->value->start, nullptr))
            start();
    }
}

void ChromiumResourceScheduler::maybeRun(FrameIdentifier client)
{
    auto it = m_clients.find(client);
    if (it == m_clients.end())
        return;
    maybeLoosenThrottlingPolicy(client, it->value);
    runRendererPendingRequests(client, it->value);
    loadAnyStartablePendingRequests(client);
    scheduleLongQueuedRequestsTimer();
}

void ChromiumResourceScheduler::scheduleLongQueuedRequestsTimer()
{
    // "If there are no pending requests, then do not start the timer."
    bool hasPendingRequest = std::ranges::any_of(m_requests.values(), [](auto& request) {
        return request->stage != Stage::InFlight;
    });
    if (!hasPendingRequest) {
        m_longQueuedRequestsTimer.stop();
        return;
    }
    if (!m_longQueuedRequestsTimer.isActive())
        m_longQueuedRequestsTimer.startOneShot(queuedRequestsDispatchPeriodicity);
}

void ChromiumResourceScheduler::longQueuedRequestsTimerFired()
{
    HashSet<FrameIdentifier> clients;
    for (auto& request : m_requests.values()) {
        if (request->stage != Stage::InFlight)
            clients.add(request->client);
    }
    for (auto client : clients)
        maybeRun(client);
    scheduleLongQueuedRequestsTimer();
}

} // namespace WebKit

#endif // PLATFORM(MAC)
