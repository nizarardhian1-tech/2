// ============================================================================
// Tool/Unity.h  [REFACTORED — Pilar 3: Native AInputEvent Hook]
// ============================================================================
#pragma once

#include <cstdint>
#include <android/input.h>
#include <jni.h>

// ── Unity touch structs (tidak berubah) ──────────────────────────────────────
struct UnityEngine_Vector2 { float x; float y; };

enum class UnityEngine_TouchPhase {
    Began = 0, Moved = 1, Stationary = 2, Ended = 3, Canceled = 4,
};
enum class UnityEngine_TouchType {
    Direct = 0, Indirect = 1, Stylus = 2,
};
struct UnityEngine_Touch {
    int32_t                  m_FingerId;
    UnityEngine_Vector2      m_Position;
    UnityEngine_Vector2      m_RawPosition;
    UnityEngine_Vector2      m_PositionDelta;
    float                    m_TimeDelta;
    int32_t                  m_TapCount;
    UnityEngine_TouchPhase   m_Phase;
    UnityEngine_TouchType    m_Type;
    float                    m_Pressure;
    float                    m_maximumPossiblePressure;
    float                    m_Radius;
    float                    m_RadiusVariance;
    float                    m_AltitudeAngle;
    float                    m_AzimuthAngle;
};

// ── NativeInput: Layer 1 — AInputEvent universal hook ────────────────────────
namespace NativeInput {
    /**
     * Forward raw AInputEvent ke ImGui (dari hook AInputQueue).
     * Thread-safe, boleh dipanggil dari thread apapun.
     */
    void ForwardEvent(const AInputEvent* event);

    /**
     * Forward koordinat touch langsung (dari JNI bridge Activity.dispatchTouchEvent).
     * @param x, y    Koordinat Android (Y akan di-flip otomatis)
     * @param action  AMOTION_EVENT_ACTION_DOWN/UP/MOVE/CANCEL
     */
    void ForwardTouch(float x, float y, int32_t action);

    bool IsActive();
    void SetActive(bool v);
}

// ── Unity namespace ───────────────────────────────────────────────────────────
namespace Unity {
    /**
     * Install input hook dengan auto-fallback 3 layer:
     *   Layer 1: NativeInput (libandroid + JNI bridge) — universal, semua Unity versi
     *   Layer 2: UnityEngine.Input IL2CPP hook — legacy fallback
     *   Layer 3: no-op (tool tetap render)
     */
    void HookInput();

    // Status query (untuk debug UI)
    bool IsNativeInputActive();
    bool IsLegacyInputActive();
}
