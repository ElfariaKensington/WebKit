/*
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2026 Squarespace, Inc. www.squarespace.com
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

#include "config.h"
#include "LazyLoadElementObserver.h"

#include "HTMLIFrameElement.h"
#include "HTMLImageElement.h"
#include "IntersectionObserverCallback.h"
#include "IntersectionObserverEntry.h"
#include "LocalFrame.h"
#include "NodeDocument.h"
#include <wtf/TZoneMallocInlines.h>

#if ENABLE(VIDEO)
#include "HTMLMediaElement.h"
#endif

#if ENABLE(MODEL_ELEMENT)
#include "HTMLModelElement.h"
#endif

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(LazyLoadElementObserver);

class LazyLoadIntersectionObserverCallback final : public IntersectionObserverCallback {
public:
    static Ref<LazyLoadIntersectionObserverCallback> create(Document& document)
    {
        return adoptRef(*new LazyLoadIntersectionObserverCallback(document));
    }

private:
    LazyLoadIntersectionObserverCallback(Document& document)
        : IntersectionObserverCallback(&document)
    {
    }

    NODELETE bool hasCallback() const final { return true; }

    CallbackResult<void> invoke(IntersectionObserver&, const Vector<Ref<IntersectionObserverEntry>>& entries, IntersectionObserver&) final
    {
        ASSERT(!entries.isEmpty());

        for (auto& entry : entries) {
            if (RefPtr element = dynamicDowncast<HTMLImageElement>(entry->target()))
                element->lazyLoadIntersectionCallbackInvoked(entry->isIntersecting());
            else if (RefPtr element = dynamicDowncast<HTMLIFrameElement>(entry->target()))
                element->lazyLoadIntersectionCallbackInvoked(entry->isIntersecting());
#if ENABLE(VIDEO)
            else if (RefPtr element = dynamicDowncast<HTMLMediaElement>(entry->target()))
                element->lazyLoadIntersectionCallbackInvoked(entry->isIntersecting());
#endif
#if ENABLE(MODEL_ELEMENT)
            else if (RefPtr element = dynamicDowncast<HTMLModelElement>(entry->target()))
                element->lazyLoadIntersectionCallbackInvoked(entry->isIntersecting());
#endif
        }
        return { };
    }

    CallbackResult<void> invokeRethrowingException(IntersectionObserver& thisObserver, const Vector<Ref<IntersectionObserverEntry>>& entries, IntersectionObserver& observer) final
    {
        return invoke(thisObserver, entries, observer);
    }
};

LazyLoadElementObserver::LazyLoadElementObserver() = default;
LazyLoadElementObserver::~LazyLoadElementObserver() = default;

void LazyLoadElementObserver::observe(Element& element)
{
    Ref document = element.document();
    CheckedRef<LazyLoadElementObserver> observer = document->lazyLoadElementObserver();
    RefPtr intersectionObserver = observer->intersectionObserver(document, kindForElement(element));
    if (!intersectionObserver)
        return;
    intersectionObserver->observe(element);
}

LazyLoadElementObserver::Kind LazyLoadElementObserver::kindForElement(const Element& element)
{
#if PLATFORM(MAC)
    if (is<HTMLImageElement>(element))
        return Kind::Image;
    if (is<HTMLIFrameElement>(element))
        return Kind::Frame;
#else
    UNUSED_PARAM(element);
#endif
    return Kind::Other;
}

void LazyLoadElementObserver::unobserve(Element& element, Document& document)
{
    CheckedPtr lazyLoadObserver = document.lazyLoadElementObserverIfExists();
    if (!lazyLoadObserver)
        return;
    if (RefPtr observer = lazyLoadObserver->m_observer)
        observer->unobserve(element);
    if (RefPtr observer = lazyLoadObserver->m_imageObserver)
        observer->unobserve(element);
    if (RefPtr observer = lazyLoadObserver->m_frameObserver)
        observer->unobserve(element);
}

void LazyLoadElementObserver::unobserveIfLazyLoadOnly(Element& element, Document& document)
{
#if ENABLE(VIDEO)
    // A <video> stays observed for its whole lifetime to drive accelerated rendering teardown
    // (311380@main), so a finished poster load must not detach it.
    if (RefPtr mediaElement = dynamicDowncast<HTMLMediaElement>(element); mediaElement && mediaElement->tracksViewportIntersection())
        return;
#endif
    unobserve(element, document);
}

IntersectionObserver* LazyLoadElementObserver::intersectionObserver(Document& document, Kind kind)
{
    auto createObserver = [&](const String& scrollMargin) -> RefPtr<IntersectionObserver> {
        auto callback = LazyLoadIntersectionObserverCallback::create(document);
        IntersectionObserver::Init options { std::nullopt, { }, scrollMargin, { } };
        auto observer = IntersectionObserver::create(document, WTF::move(callback), WTF::move(options));
        if (observer.hasException())
            return nullptr;
        return observer.releaseReturnValue();
    };

    switch (kind) {
    case Kind::Image:
        if (!m_imageObserver) {
            // Blink settings.json5 lazyLoadingImageMarginPx4G (chromium/chromium@25cdf5d33309850b64974c2f9fc617fe925746e7).
            static NeverDestroyed<const String> imageScrollMargin(MAKE_STATIC_STRING_IMPL("1250px"));
            RefPtr observer = createObserver(imageScrollMargin);
            if (!observer)
                return nullptr;
            lazyInitialize(m_imageObserver, observer.releaseNonNull());
        }
        return m_imageObserver.get();
    case Kind::Frame:
        if (!m_frameObserver) {
            // Blink settings.json5 lazyLoadingFrameMarginPx4G, used as LazyLoadFrameObserver's scroll_margin.
            static NeverDestroyed<const String> frameScrollMargin(MAKE_STATIC_STRING_IMPL("2500px"));
            RefPtr observer = createObserver(frameScrollMargin);
            if (!observer)
                return nullptr;
            lazyInitialize(m_frameObserver, observer.releaseNonNull());
        }
        return m_frameObserver.get();
    case Kind::Other:
        break;
    }

    if (!m_observer) {
        static NeverDestroyed<const String> lazyLoadingScrollMarginFallback(MAKE_STATIC_STRING_IMPL("100%"));
        RefPtr observer = createObserver(lazyLoadingScrollMarginFallback);
        if (!observer)
            return nullptr;
        lazyInitialize(m_observer, observer.releaseNonNull());
    }
    return m_observer.get();
}

}
