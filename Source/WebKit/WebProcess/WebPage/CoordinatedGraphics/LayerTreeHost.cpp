/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Company 100, Inc.
 * Copyright (C) 2014-2019 Igalia S.L.
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
#include "LayerTreeHost.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedSceneState.h"
#include "DrawingArea.h"
#include "RenderProcessInfo.h"
#include "WebPageInlines.h"
#include "WebPageProxyMessages.h"
#include "WebProcess.h"
#include <WebCore/AsyncScrollingCoordinator.h>
#include <WebCore/Chrome.h>
#include <WebCore/CoordinatedTileManager.h>
#include <WebCore/Damage.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PageOverlayController.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/RenderLayerBacking.h>
#include <WebCore/RenderView.h>
#include <WebCore/ScrollingThread.h>
#include <WebCore/Settings.h>
#include <WebCore/ThreadedScrollingTree.h>
#include <WebCore/WindowEventLoop.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/threads/BinarySemaphore.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LayerTreeHost);

#if ENABLE(DAMAGE_TRACKING)
static bool damageOverlayForcesPropagation()
{
    // WEBKIT_SHOW_DAMAGE draws the damage the layers report, so it needs propagation on even when the
    // propagateDamagingInformation setting that normally turns it on is off.
    const auto* showDamageEnvvar = getenv("WEBKIT_SHOW_DAMAGE");
    if (!showDamageEnvvar)
        return false;

    auto value = parseInteger<unsigned>(StringView::fromLatin1(showDamageEnvvar));
    return value && *value;
}
#endif

std::unique_ptr<LayerTreeHost> LayerTreeHost::create(WebPage& webPage)
{
    return makeUnique<LayerTreeHost>(webPage);
}

LayerTreeHost::LayerTreeHost(WebPage& webPage)
    : m_webPage(webPage)
    , m_sceneState(CoordinatedSceneState::create())
{
    {
        auto& rootLayer = m_sceneState->rootLayer();
#if ENABLE(DAMAGE_TRACKING)
        const auto& settings = webPage.corePage()->settings();
        const bool propagateDamage = settings.propagateDamagingInformation() || damageOverlayForcesPropagation();
        rootLayer.setDamagePropagationEnabled(propagateDamage);
        if (propagateDamage) {
            m_damageInGlobalCoordinateSpace = std::make_shared<Damage>(m_webPage->size());
            rootLayer.setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
        }
#endif
        Locker locker { rootLayer.lock() };
        rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
        rootLayer.setSize(m_webPage->size());
    }

    lazyInitialize(m_compositor, ThreadedCompositor::create(webPage, *this, m_sceneState.get()));
    m_tileManager = CoordinatedTileManager::create(m_compositor->threadSafeGrContext());

#if ENABLE(DAMAGE_TRACKING)
    std::optional<OptionSet<ThreadedCompositor::DamagePropagationFlags>> damagePropagationFlags;
    const auto& settings = webPage.corePage()->settings();
    if (settings.propagateDamagingInformation()) {
        damagePropagationFlags = OptionSet<ThreadedCompositor::DamagePropagationFlags> { };
        if (settings.unifyDamagedRegions())
            damagePropagationFlags->add(ThreadedCompositor::DamagePropagationFlags::Unified);
        if (settings.useDamagingInformationForCompositing())
            damagePropagationFlags->add(ThreadedCompositor::DamagePropagationFlags::UseForCompositing);
    }
    m_compositor->setDamagePropagationSettings(damagePropagationFlags, settings.damageRectangleThreshold());
#endif
}

LayerTreeHost::~LayerTreeHost()
{
    m_tileManager = nullptr;

    // ThreadedCompositor must be invalidated before invalidating CoordinatedSceneState
    // to invalidate pending layers in the compositor thread.
    m_compositor->invalidate();
    m_sceneState->invalidate();
}

uint64_t LayerTreeHost::surfaceID() const
{
    return m_compositor->surfaceID();
}

void LayerTreeHost::scheduleRenderingUpdate()
{
    WTFEmitSignpost(this, LayerTreeHostScheduleRenderingUpdate, "isWaitingForRenderer %s", m_isWaitingForRenderer ? "yes" : "no");

    if (m_layerTreeStateIsFrozen || m_isSuspended || m_webPage->size().isEmpty())
        return;

    if (!canUpdateRendering()) {
        m_scheduledWhileWaitingForRenderer = true;
        return;
    }

    scheduleRenderingUpdateRunLoopObserver();
}

void LayerTreeHost::scheduleRenderingUpdateRunLoopObserver()
{
    FrameRenderer::scheduleRenderingUpdateRunLoopObserver();

    // Avoid running any more tasks before the runloop observer fires.
    WindowEventLoop::breakToAllowRenderingUpdate();
}

bool LayerTreeHost::canUpdateRendering() const
{
    // While composition is suppressed no frame is going to be composited, so rendering updates must not
    // wait for one. A frame requested before the suppression started stays parked in the suspended
    // compositor, with m_isWaitingForRenderer set, until the suppression ends.
    return !m_isWaitingForRenderer || m_isCompositionSuppressed;
}

void LayerTreeHost::updateRendering()
{
    invalidateRenderingUpdateRunLoopObserver();

    RELEASE_ASSERT(!m_isUpdatingRendering);
    if (m_layerTreeStateIsFrozen)
        return;

    SetForScope<bool> reentrancyProtector(m_isUpdatingRendering, true);

    TraceScope traceScope(LayerTreeHostRenderingUpdateStart, LayerTreeHostRenderingUpdateEnd);

    m_renderingUpdateID++;

    m_tileManager->didStartRenderingUpdate(m_renderingUpdateID);

    Ref page = m_webPage;
    page->updateRendering();
    page->flushPendingEditorStateUpdate();
    page->flushPendingThemeColorChange();

    if (m_overlayCompositingLayer)
        m_overlayCompositingLayer->flushCompositingState(visibleContentsRect());

    OptionSet<FinalizeRenderingUpdateFlags> flags;
#if PLATFORM(GTK)
    if (!m_transientZoom)
        flags.add(FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions);
#else
    flags.add(FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions);
#endif
    page->finalizeRenderingUpdate(flags);

    if (m_pendingResize) {
        m_compositor->setSize(page->size(), page->deviceScaleFactor());
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setSize(page->size());
    }

#if PLATFORM(GTK)
    // If we have an active transient zoom, we want the zoom to win over any changes
    // that WebCore makes to the relevant layers, so re-apply our changes after flushing.
    if (m_transientZoom)
        applyTransientZoomToLayers(m_transientZoomScale, m_transientZoomOrigin);
#endif

    if (RefPtr drawingArea = page->drawingArea())
        drawingArea->dispatchPendingCallbacksAfterEnsuringDrawing();

    bool didChangeSceneState = m_sceneState->flush();
    m_tileManager->didFinishRenderingUpdate();
    if (m_isCompositionSuppressed) {
        // No frame follows this rendering update. The frame that is owed needs no bookkeeping here: what it
        // is made of outlives the suppression (the flushed scene state, the compositor size, the dirty
        // regions kept by the layers) and setCompositionSuppressed(false) always requires a composition.

        // A forced repaint has no frame to wait for, so it is complete now.
        if (m_forcedRepaintAsyncCallback)
            m_forcedRepaintAsyncCallback();

        // Removed layers are only released by the next composition; don't let them pile up until then.
        if (didChangeSceneState) {
            if (RunLoop* runLoop = m_compositor->runLoop()) {
                runLoop->dispatch([sceneState = m_sceneState] {
                    sceneState->invalidateRemovedLayers();
                });
            }
        }
    } else if (m_compositionRequired || m_pendingResize || m_forceFrameSync || didChangeSceneState)
        requestCompositionForRenderingUpdate();

    m_compositionRequired = false;
    m_pendingResize = false;
    m_forceFrameSync = false;

    page->didUpdateRendering();

    // Eject any backing stores whose only reference is held in the HashMap cache.
    m_imageBackingStores.removeIf([](auto& it) {
        return it.value->hasOneRef();
    });

    if (m_waitUntilPaintingFinished) {
        m_sceneState->waitUntilPaintingFinished();
        m_waitUntilPaintingFinished = false;
    }
}

void LayerTreeHost::updateRootLayer()
{
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (m_rootCompositingLayer) {
        children.append(downcast<GraphicsLayerCoordinated>(m_rootCompositingLayer)->coordinatedPlatformLayer());
        if (m_overlayCompositingLayer)
            children.append(downcast<GraphicsLayerCoordinated>(m_overlayCompositingLayer)->coordinatedPlatformLayer());
    }

    m_sceneState->setRootLayerChildren(WTF::move(children));
}

void LayerTreeHost::setRootCompositingLayer(GraphicsLayer* graphicsLayer)
{
    if (m_rootCompositingLayer == graphicsLayer)
        return;

    m_rootCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::setViewOverlayRootLayer(GraphicsLayer* graphicsLayer)
{
    if (m_overlayCompositingLayer == graphicsLayer)
        return;

    m_overlayCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::updateRenderingWithForcedRepaint()
{
    if (m_webPage->size().isEmpty()) {
        m_pendingForceRepaint = false;
        if (m_forcedRepaintAsyncCallback)
            m_forcedRepaintAsyncCallback();
        return;
    }

    if (!canUpdateRendering()) {
        if (m_forcedRepaintAsyncCallback)
            m_pendingForceRepaint = true;
        return;
    }

    m_pendingForceRepaint = false;
    protect(m_webPage)->corePage()->forceRepaintAllFrames();
    m_forceFrameSync = true;

    // Make sure `m_sceneState->waitUntilPaintingFinished()` is invoked at the
    // end of the currently running layer flush, or after the next one if there
    // is none ongoing at present.
    m_waitUntilPaintingFinished = true;

    // If updateRenderingWithForcedRepaint() is invoked via JS through e.g. a rAF() callback, a call
    // to `page->updateRendering()` _during_ a layer flush is responsible for that.
    // If m_isUpdatingRendering is true, that layer flush is still ongoing, so we do
    // not need to cancel pending ones and immediately flush again (re-entrancy!).
    if (m_isUpdatingRendering)
        return;
    updateRendering();
}

bool LayerTreeHost::ensureDrawing()
{
    if (m_layerTreeStateIsFrozen || m_isSuspended || m_webPage->size().isEmpty())
        return false;

    m_forceFrameSync = true;
    scheduleRenderingUpdate();
    return true;
}

void LayerTreeHost::sizeDidChange()
{
    m_pendingResize = true;
    if (m_isWaitingForRenderer)
        scheduleRenderingUpdate();
    else
        updateRendering();
}

void LayerTreeHost::suspend()
{
    FrameRenderer::suspend();
    m_compositor->suspend();
}

void LayerTreeHost::resume()
{
    m_compositor->resume();
    FrameRenderer::resume();
}

void LayerTreeHost::setCompositionSuppressed(bool suppressed)
{
    if (m_isCompositionSuppressed == suppressed)
        return;

    m_isCompositionSuppressed = suppressed;
    if (suppressed) {
        // The compositor's own suspension is what suppression needs: it stops rendering, including the
        // frames the compositor schedules for itself (accelerated animations), and it keeps every
        // composition requested meanwhile, from any thread, until it is resumed.
        m_compositor->suspend();

        // Rendering updates stop waiting for the renderer now, but they must still not run while it flushes
        // the scene for a frame. One that is being rendered at this moment has to finish first; later ones
        // find the compositor suspended.
        if (RunLoop* runLoop = m_compositor->runLoop()) {
            BinarySemaphore semaphore;
            runLoop->dispatch([&semaphore] {
                semaphore.signal();
            });
            semaphore.wait();
        }

        // This forced repaint was waiting for the frame in flight, which may now be parked.
        if (m_pendingForceRepaint)
            updateRenderingWithForcedRepaint();
    } else {
        m_compositor->resume();

        // Nothing was painted or composited while suppressed, so a frame is owed even if the page is static.
        m_compositionRequired = true;
    }

    // When the suppression starts this runs the update that was waiting for the frame in flight. When it
    // ends this either updates right away or, if that frame is still pending, right after it.
    scheduleRenderingUpdate();
}

GraphicsLayerFactory* LayerTreeHost::graphicsLayerFactory()
{
    return this;
}

FloatRect LayerTreeHost::visibleContentsRect() const
{
    Ref webPage = m_webPage;
    if (auto* localMainFrameView = webPage->localMainFrameView())
        return FloatRect({ }, localMainFrameView->sizeForVisibleContent(ScrollableArea::VisibleContentRectIncludesScrollbars::Yes));
    return webPage->bounds();
}

void LayerTreeHost::backgroundColorDidChange()
{
    m_compositor->backgroundColorDidChange();
}

void LayerTreeHost::attachLayer(CoordinatedPlatformLayer& layer)
{
#if ENABLE(DAMAGE_TRACKING)
    layer.setDamagePropagationEnabled(!!m_damageInGlobalCoordinateSpace);
    if (m_damageInGlobalCoordinateSpace)
        layer.setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
#endif
    m_sceneState->addLayer(layer);
}

void LayerTreeHost::detachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->removeLayer(layer);
}

void LayerTreeHost::notifyCompositionRequired()
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_compositionRequiredInScrollingThread = true;
        return;
    }
#endif
    m_compositionRequired = true;
}

bool LayerTreeHost::isCompositionRequiredOrOngoing() const
{
    // No composition follows a rendering update while suppressed, and the scrolling tree would wait for it.
    if (m_isCompositionSuppressed)
        return false;

    return m_compositionRequired || m_forceFrameSync || m_compositor->isActive();
}

void LayerTreeHost::requestComposition(CompositionReason reason)
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_sceneState->flushPendingState();
        if (!m_compositionRequiredInScrollingThread)
            return;
        m_compositionRequiredInScrollingThread = false;
    }
#endif

    m_compositor->requestComposition(reason);
}

void LayerTreeHost::didChangeLayerPendingPosition(CoordinatedPlatformLayer& layer)
{
    m_sceneState->didChangeLayerPendingPosition(layer);
}

RunLoop* LayerTreeHost::compositingRunLoop() const
{
    return m_compositor->runLoop();
}

int LayerTreeHost::maxTextureSize() const
{
    return m_compositor->maxTextureSize();
}

void LayerTreeHost::willPaintTile()
{
    m_sceneState->willPaintTile();
}

void LayerTreeHost::didPaintTile()
{
    m_sceneState->didPaintTile();
    m_compositor->pendingTilesDidChange();
}

#if USE(SKIA)
void LayerTreeHost::didPaintPreRenderedTile(CoordinatedPlatformLayer& layer)
{
    if (m_sceneState->didPaintPreRenderedTile(layer) == CoordinatedSceneState::ChangedPreRenderedTiles::Yes)
        m_compositor->preRenderedTilesDidChange();
}
#endif

Ref<CoordinatedImageBackingStore> LayerTreeHost::imageBackingStore(Ref<NativeImage>&& nativeImage)
{
    auto nativeImageID = nativeImage->uniqueID();
    auto addResult = m_imageBackingStores.ensure(nativeImageID, [&] {
        return CoordinatedImageBackingStore::create(WTF::move(nativeImage), m_compositor->threadSafeGrContext());
    });
    return addResult.iterator->value;
}

Ref<GraphicsLayer> LayerTreeHost::createGraphicsLayer(GraphicsLayer::Type layerType, GraphicsLayerClient& client)
{
    return adoptRef(*new GraphicsLayerCoordinated(layerType, client, CoordinatedPlatformLayer::create(*this)));
}

void LayerTreeHost::willRenderFrame()
{
    if (RefPtr drawingArea = protect(m_webPage)->drawingArea())
        drawingArea->willStartRenderingUpdateDisplay();
}

void LayerTreeHost::didRenderFrame()
{
    Ref webPage = m_webPage;
    if (RefPtr drawingArea = webPage->drawingArea())
        drawingArea->didCompleteRenderingUpdateDisplay();
    if (auto fps = m_compositor->fps()) {
        if (RefPtr document = webPage->corePage()->localTopDocument())
            document->addConsoleMessage(MessageSource::Rendering, MessageLevel::Info, makeString("FPS: "_s, *fps));
    }
}

void LayerTreeHost::requestCompositionForRenderingUpdate()
{
    m_isWaitingForRenderer = true;
    m_compositor->requestCompositionForRenderingUpdate([this] {
        WTFBeginSignpost(this, DidComposite);

        if (!m_pendingForceRepaint && m_forcedRepaintAsyncCallback)
            m_forcedRepaintAsyncCallback();

        m_isWaitingForRenderer = false;
        bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);
        if (m_pendingForceRepaint) {
            if (!m_layerTreeStateIsFrozen)
                updateRenderingWithForcedRepaint();
            else if (m_forcedRepaintAsyncCallback)
                m_forcedRepaintAsyncCallback();
        } else if (!m_isSuspended && !m_layerTreeStateIsFrozen && scheduledWhileWaitingForRenderer)
            scheduleRenderingUpdateRunLoopObserver();

        WTFEndSignpost(this, DidComposite);
    });
    WTFEmitSignpost(this, RequestCompositionForRenderingUpdate);
}

#if PLATFORM(GTK)
FloatPoint LayerTreeHost::constrainTransientZoomOrigin(double scale, FloatPoint origin) const
{
    Ref webPage = m_webPage;
    auto* frameView = webPage->localMainFrameView();
    if (!frameView)
        return origin;

    FloatRect visibleContentRect = frameView->visibleContentRectIncludingScrollbars();

    FloatPoint constrainedOrigin = visibleContentRect.location();
    constrainedOrigin.moveBy(-origin);

    IntSize scaledTotalContentsSize = frameView->totalContentsSize();
    scaledTotalContentsSize.scale(scale * webPage->viewScaleFactor() / webPage->totalScaleFactor());

    // Scaling may have exposed the overhang area, so we need to constrain the final
    // layer position exactly like scrolling will once it's committed, to ensure that
    // scrolling doesn't make the view jump.
    constrainedOrigin = ScrollableArea::constrainScrollPositionForOverhang(roundedIntRect(visibleContentRect),
        scaledTotalContentsSize, roundedIntPoint(constrainedOrigin), frameView->scrollOrigin(),
        frameView->headerHeight(), frameView->footerHeight());
    constrainedOrigin.moveBy(-visibleContentRect.location());
    constrainedOrigin = -constrainedOrigin;

    return constrainedOrigin;
}

CoordinatedPlatformLayer* LayerTreeHost::layerForTransientZoom() const
{
    auto* frameView = protect(m_webPage)->localMainFrameView();
    if (!frameView)
        return nullptr;

    RenderLayerBacking* renderViewBacking = frameView->renderView()->layer()->backing();
    if (!renderViewBacking)
        return nullptr;

    auto* scaledLayer = renderViewBacking->contentsContainmentLayer();
    if (!scaledLayer)
        scaledLayer = renderViewBacking->graphicsLayer();
    ASSERT(scaledLayer);
    return &downcast<GraphicsLayerCoordinated>(*scaledLayer).coordinatedPlatformLayer();
}

void LayerTreeHost::applyTransientZoomToLayers(double scale, FloatPoint origin)
{
    // FIXME: Scrollbars should stay in-place and change height while zooming.
    FloatPoint constrainedOrigin = constrainTransientZoomOrigin(scale, origin);
    auto* zoomLayer = layerForTransientZoom();

    TransformationMatrix transform;
    transform.translate(constrainedOrigin.x(), constrainedOrigin.y());
    transform.scale(scale);

    Locker locker { zoomLayer->lock() };
    zoomLayer->setTransform(transform);
    zoomLayer->setAnchorPoint(FloatPoint3D());
    zoomLayer->setPosition(FloatPoint());
}

void LayerTreeHost::adjustTransientZoom(double scale, FloatPoint origin, FloatPoint)
{
    m_transientZoom = true;
    m_transientZoomScale = scale;
    m_transientZoomOrigin = origin;

    applyTransientZoomToLayers(m_transientZoomScale, m_transientZoomOrigin);

    if (m_isWaitingForRenderer)
        scheduleRenderingUpdate();
    else
        updateRendering();
}

void LayerTreeHost::commitTransientZoom(double scale, FloatPoint origin, FloatPoint unscrolledOrigin)
{
    if (m_transientZoomScale == scale) {
        // If the page scale is already the target scale, setPageScaleFactor() will short-circuit
        // and not apply the transform, so we can't depend on it to do so.
        TransformationMatrix finalTransform;
        finalTransform.scale(scale);

        auto* zoomLayer = layerForTransientZoom();
        Locker locker { zoomLayer->lock() };
        zoomLayer->setTransform(finalTransform);
    }

    m_transientZoom = false;
    m_transientZoomScale = 1;
    m_transientZoomOrigin = FloatPoint();

    Ref webPage = m_webPage;
    webPage->scalePage(scale / webPage->viewScaleFactor(), roundedIntPoint(-unscrolledOrigin));
}
#endif

#if PLATFORM(WPE) && ENABLE(WPE_PLATFORM) && (USE(GBM) || OS(ANDROID))
void LayerTreeHost::preferredBufferFormatsDidChange()
{
    m_compositor->preferredBufferFormatsDidChange();
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void LayerTreeHost::notifyFrameDamageForTesting(Region&& damageRegion)
{
    Locker locker { m_frameDamageHistoryForTestingLock };
    m_frameDamageHistoryForTesting.append(WTF::move(damageRegion));
}

void LayerTreeHost::resetDamageHistoryForTesting()
{
    {
        Locker locker { m_frameDamageHistoryForTestingLock };
        m_frameDamageHistoryForTesting.clear();
    }
    m_compositor->enableFrameDamageNotificationForTesting();
}

void LayerTreeHost::foreachRegionInDamageHistoryForTesting(NOESCAPE const Function<void(const Region&)>& callback) const
{
    Locker locker { m_frameDamageHistoryForTestingLock };
    for (const auto& region : m_frameDamageHistoryForTesting)
        callback(region);
}
#endif

void LayerTreeHost::fillGLInformation(RenderProcessInfo&& info, CompletionHandler<void(RenderProcessInfo&&)>&& completionHandler)
{
    info.paintingThreadsCount = CoordinatedTileManager::numberOfPaintingThreads();
    m_compositor->fillGLInformation(WTF::move(info), WTF::move(completionHandler));
}

void LayerTreeHost::releaseMemory(WTF::Critical critical)
{
    PlatformDisplay::sharedDisplay().skiaReleaseUnusedResources(critical);
    m_compositor->releaseMemory(critical);
}

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
