/*
 * Copyright 2020 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkCanvas.h"
#include "include/core/SkMatrix44.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/private/SkM44.h"
#include "include/utils/Sk3D.h"
#include "include/utils/SkRandom.h"
#include "samplecode/Sample.h"
#include "tools/Resources.h"

static SkMatrix44 inv(const SkMatrix44& m) {
    SkMatrix44 inverse;
    SkAssertResult(m.invert(&inverse));
    return inverse;
}

static SkM44 inv(const SkM44& m) {
    SkM44 inverse;
    SkAssertResult(m.invert(&inverse));
    return inverse;
}

static SkPoint project(const SkM44& m, SkV4 p) {
    auto v = m * p;
    return {v.x / v.w, v.y / v.w};
}

class Sample3DView : public Sample {
protected:
    float   fNear = 0.05f;
    float   fFar = 4;
    float   fAngle = SK_ScalarPI / 12;

    SkPoint3    fEye { 0, 0, 1.0f/tan(fAngle/2) - 1 };
    SkPoint3    fCOA { 0, 0, 0 };
    SkPoint3    fUp  { 0, 1, 0 };

    SkMatrix44  fRot;
    SkPoint3    fTrans;

    void rotate(float x, float y, float z) {
        SkMatrix44 r;
        if (x) {
            r.setRotateAboutUnit(1, 0, 0, x);
        } else if (y) {
            r.setRotateAboutUnit(0, 1, 0, y);
        } else {
            r.setRotateAboutUnit(0, 0, 1, z);
        }
        fRot.postConcat(r);
    }

public:
    void saveCamera(SkCanvas* canvas, const SkRect& area, SkScalar zscale) {
        SkMatrix44  camera,
                    perspective,
                    viewport;

        Sk3Perspective(&perspective, fNear, fFar, fAngle);
        Sk3LookAt(&camera, fEye, fCOA, fUp);
        viewport.setScale(area.width()*0.5f, area.height()*0.5f, zscale)
                .postTranslate(area.centerX(), area.centerY(), 0);

        // want "world" to be in our big coordinates (e.g. area), so apply this inverse
        // as part of our "camera".
        canvas->experimental_saveCamera(viewport * perspective, camera * inv(viewport));
    }

    bool onChar(SkUnichar uni) override {
        float delta = SK_ScalarPI / 30;
        switch (uni) {
            case '8': this->rotate( delta, 0, 0); return true;
            case '2': this->rotate(-delta, 0, 0); return true;
            case '4': this->rotate(0,  delta, 0); return true;
            case '6': this->rotate(0, -delta, 0); return true;
            case '-': this->rotate(0, 0,  delta); return true;
            case '+': this->rotate(0, 0, -delta); return true;

            case 'i': fTrans.fZ += 0.1f; SkDebugf("z %g\n", fTrans.fZ); return true;
            case 'k': fTrans.fZ -= 0.1f; SkDebugf("z %g\n", fTrans.fZ); return true;

            case 'n': fNear += 0.1f; SkDebugf("near %g\n", fNear); return true;
            case 'N': fNear -= 0.1f; SkDebugf("near %g\n", fNear); return true;
            case 'f': fFar  += 0.1f; SkDebugf("far  %g\n", fFar); return true;
            case 'F': fFar  -= 0.1f; SkDebugf("far  %g\n", fFar); return true;
            default: break;
        }
        return false;
    }
};

static SkMatrix44 RX(SkScalar rad) {
    SkScalar c = SkScalarCos(rad), s = SkScalarSin(rad);
    SkMatrix44 m;
    m.set3x3(1, 0, 0,
             0, c, s,
             0,-s, c);
    return m;
}

static SkMatrix44 RY(SkScalar rad) {
    SkScalar c = SkScalarCos(rad), s = SkScalarSin(rad);
    SkMatrix44 m;
    m.set3x3( c, 0,-s,
              0, 1, 0,
              s, 0, c);
    return m;
}

struct Face {
    SkScalar fRx, fRy;
    SkColor  fColor;

    static SkMatrix44 T(SkScalar x, SkScalar y, SkScalar z) {
        SkMatrix44 m;
        m.setTranslate(x, y, z);
        return m;
    }

    static SkMatrix44 R(SkScalar x, SkScalar y, SkScalar z, SkScalar rad) {
        SkMatrix44 m;
        m.setRotateAboutUnit(x, y, z, rad);
        return m;
    }

    SkMatrix44 asM44(SkScalar scale) const {
        return RY(fRy) * RX(fRx) * T(0, 0, scale);
    }
};

static bool front(const SkM44& m) {
    SkM44 m2;
    m.invert(&m2);
    /*
     *  Classically we want to dot the transpose(inverse(ctm)) with our surface normal.
     *  In this case, the normal is known to be {0, 0, 1}, so we only actually need to look
     *  at the z-scale of the inverse (the transpose doesn't change the main diagonal, so
     *  no need to actually transpose).
     */
    return m2.atColMajor(10) > 0;
}

const Face faces[] = {
    {             0,             0,  SK_ColorRED }, // front
    {             0,   SK_ScalarPI,  SK_ColorGREEN }, // back

    { SK_ScalarPI/2,             0,  SK_ColorBLUE }, // top
    {-SK_ScalarPI/2,             0,  SK_ColorCYAN }, // bottom

    {             0, SK_ScalarPI/2,  SK_ColorMAGENTA }, // left
    {             0,-SK_ScalarPI/2,  SK_ColorYELLOW }, // right
};

#include "include/core/SkColorFilter.h"
#include "include/effects/SkColorMatrix.h"

static SkV3 normalize(SkV3 v) { return v * (1.0f / v.length()); }

static SkColorMatrix comput_planar_lighting(SkCanvas* canvas, SkV3 lightDir) {
    SkM44 l2w = canvas->experimental_getLocalToWorld();
    auto normal = normalize(l2w * SkV3{0, 0, 1});
    float dot = -normal * lightDir;

    SkColorMatrix cm;
    if (dot < 0) {
        dot = 0;
    }

    float ambient = 0.5f;
    float scale = ambient + dot;
    cm.setScale(scale, scale, scale, 1);
    return cm;
}

struct Light {
    SkPoint fCenter;
    SkPoint fEndPt;
    SkScalar fRadius;
    SkScalar fHeight;

    bool hitTest(SkScalar x, SkScalar y) const {
        auto xx = x - fCenter.fX;
        auto yy = y - fCenter.fY;
        return xx*xx + yy*yy <= fRadius*fRadius;
    }

    void update(SkScalar x, SkScalar y) {
        auto xx = x - fCenter.fX;
        auto yy = y - fCenter.fY;
        auto len = SkScalarSqrt(xx*xx + yy*yy);
        if (len > fRadius) {
            xx *= fRadius / len;
            yy *= fRadius / len;
        }
        fEndPt = {fCenter.fX + xx, fCenter.fY + yy};
    }

    SkV3 getDir() const {
        auto pt = fEndPt - fCenter;
        return normalize({pt.fX, pt.fY, -fHeight});
    }

    void draw(SkCanvas* canvas) {
        SkPaint paint;
        paint.setAntiAlias(true);
        canvas->drawCircle(fCenter.fX, fCenter.fY, 5, paint);
        paint.setStyle(SkPaint::kStroke_Style);
        canvas->drawCircle(fCenter.fX, fCenter.fY, fRadius, paint);
        paint.setColor(SK_ColorRED);
        canvas->drawLine(fCenter.fX, fCenter.fY, fEndPt.fX, fEndPt.fY, paint);
    }
};

class SampleRR3D : public Sample3DView {
    SkRRect fRR;
    Light   fLight = {
        {60, 60}, {60, 60}, 50, 10
    };
    sk_sp<SkShader> fShader;

    SkString name() override { return SkString("rrect3d"); }

    void onOnceBeforeDraw() override {
        fRR = SkRRect::MakeRectXY({20, 20, 380, 380}, 50, 50);
        fShader = GetResourceAsImage("images/mandrill_128.png")
                        ->makeShader(SkMatrix::MakeScale(3, 3));
    }

    bool onChar(SkUnichar uni) override {
        return this->Sample3DView::onChar(uni);
    }

    void drawContent(SkCanvas* canvas, const SkMatrix44& m) {
        SkMatrix44 trans;
        trans.setTranslate(200, 200, 0);   // center of the rotation

        canvas->experimental_concat44(trans * fRot * m * inv(trans));

        if (!front(canvas->experimental_getLocalToDevice())) {
            return;
        }

        SkPaint paint;
        paint.setAlphaf(front(canvas->experimental_getLocalToDevice()) ? 1 : 0.25f);
        paint.setShader(fShader);

        SkColorMatrix cm = comput_planar_lighting(canvas, fLight.getDir());
        paint.setColorFilter(SkColorFilters::Matrix(cm));

        canvas->drawRRect(fRR, paint);
    }

    void onDrawContent(SkCanvas* canvas) override {
        canvas->save();
        canvas->translate(400, 300);

        this->saveCamera(canvas, {0, 0, 400, 400}, 200);

        for (auto f : faces) {
            SkAutoCanvasRestore acr(canvas, true);
            this->drawContent(canvas, f.asM44(200));
        }

        canvas->restore();
        canvas->restore();

        fLight.draw(canvas);
    }

    Click* onFindClickHandler(SkScalar x, SkScalar y, skui::ModifierKey modi) override {
        if (fLight.hitTest(x, y)) {
            return new Click();
        }
        return nullptr;
    }
    bool onClick(Click* click) override {
        fLight.update(click->fCurr.fX, click->fCurr.fY);
        return true;
    }
};
DEF_SAMPLE( return new SampleRR3D(); )

#include "include/effects/SkRuntimeEffect.h"

struct LightPos {
    SkV4     fPos;
    SkScalar fUIRadius;

    bool hitTest(SkScalar x, SkScalar y) const {
        auto xx = x - fPos.x;
        auto yy = y - fPos.y;
        return xx*xx + yy*yy <= fUIRadius*fUIRadius;
    }

    void update(SkScalar x, SkScalar y) {
        fPos.x = x;
        fPos.y = y;
    }

    void draw(SkCanvas* canvas) {
        SkPaint paint;
        paint.setAntiAlias(true);

        SkAutoCanvasRestore acr(canvas, true);
        canvas->experimental_concat44(SkM44::Translate(0, 0, fPos.z));
        canvas->drawCircle(fPos.x, fPos.y, fUIRadius, paint);
    }
};

class SamplePointLight3D : public Sample3DView {
    SkRRect fRR;
    LightPos fLight = {{200, 200, 800, 1}, 8};

    sk_sp<SkShader> fShader;
    sk_sp<SkRuntimeEffect> fEffect;

    SkM44 fWorldToClick,
          fClickToWorld;

    SkString name() override { return SkString("pointlight3d"); }

    void onOnceBeforeDraw() override {
        fRR = SkRRect::MakeRectXY({20, 20, 380, 380}, 50, 50);
        fShader = GetResourceAsImage("images/mandrill_128.png")
                        ->makeShader(SkMatrix::MakeScale(3, 3));

        const char code[] = R"(
        //    in fragmentProcessor texture;
        //       color = sample(texture) * half(scale);

            uniform float4x4 localToWorld;
            uniform float3   lightPos;

            void main(float x, float y, inout half4 color) {
                float3 plane_pos = (localToWorld * float4(x, y, 0, 1)).xyz;
                float3 plane_norm = normalize((localToWorld * float4(0, 0, 1, 0)).xyz);
                float3 light_dir = normalize(lightPos - plane_pos);
                float ambient = 0.5;
                float dp = dot(plane_norm, light_dir);
                float scale = ambient + max(dp, 0);

                color = color * half4(float4(scale, scale, scale, 1));
            }
        )";
        auto [effect, error] = SkRuntimeEffect::Make(SkString(code));
        if (!effect) {
            SkDebugf("runtime error %s\n", error.c_str());
        }
        fEffect = effect;
    }

    bool onChar(SkUnichar uni) override {
        switch (uni) {
            case 'X': fLight.fPos.x += 10; return true;
            case 'x': fLight.fPos.x -= 10; return true;
            case 'Y': fLight.fPos.y += 10; return true;
            case 'y': fLight.fPos.y -= 10; return true;
            case 'Z': fLight.fPos.z += 10; return true;
            case 'z': fLight.fPos.z -= 10; return true;
        }
        return this->Sample3DView::onChar(uni);
    }

    void drawContent(SkCanvas* canvas, const SkMatrix44& m, SkColor color) {
        SkMatrix44 trans;
        trans.setTranslate(200, 200, 0);   // center of the rotation

        canvas->experimental_concat44(trans * fRot * m * inv(trans));

        // wonder if the runtimeeffect can do this reject? (in a setup function)
        if (!front(canvas->experimental_getLocalToDevice())) {
            return;
        }

        struct Uniforms {
            SkM44  fLocalToWorld;
            SkV3   fLightPos;
        } uni;
        uni.fLocalToWorld = canvas->experimental_getLocalToWorld();
        uni.fLightPos     = {fLight.fPos.x, fLight.fPos.y, fLight.fPos.z};
        sk_sp<SkData> data = SkData::MakeWithCopy(&uni, sizeof(uni));

        SkPaint paint;
        paint.setColor(color);
        paint.setShader(fEffect->makeShader(data, &fShader, 0, nullptr, true));

        canvas->drawRRect(fRR, paint);
    }

    void setClickToWorld(SkCanvas* canvas, const SkM44& clickM) {
        auto l2d = canvas->experimental_getLocalToDevice();
        fWorldToClick = inv(clickM) * l2d;
        fClickToWorld = inv(fWorldToClick);
    }

    void onDrawContent(SkCanvas* canvas) override {
        if (canvas->getGrContext() == nullptr) {
            return;
        }
        SkM44 clickM = canvas->experimental_getLocalToDevice();

        canvas->save();
        canvas->translate(400, 300);

        this->saveCamera(canvas, {0, 0, 400, 400}, 200);

        this->setClickToWorld(canvas, clickM);

        for (auto f : faces) {
            SkAutoCanvasRestore acr(canvas, true);
            this->drawContent(canvas, f.asM44(200), f.fColor);
        }

        fLight.draw(canvas);
        canvas->restore();
        canvas->restore();
    }

    Click* onFindClickHandler(SkScalar x, SkScalar y, skui::ModifierKey modi) override {
        auto L = fWorldToClick * fLight.fPos;
        SkPoint c = project(fClickToWorld, {x, y, L.z/L.w, 1});
        if (fLight.hitTest(c.fX, c.fY)) {
            return new Click();
        }
        return nullptr;
    }
    bool onClick(Click* click) override {
        auto L = fWorldToClick * fLight.fPos;
        SkPoint c = project(fClickToWorld, {click->fCurr.fX, click->fCurr.fY, L.z/L.w, 1});
        fLight.update(c.fX, c.fY);
        return true;
    }
};
DEF_SAMPLE( return new SamplePointLight3D(); )
