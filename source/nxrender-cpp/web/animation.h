// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "nxgfx/transform.h"
#include "web/css/cascade.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cstdint>

namespace NXRender {
namespace Web {

class BoxNode;

// ==================================================================
// Easing functions (CSS timing functions)
// ==================================================================

class TimingFunction {
public:
    enum class Type : uint8_t {
        Linear, Ease, EaseIn, EaseOut, EaseInOut,
        CubicBezier, Steps, Spring
    };

    static TimingFunction linear();
    static TimingFunction ease();
    static TimingFunction easeIn();
    static TimingFunction easeOut();
    static TimingFunction easeInOut();
    static TimingFunction cubicBezier(float x1, float y1, float x2, float y2);
    static TimingFunction steps(int count, bool jumpStart = false);
    static TimingFunction parse(const std::string& cssValue);

    float evaluate(float t) const;

private:
    Type type_ = Type::Linear;
    float x1_ = 0, y1_ = 0, x2_ = 1, y2_ = 1;
    int stepCount_ = 1;
    bool stepJumpStart_ = false;

    float solveCubicBezier(float t) const;
    float bezierSample(float t) const;
};

// ==================================================================
// Keyframe: snapshot of animated properties at a progress point
// ==================================================================

struct AnimatedValue {
    enum class Type : uint8_t {
        None, Number, Color, Length, Transform, String
    } type = Type::None;

    float number = 0;
    uint32_t color = 0;
    std::string string;
    TransformMatrix transform;
};

struct Keyframe {
    float offset = 0;              // 0..1
    TimingFunction easing;
    std::unordered_map<std::string, AnimatedValue> properties;
};

// ==================================================================
// Animation effect — computes values at a given local time
// ==================================================================

class KeyframeEffect {
public:
    KeyframeEffect();
    ~KeyframeEffect();

    void setTarget(BoxNode* target) { target_ = target; }
    BoxNode* target() const { return target_; }

    void setKeyframes(const std::vector<Keyframe>& keyframes);
    const std::vector<Keyframe>& keyframes() const { return keyframes_; }

    void setProperty(const std::string& property) { property_ = property; }
    const std::string& property() const { return property_; }

    // Compute interpolated value at progress [0..1]
    AnimatedValue sample(float progress) const;

    // Apply computed value to target's computed style
    void apply(float progress);

private:
    BoxNode* target_ = nullptr;
    std::string property_;
    std::vector<Keyframe> keyframes_;

    // Find bounding keyframes for a progress value
    void findKeyframePair(float progress, size_t& fromIdx, size_t& toIdx, float& localT) const;

    // Interpolate between two values
    AnimatedValue interpolateValues(const AnimatedValue& from, const AnimatedValue& to,
                                      float t) const;

    // Apply a single animated property to ComputedValues
    void applyToStyle(ComputedValues& cv, const std::string& prop, const AnimatedValue& value);
};

// ==================================================================
// Animation — timing model (Web Animations API level 1)
// ==================================================================

class Animation {
public:
    enum class PlayState : uint8_t {
        Idle, Pending, Running, Paused, Finished
    };

    enum class FillMode : uint8_t {
        None, Forwards, Backwards, Both, Auto
    };

    enum class PlaybackDirection : uint8_t {
        Normal, Reverse, Alternate, AlternateReverse
    };

    Animation();
    ~Animation();

    // Configuration
    void setEffect(std::shared_ptr<KeyframeEffect> effect) { effect_ = effect; }
    KeyframeEffect* effect() const { return effect_.get(); }

    void setDuration(double ms) { duration_ = ms; }
    double duration() const { return duration_; }

    void setDelay(double ms) { delay_ = ms; }
    double delay() const { return delay_; }

    void setEndDelay(double ms) { endDelay_ = ms; }

    void setIterations(double count) { iterations_ = count; }
    double iterations() const { return iterations_; }

    void setDirection(PlaybackDirection dir) { direction_ = dir; }
    void setFill(FillMode fill) { fill_ = fill; }
    void setPlaybackRate(double rate) { playbackRate_ = rate; }
    double playbackRate() const { return playbackRate_; }

    // Playback control
    void play();
    void pause();
    void cancel();
    void finish();
    void reverse();

    PlayState playState() const { return playState_; }
    bool finished() const { return playState_ == PlayState::Finished; }

    // Timeline
    void setCurrentTime(double ms);
    double currentTime() const;

    // Called each frame by the orchestrator
    void tick(double timestamp);

    // Callbacks
    using FinishCallback = std::function<void()>;
    void onFinish(FinishCallback cb) { onFinish_ = std::move(cb); }

    uint32_t id() const { return id_; }

private:
    uint32_t id_;
    static uint32_t nextId_;

    std::shared_ptr<KeyframeEffect> effect_;

    double duration_ = 0;          // ms
    double delay_ = 0;
    double endDelay_ = 0;
    double iterations_ = 1;
    double playbackRate_ = 1;

    PlaybackDirection direction_ = PlaybackDirection::Normal;
    FillMode fill_ = FillMode::None;
    PlayState playState_ = PlayState::Idle;

    double startTime_ = -1;
    double holdTime_ = -1;
    double currentIteration_ = 0;

    FinishCallback onFinish_;

    // Compute the directed progress from the overall progress
    double computeActiveTime(double localTime) const;
    double computeOverallProgress(double activeTime) const;
    double computeDirectedProgress(double overallProgress, double currentIteration) const;
    double computeTransformedProgress(double directedProgress) const;
};

// ==================================================================
// AnimationTimeline — manages all active animations
// ==================================================================

class AnimationTimeline {
public:
    static AnimationTimeline& instance();

    void add(std::shared_ptr<Animation> anim);
    void remove(uint32_t id);

    // Tick all animations at the given timestamp
    void tick(double timestamp);

    // Remove finished animations
    void collectGarbage();

    size_t activeCount() const { return animations_.size(); }
    bool hasActiveAnimations() const { return !animations_.empty(); }

private:
    AnimationTimeline() = default;
    std::vector<std::shared_ptr<Animation>> animations_;
    double timestamp_ = 0;

public:
    double lastTimestamp() const { return timestamp_; }
};

// ==================================================================
// CSS Transition — generates Animation from property changes
// ==================================================================

struct CSSTransitionDef {
    std::string property;          // "all", "opacity", "transform", etc.
    double duration = 0;           // ms
    double delay = 0;
    TimingFunction easing;
};

class TransitionManager {
public:
    static TransitionManager& instance();

    // Parse CSS transition shorthand
    static std::vector<CSSTransitionDef> parseTransitions(const std::string& cssTransition);

    // Check if a property change should trigger a transition
    bool shouldTransition(BoxNode* node, const std::string& property,
                            const AnimatedValue& oldVal, const AnimatedValue& newVal);

    // Start a transition
    std::shared_ptr<Animation> startTransition(BoxNode* node, const std::string& property,
                                                  const AnimatedValue& from,
                                                  const AnimatedValue& to,
                                                  const CSSTransitionDef& def);

    // Cancel running transition for a property
    void cancelTransition(BoxNode* node, const std::string& property);

private:
    TransitionManager() = default;
    struct ActiveTransition {
        BoxNode* node;
        std::string property;
        std::shared_ptr<Animation> animation;
    };
    std::vector<ActiveTransition> active_;
};

} // namespace Web
} // namespace NXRender
