/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkGpuBlurUtils.h"

#include "include/core/SkRect.h"

#if SK_SUPPORT_GPU
#include "include/private/GrRecordingContext.h"
#include "src/gpu/GrCaps.h"
#include "src/gpu/GrFixedClip.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/GrRenderTargetContextPriv.h"
#include "src/gpu/effects/GrGaussianConvolutionFragmentProcessor.h"
#include "src/gpu/effects/GrMatrixConvolutionEffect.h"

#include "src/gpu/SkGr.h"

#define MAX_BLUR_SIGMA 4.0f

using Direction = GrGaussianConvolutionFragmentProcessor::Direction;

static void scale_irect_roundout(SkIRect* rect, float xScale, float yScale) {
    rect->fLeft   = SkScalarFloorToInt(rect->fLeft  * xScale);
    rect->fTop    = SkScalarFloorToInt(rect->fTop   * yScale);
    rect->fRight  = SkScalarCeilToInt(rect->fRight  * xScale);
    rect->fBottom = SkScalarCeilToInt(rect->fBottom * yScale);
}

static void scale_irect(SkIRect* rect, int xScale, int yScale) {
    rect->fLeft   *= xScale;
    rect->fTop    *= yScale;
    rect->fRight  *= xScale;
    rect->fBottom *= yScale;
}

#ifdef SK_DEBUG
static inline int is_even(int x) { return !(x & 1); }
#endif

static void shrink_irect_by_2(SkIRect* rect, bool xAxis, bool yAxis) {
    if (xAxis) {
        SkASSERT(is_even(rect->fLeft) && is_even(rect->fRight));
        rect->fLeft /= 2;
        rect->fRight /= 2;
    }
    if (yAxis) {
        SkASSERT(is_even(rect->fTop) && is_even(rect->fBottom));
        rect->fTop /= 2;
        rect->fBottom /= 2;
    }
}

static float adjust_sigma(float sigma, int maxTextureSize, int *scaleFactor, int *radius) {
    *scaleFactor = 1;
    while (sigma > MAX_BLUR_SIGMA) {
        *scaleFactor *= 2;
        sigma *= 0.5f;
        if (*scaleFactor > maxTextureSize) {
            *scaleFactor = maxTextureSize;
            sigma = MAX_BLUR_SIGMA;
        }
    }
    *radius = static_cast<int>(ceilf(sigma * 3.0f));
    SkASSERT(*radius <= GrGaussianConvolutionFragmentProcessor::kMaxKernelRadius);
    return sigma;
}

static GrTextureDomain::Mode to_texture_domain_mode(SkTileMode tileMode) {
    switch (tileMode) {
        case SkTileMode::kClamp:
            return GrTextureDomain::kClamp_Mode;
        case SkTileMode::kDecal:
            return GrTextureDomain::kDecal_Mode;
        case SkTileMode::kMirror:
            // TODO (michaelludwig) - Support mirror mode, treat as repeat for now
        case SkTileMode::kRepeat:
            return GrTextureDomain::kRepeat_Mode;
        default:
            SK_ABORT("Unsupported tile mode.");
    }
}

static void convolve_gaussian_1d(GrRenderTargetContext* renderTargetContext,
                                 const GrClip& clip,
                                 const SkIRect& dstRect,
                                 const SkIPoint& srcOffset,
                                 sk_sp<GrTextureProxy> proxy,
                                 SkAlphaType srcAlphaType,
                                 Direction direction,
                                 int radius,
                                 float sigma,
                                 SkTileMode mode,
                                 int bounds[2]) {
    GrPaint paint;
    auto domainMode = to_texture_domain_mode(mode);
    int realBounds[2];
    if (bounds) {
        realBounds[0] = bounds[0]; realBounds[1] = bounds[1];
    } else {
        realBounds[0] = 0;
        realBounds[1] = direction == Direction::kX ? proxy->width() : proxy->height();
    }
    std::unique_ptr<GrFragmentProcessor> conv(GrGaussianConvolutionFragmentProcessor::Make(
            std::move(proxy), srcAlphaType, direction, radius, sigma, domainMode, realBounds));
    paint.addColorFragmentProcessor(std::move(conv));
    paint.setPorterDuffXPFactory(SkBlendMode::kSrc);
    SkMatrix localMatrix = SkMatrix::MakeTrans(-SkIntToScalar(srcOffset.x()),
                                               -SkIntToScalar(srcOffset.y()));
    renderTargetContext->fillRectWithLocalMatrix(clip, std::move(paint), GrAA::kNo, SkMatrix::I(),
                                                 SkRect::Make(dstRect), localMatrix);
}

static std::unique_ptr<GrRenderTargetContext> convolve_gaussian_2d(GrRecordingContext* context,
                                                                   sk_sp<GrTextureProxy> srcProxy,
                                                                   GrColorType srcColorType,
                                                                   const SkIRect& srcBounds,
                                                                   const SkIPoint& srcOffset,
                                                                   int radiusX,
                                                                   int radiusY,
                                                                   SkScalar sigmaX,
                                                                   SkScalar sigmaY,
                                                                   SkTileMode mode,
                                                                   int finalW,
                                                                   int finalH,
                                                                   sk_sp<SkColorSpace> finalCS,
                                                                   SkBackingFit dstFit) {
    auto renderTargetContext = GrRenderTargetContext::Make(
            context, srcColorType, std::move(finalCS), dstFit, {finalW, finalH}, 1,
            GrMipMapped::kNo, srcProxy->isProtected(), srcProxy->origin());
    if (!renderTargetContext) {
        return nullptr;
    }

    SkMatrix localMatrix = SkMatrix::MakeTrans(-SkIntToScalar(srcOffset.x()),
                                               -SkIntToScalar(srcOffset.y()));
    SkISize size = SkISize::Make(2 * radiusX + 1,  2 * radiusY + 1);
    SkIPoint kernelOffset = SkIPoint::Make(radiusX, radiusY);
    GrPaint paint;
    auto domainMode = to_texture_domain_mode(mode);
    auto conv = GrMatrixConvolutionEffect::MakeGaussian(std::move(srcProxy), srcBounds, size,
                                                        1.0, 0.0, kernelOffset, domainMode, true,
                                                        sigmaX, sigmaY);
    paint.addColorFragmentProcessor(std::move(conv));
    paint.setPorterDuffXPFactory(SkBlendMode::kSrc);
    GrFixedClip clip(SkIRect::MakeWH(finalW, finalH));

    renderTargetContext->fillRectWithLocalMatrix(clip, std::move(paint), GrAA::kNo, SkMatrix::I(),
                                                 SkRect::MakeWH(finalW, finalH), localMatrix);

    return renderTargetContext;
}

// NOTE: Both convolve_gaussian or decimate accept a proxyOffset. This is separate from the
// srcBounds and srcOffset, which are relative to the content rect of the image, whereas proxyOffset
// maps from the content rect to the proxy's coordinate space. Due to how the destination bounds are
// calculated, it is more convenient to have the proxy offset kept separate from the logical bounds
// (which do impact destination decisions). Both functions incorporate the proxy offset into the
// geometry they submit or before calling convolve_gaussian_1d.

static std::unique_ptr<GrRenderTargetContext> convolve_gaussian(GrRecordingContext* context,
                                                                sk_sp<GrTextureProxy> srcProxy,
                                                                GrColorType srcColorType,
                                                                SkAlphaType srcAlphaType,
                                                                const SkIPoint& proxyOffset,
                                                                const SkIRect& srcRect,
                                                                const SkIPoint& srcOffset,
                                                                Direction direction,
                                                                int radius,
                                                                float sigma,
                                                                SkIRect* contentRect,
                                                                SkTileMode mode,
                                                                int finalW,
                                                                int finalH,
                                                                sk_sp<SkColorSpace> finalCS,
                                                                SkBackingFit fit) {
    SkASSERT(srcRect.width() <= finalW && srcRect.height() <= finalH);

    auto dstRenderTargetContext = GrRenderTargetContext::Make(
            context, srcColorType, std::move(finalCS), fit, srcRect.size(), 1,
            GrMipMapped::kNo, srcProxy->isProtected(), srcProxy->origin());
    if (!dstRenderTargetContext) {
        return nullptr;
    }

    GrFixedClip clip(SkIRect::MakeWH(finalW, finalH));

    SkIRect dstRect = SkIRect::MakeWH(srcRect.width(), srcRect.height());
    SkIPoint netOffset = srcOffset - proxyOffset;
    if (SkTileMode::kClamp == mode && proxyOffset.isZero() &&
        contentRect->contains(SkIRect::MakeSize(srcProxy->backingStoreDimensions()))) {
        *contentRect = dstRect;
        convolve_gaussian_1d(dstRenderTargetContext.get(), clip, dstRect, netOffset,
                             std::move(srcProxy), srcAlphaType, direction, radius, sigma,
                             SkTileMode::kClamp, nullptr);
        return dstRenderTargetContext;
    }

    // These destination rects need to be adjusted by srcOffset, but should *not* be adjusted by
    // the proxyOffset, which is why keeping them separate is convenient.
    SkIRect midRect = *contentRect, leftRect, rightRect;
    midRect.offset(srcOffset);
    SkIRect topRect, bottomRect;
    int bounds[2];
    if (Direction::kX == direction) {
        bounds[0] = contentRect->left() + proxyOffset.x();
        bounds[1] = contentRect->right() + proxyOffset.x();
        topRect = SkIRect::MakeLTRB(0, 0, dstRect.right(), midRect.top());
        bottomRect = SkIRect::MakeLTRB(0, midRect.bottom(), dstRect.right(), dstRect.bottom());
        midRect.inset(radius, 0);
        leftRect = SkIRect::MakeLTRB(0, midRect.top(), midRect.left(), midRect.bottom());
        rightRect =
            SkIRect::MakeLTRB(midRect.right(), midRect.top(), dstRect.width(), midRect.bottom());
        dstRect.fTop = midRect.top();
        dstRect.fBottom = midRect.bottom();

        contentRect->fLeft = dstRect.fLeft;
        contentRect->fTop = midRect.fTop;
        contentRect->fRight = dstRect.fRight;
        contentRect->fBottom = midRect.fBottom;
    } else {
        bounds[0] = contentRect->top() + proxyOffset.y();
        bounds[1] = contentRect->bottom() + proxyOffset.y();
        topRect = SkIRect::MakeLTRB(0, 0, midRect.left(), dstRect.bottom());
        bottomRect = SkIRect::MakeLTRB(midRect.right(), 0, dstRect.right(), dstRect.bottom());
        midRect.inset(0, radius);
        leftRect = SkIRect::MakeLTRB(midRect.left(), 0, midRect.right(), midRect.top());
        rightRect =
            SkIRect::MakeLTRB(midRect.left(), midRect.bottom(), midRect.right(), dstRect.height());
        dstRect.fLeft = midRect.left();
        dstRect.fRight = midRect.right();

        contentRect->fLeft = midRect.fLeft;
        contentRect->fTop = dstRect.fTop;
        contentRect->fRight = midRect.fRight;
        contentRect->fBottom = dstRect.fBottom;
    }
    if (!topRect.isEmpty()) {
        dstRenderTargetContext->clear(&topRect, SK_PMColor4fTRANSPARENT,
                                      GrRenderTargetContext::CanClearFullscreen::kYes);
    }

    if (!bottomRect.isEmpty()) {
        dstRenderTargetContext->clear(&bottomRect, SK_PMColor4fTRANSPARENT,
                                      GrRenderTargetContext::CanClearFullscreen::kYes);
    }

    if (midRect.isEmpty()) {
        // Blur radius covers srcBounds; use bounds over entire draw
        convolve_gaussian_1d(dstRenderTargetContext.get(), clip, dstRect, netOffset,
                             std::move(srcProxy), srcAlphaType, direction, radius, sigma, mode,
                             bounds);
    } else {
        // Draw right and left margins with bounds; middle without.
        convolve_gaussian_1d(dstRenderTargetContext.get(), clip, leftRect, netOffset, srcProxy,
                             srcAlphaType, direction, radius, sigma, mode, bounds);
        convolve_gaussian_1d(dstRenderTargetContext.get(), clip, rightRect, netOffset, srcProxy,
                             srcAlphaType, direction, radius, sigma, mode, bounds);
        convolve_gaussian_1d(dstRenderTargetContext.get(), clip, midRect, netOffset,
                             std::move(srcProxy), srcAlphaType, direction, radius, sigma,
                             SkTileMode::kClamp, nullptr);
    }

    return dstRenderTargetContext;
}

// Returns a high quality scaled-down version of src. This is used to create an intermediate,
// shrunken version of the source image in the event that the requested blur sigma exceeds
// MAX_BLUR_SIGMA.
static sk_sp<GrTextureProxy> decimate(GrRecordingContext* context,
                                      sk_sp<GrTextureProxy> srcProxy,
                                      GrColorType srcColorType,
                                      SkAlphaType srcAlphaType,
                                      const SkIPoint& proxyOffset,
                                      SkIPoint* srcOffset,
                                      SkIRect* contentRect,
                                      int scaleFactorX,
                                      int scaleFactorY,
                                      SkTileMode mode,
                                      sk_sp<SkColorSpace> finalCS) {
    SkASSERT(SkIsPow2(scaleFactorX) && SkIsPow2(scaleFactorY));
    SkASSERT(scaleFactorX > 1 || scaleFactorY > 1);

    SkIRect srcRect = *contentRect;
    srcRect.offset(*srcOffset);

    scale_irect_roundout(&srcRect, 1.0f / scaleFactorX, 1.0f / scaleFactorY);
    scale_irect(&srcRect, scaleFactorX, scaleFactorY);

    SkIRect dstRect(srcRect);

    // Map the src rect into proxy space, this only has to happen once since subsequent loops
    // to decimate will have created a new proxy that has its origin at (0, 0).
    srcRect.offset(proxyOffset.x(), proxyOffset.y());
    std::unique_ptr<GrRenderTargetContext> dstRenderTargetContext;

    for (int i = 1; i < scaleFactorX || i < scaleFactorY; i *= 2) {
        shrink_irect_by_2(&dstRect, i < scaleFactorX, i < scaleFactorY);

        dstRenderTargetContext = GrRenderTargetContext::Make(
                context, srcColorType, finalCS, SkBackingFit::kApprox,
                {dstRect.fRight, dstRect.fBottom}, 1, GrMipMapped::kNo, srcProxy->isProtected(),
                srcProxy->origin());
        if (!dstRenderTargetContext) {
            return nullptr;
        }

        GrPaint paint;
        auto fp = GrTextureEffect::Make(std::move(srcProxy), srcAlphaType, SkMatrix::I(),
                                        GrSamplerState::Filter::kBilerp);
        if (i == 1) {
            // GrDomainEffect does not support kRepeat_Mode with GrSamplerState::Filter.
            GrTextureDomain::Mode domainMode;
            if (mode == SkTileMode::kClamp) {
                domainMode = GrTextureDomain::kClamp_Mode;
            } else {
                // GrDomainEffect does not support k[Mirror]Repeat with GrSamplerState::Filter.
                // So we use decal.
                domainMode = GrTextureDomain::kDecal_Mode;
            }
            SkRect domain = SkRect::Make(*contentRect);
            domain.inset((i < scaleFactorX) ? SK_ScalarHalf + SK_ScalarNearlyZero : 0.0f,
                         (i < scaleFactorY) ? SK_ScalarHalf + SK_ScalarNearlyZero : 0.0f);
            // Ensure that the insetting doesn't invert the domain rectangle.
            if (domain.fRight < domain.fLeft) {
                domain.fLeft = domain.fRight = SkScalarAve(domain.fLeft, domain.fRight);
            }
            if (domain.fBottom < domain.fTop) {
                domain.fTop = domain.fBottom = SkScalarAve(domain.fTop, domain.fBottom);
            }
            domain.offset(proxyOffset.x(), proxyOffset.y());
            fp = GrDomainEffect::Make(std::move(fp), domain, domainMode, true);
            srcRect.offset(-(*srcOffset));
            // TODO: consume the srcOffset in both first draws and always set it to zero
            // back in GaussianBlur
            srcOffset->set(0, 0);
        }
        paint.addColorFragmentProcessor(std::move(fp));
        paint.setPorterDuffXPFactory(SkBlendMode::kSrc);

        dstRenderTargetContext->fillRectToRect(GrFixedClip::Disabled(), std::move(paint), GrAA::kNo,
                                               SkMatrix::I(), SkRect::Make(dstRect),
                                               SkRect::Make(srcRect));

        srcProxy = dstRenderTargetContext->asTextureProxyRef();
        if (!srcProxy) {
            return nullptr;
        }
        srcRect = dstRect;
    }

    *contentRect = dstRect;

    SkASSERT(dstRenderTargetContext);

    return dstRenderTargetContext->asTextureProxyRef();
}

// Expand the contents of 'srcRenderTargetContext' to fit in 'dstII'. At this point, we are
// expanding an intermediate image, so there's no need to account for a proxy offset from the
// original input.
static std::unique_ptr<GrRenderTargetContext> reexpand(
        GrRecordingContext* context,
        std::unique_ptr<GrRenderTargetContext> srcRenderTargetContext,
        const SkIRect& localSrcBounds,
        int scaleFactorX, int scaleFactorY,
        int finalW,
        int finalH,
        sk_sp<SkColorSpace> finalCS,
        SkBackingFit fit) {
    const SkIRect srcRect = SkIRect::MakeWH(srcRenderTargetContext->width(),
                                            srcRenderTargetContext->height());

    sk_sp<GrTextureProxy> srcProxy = srcRenderTargetContext->asTextureProxyRef();
    if (!srcProxy) {
        return nullptr;
    }

    GrColorType srcColorType = srcRenderTargetContext->colorInfo().colorType();
    SkAlphaType srcAlphaType = srcRenderTargetContext->colorInfo().alphaType();

    srcRenderTargetContext = nullptr; // no longer needed

    auto dstRenderTargetContext = GrRenderTargetContext::Make(
            context, srcColorType, std::move(finalCS), fit, {finalW, finalH}, 1,
            GrMipMapped::kNo, srcProxy->isProtected(), srcProxy->origin());
    if (!dstRenderTargetContext) {
        return nullptr;
    }

    GrPaint paint;
    SkRect domain = GrTextureDomain::MakeTexelDomain(localSrcBounds, GrTextureDomain::kClamp_Mode,
                                                     GrTextureDomain::kClamp_Mode);
    auto fp = GrTextureEffect::Make(std::move(srcProxy), srcAlphaType, SkMatrix::I(),
                                    GrSamplerState::Filter::kBilerp);
    fp = GrDomainEffect::Make(std::move(fp), domain, GrTextureDomain::kClamp_Mode, true);
    paint.addColorFragmentProcessor(std::move(fp));
    paint.setPorterDuffXPFactory(SkBlendMode::kSrc);
    GrFixedClip clip(SkIRect::MakeWH(finalW, finalH));

    // TODO: using dstII as dstRect results in some image diffs - why?
    SkIRect dstRect(srcRect);
    scale_irect(&dstRect, scaleFactorX, scaleFactorY);

    dstRenderTargetContext->fillRectToRect(clip, std::move(paint), GrAA::kNo, SkMatrix::I(),
                                           SkRect::Make(dstRect), SkRect::Make(srcRect));

    return dstRenderTargetContext;
}

static std::unique_ptr<GrRenderTargetContext> two_pass_gaussian(GrRecordingContext* context,
                                                                sk_sp<GrTextureProxy> srcProxy,
                                                                GrColorType srcColorType,
                                                                SkAlphaType srcAlphaType,
                                                                sk_sp<SkColorSpace> colorSpace,
                                                                SkIPoint proxyOffset,
                                                                int finalW,
                                                                int finalH,
                                                                SkIRect srcRect,
                                                                SkIPoint srcOffset,
                                                                SkIRect* srcBounds,
                                                                float sigmaX,
                                                                float sigmaY,
                                                                int radiusX,
                                                                int radiusY,
                                                                SkTileMode mode,
                                                                SkBackingFit fit) {
    std::unique_ptr<GrRenderTargetContext> dstRenderTargetContext;
    if (sigmaX > 0.0f) {
        SkBackingFit xFit = sigmaY > 0 ? SkBackingFit::kApprox : fit;
        dstRenderTargetContext =
                convolve_gaussian(context, std::move(srcProxy), srcColorType, srcAlphaType,
                                  proxyOffset, srcRect, srcOffset, Direction::kX, radiusX, sigmaX,
                                  srcBounds, mode, finalW, finalH, colorSpace, xFit);
        if (!dstRenderTargetContext) {
            return nullptr;
        }
        srcProxy = dstRenderTargetContext->asTextureProxyRef();

        srcRect.offsetTo(0, 0);
        srcOffset.set(0, 0);
        proxyOffset.set(0, 0);
    }

    if (sigmaY == 0.0f) {
        return dstRenderTargetContext;
    }

    return convolve_gaussian(context, std::move(srcProxy), srcColorType, srcAlphaType,
                             proxyOffset, srcRect, srcOffset, Direction::kY, radiusY, sigmaY,
                             srcBounds, mode, finalW, finalH, colorSpace, fit);
}

namespace SkGpuBlurUtils {

std::unique_ptr<GrRenderTargetContext> GaussianBlur(GrRecordingContext* context,
                                                    sk_sp<GrTextureProxy> srcProxy,
                                                    GrColorType srcColorType,
                                                    SkAlphaType srcAlphaType,
                                                    const SkIPoint& proxyOffset,
                                                    sk_sp<SkColorSpace> colorSpace,
                                                    const SkIRect& dstBounds,
                                                    const SkIRect& srcBounds,
                                                    float sigmaX,
                                                    float sigmaY,
                                                    SkTileMode mode,
                                                    SkBackingFit fit) {
    SkASSERT(context);

    TRACE_EVENT2("skia.gpu", "GaussianBlur", "sigmaX", sigmaX, "sigmaY", sigmaY);

    int finalW = dstBounds.width();
    int finalH = dstBounds.height();

    int scaleFactorX, radiusX;
    int scaleFactorY, radiusY;
    int maxTextureSize = context->priv().caps()->maxTextureSize();
    sigmaX = adjust_sigma(sigmaX, maxTextureSize, &scaleFactorX, &radiusX);
    sigmaY = adjust_sigma(sigmaY, maxTextureSize, &scaleFactorY, &radiusY);
    SkASSERT(sigmaX || sigmaY);

    SkIPoint srcOffset = SkIPoint::Make(-dstBounds.x(), -dstBounds.y());

    auto localSrcBounds = srcBounds;
    if (scaleFactorX == 1 && scaleFactorY == 1) {
        // For really small blurs (certainly no wider than 5x5 on desktop GPUs) it is faster to just
        // launch a single non separable kernel vs two launches.
        if (sigmaX > 0 && sigmaY > 0 && (2 * radiusX + 1) * (2 * radiusY + 1) <= MAX_KERNEL_SIZE) {
            // Apply the proxy offset to src bounds and offset directly
            return convolve_gaussian_2d(context, std::move(srcProxy), srcColorType,
                                        srcBounds.makeOffset(proxyOffset), srcOffset - proxyOffset,
                                        radiusX, radiusY, sigmaX, sigmaY, mode, finalW, finalH,
                                        colorSpace, fit);
        }
        auto srcRect = SkIRect::MakeWH(finalW, finalH);
        return two_pass_gaussian(context, std::move(srcProxy), srcColorType, srcAlphaType,
                                 std::move(colorSpace), proxyOffset, finalW, finalH, srcRect,
                                 srcOffset, &localSrcBounds, sigmaX, sigmaY, radiusX, radiusY, mode,
                                 fit);
    }

    srcProxy = decimate(context, std::move(srcProxy), srcColorType, srcAlphaType, proxyOffset,
                        &srcOffset, &localSrcBounds, scaleFactorX, scaleFactorY, mode, colorSpace);
    if (!srcProxy) {
        return nullptr;
    }
    auto srcRect = SkIRect::MakeWH(finalW, finalH);
    scale_irect_roundout(&srcRect, 1.0f / scaleFactorX, 1.0f / scaleFactorY);
    auto rtc =
            two_pass_gaussian(context, std::move(srcProxy), srcColorType, srcAlphaType, colorSpace,
                              {0, 0}, finalW, finalH, srcRect, srcOffset, &localSrcBounds, sigmaX,
                              sigmaY, radiusX, radiusY, mode, SkBackingFit::kApprox);
    if (!rtc) {
        return nullptr;
    }
    return reexpand(context, std::move(rtc), localSrcBounds, scaleFactorX, scaleFactorY, finalW,
                    finalH, std::move(colorSpace), fit);
}

}

#endif
