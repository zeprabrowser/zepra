// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "animation.h"
#include "web/box/box_tree.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace NXRender {
namespace Web {

// ==================================================================
// TimingFunction
// ==================================================================

TimingFunction TimingFunction::linear() {
    TimingFunction tf;
    tf.type_ = Type::Linear;
    return tf;
}

TimingFunction TimingFunction::ease() {
    return cubicBezier(0.25f, 0.1f, 0.25f, 1.0f);
}

TimingFunction TimingFunction::easeIn() {
    return cubicBezier(0.42f, 0.0f, 1.0f, 1.0f);
}

TimingFunction TimingFunction::easeOut() {
    return cubicBezier(0.0f, 0.0f, 0.58f, 1.0f);
}

TimingFunction TimingFunction::easeInOut() {
    return cubicBezier(0.42f, 0.0f, 0.58f, 1.0f);
}

TimingFunction TimingFunction::cubicBezier(float x1, float y1, float x2, float y2) {
    TimingFunction tf;
    tf.type_ = Type::CubicBezier;
    tf.x1_ = x1; tf.y1_ = y1;
    tf.x2_ = x2; tf.y2_ = y2;
    return tf;
}

TimingFunction TimingFunction::steps(int count, bool jumpStart) {
    TimingFunction tf;
    tf.type_ = Type::Steps;
    tf.stepCount_ = std::max(1, count);
    tf.stepJumpStart_ = jumpStart;
    return tf;
}

TimingFunction TimingFunction::parse(const std::string& cssValue) {
    if (cssValue == "linear") return linear();
    if (cssValue == "ease") return ease();
    if (cssValue == "ease-in") return easeIn();
    if (cssValue == "ease-out") return easeOut();
    if (cssValue == "ease-in-out") return easeInOut();
    if (cssValue == "step-start") return steps(1, true);
    if (cssValue == "step-end") return steps(1, false);

    // cubic-bezier(x1, y1, x2, y2)
    if (cssValue.find("cubic-bezier") != std::string::npos) {
        size_t start = cssValue.find('(');
        size_t end = cssValue.find(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string args = cssValue.substr(start + 1, end - start - 1);
            std::istringstream ss(args);
            float a, b, c, d;
            char comma;
            if (ss >> a >> comma >> b >> comma >> c >> comma >> d) {
                return cubicBezier(a, b, c, d);
            }
        }
    }

    // steps(n, jump-start|jump-end|...)
    if (cssValue.find("steps") != std::string::npos) {
        size_t start = cssValue.find('(');
        size_t end = cssValue.find(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string args = cssValue.substr(start + 1, end - start - 1);
            std::istringstream ss(args);
            int count;
            char comma;
            std::string direction;
            ss >> count;
            if (ss >> comma >> direction) {
                bool jump = (direction == "jump-start" || direction == "start");
                return steps(count, jump);
            }
            return steps(count, false);
        }
    }

    return ease(); // Default
}

float TimingFunction::evaluate(float t) const {
    t = std::clamp(t, 0.0f, 1.0f);

    switch (type_) {
        case Type::Linear:
            return t;

        case Type::Ease:
        case Type::EaseIn:
        case Type::EaseOut:
        case Type::EaseInOut:
        case Type::CubicBezier:
            return solveCubicBezier(t);

        case Type::Steps: {
            float step = 1.0f / stepCount_;
            if (stepJumpStart_) {
                return std::min(1.0f, std::ceil(t * stepCount_) * step);
            } else {
                return std::min(1.0f, std::floor(t * stepCount_) * step);
            }
        }

        default:
            return t;
    }
}

// Solve cubic bezier: find y for given x using Newton-Raphson
float TimingFunction::solveCubicBezier(float x) const {
    if (x <= 0) return 0;
    if (x >= 1) return 1;

    // Newton-Raphson to find t where bezierX(t) = x
    float t = x; // Initial guess
    for (int i = 0; i < 8; i++) {
        float bx = bezierSample(t);

        // Derivative of bezier X(t) = 3(1-t)²t·x1 + 3(1-t)t²·x2 + t³
        float dx = 3 * (1 - t) * (1 - t) * x1_
                  + 6 * (1 - t) * t * (x2_ - x1_)
                  + 3 * t * t * (1.0f - x2_);

        if (std::abs(dx) < 1e-6f) break;
        t -= (bx - x) / dx;
        t = std::clamp(t, 0.0f, 1.0f);
    }

    // Evaluate Y at solved t
    float u = 1 - t;
    return 3 * u * u * t * y1_ + 3 * u * t * t * y2_ + t * t * t;
}

float TimingFunction::bezierSample(float t) const {
    float u = 1 - t;
    return 3 * u * u * t * x1_ + 3 * u * t * t * x2_ + t * t * t;
}

// ==================================================================
// KeyframeEffect
// ==================================================================

KeyframeEffect::KeyframeEffect() {}
KeyframeEffect::~KeyframeEffect() {}

void KeyframeEffect::setKeyframes(const std::vector<Keyframe>& keyframes) {
    keyframes_ = keyframes;
    // Sort by offset
    std::sort(keyframes_.begin(), keyframes_.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
}

void KeyframeEffect::findKeyframePair(float progress, size_t& fromIdx, size_t& toIdx,
                                          float& localT) const {
    if (keyframes_.empty()) {
        fromIdx = toIdx = 0;
        localT = 0;
        return;
    }

    if (keyframes_.size() == 1) {
        fromIdx = toIdx = 0;
        localT = 0;
        return;
    }

    // Find the two keyframes that bracket the progress
    for (size_t i = 0; i < keyframes_.size() - 1; i++) {
        if (progress >= keyframes_[i].offset && progress <= keyframes_[i + 1].offset) {
            fromIdx = i;
            toIdx = i + 1;
            float range = keyframes_[i + 1].offset - keyframes_[i].offset;
            if (range > 0) {
                localT = (progress - keyframes_[i].offset) / range;
                // Apply the keyframe's easing to the local progress
                localT = keyframes_[i].easing.evaluate(localT);
            } else {
                localT = 0;
            }
            return;
        }
    }

    // Past the end
    fromIdx = toIdx = keyframes_.size() - 1;
    localT = 1;
}

AnimatedValue KeyframeEffect::interpolateValues(const AnimatedValue& from,
                                                    const AnimatedValue& to, float t) const {
    AnimatedValue result;
    result.type = from.type;

    switch (from.type) {
        case AnimatedValue::Type::Number:
        case AnimatedValue::Type::Length:
            result.number = from.number + (to.number - from.number) * t;
            break;

        case AnimatedValue::Type::Color: {
            // Component-wise RGBA interpolation
            uint8_t fr = (from.color >> 24) & 0xFF;
            uint8_t fg = (from.color >> 16) & 0xFF;
            uint8_t fb = (from.color >> 8) & 0xFF;
            uint8_t fa = from.color & 0xFF;

            uint8_t tr = (to.color >> 24) & 0xFF;
            uint8_t tg = (to.color >> 16) & 0xFF;
            uint8_t tb = (to.color >> 8) & 0xFF;
            uint8_t ta = to.color & 0xFF;

            uint8_t rr = static_cast<uint8_t>(fr + (tr - fr) * t);
            uint8_t rg = static_cast<uint8_t>(fg + (tg - fg) * t);
            uint8_t rb = static_cast<uint8_t>(fb + (tb - fb) * t);
            uint8_t ra = static_cast<uint8_t>(fa + (ta - fa) * t);

            result.color = (rr << 24) | (rg << 16) | (rb << 8) | ra;
            break;
        }

        case AnimatedValue::Type::Transform:
            result.transform = TransformMatrix::interpolate(from.transform, to.transform, t);
            break;

        case AnimatedValue::Type::String:
            // Discrete: use 'from' for t < 0.5, 'to' for t >= 0.5
            result.string = (t < 0.5f) ? from.string : to.string;
            break;

        default:
            break;
    }

    return result;
}

AnimatedValue KeyframeEffect::sample(float progress) const {
    if (keyframes_.empty()) return AnimatedValue{};

    size_t fromIdx, toIdx;
    float localT;
    findKeyframePair(progress, fromIdx, toIdx, localT);

    if (fromIdx == toIdx || keyframes_.size() < 2) {
        // Return the value at this keyframe
        auto it = keyframes_[fromIdx].properties.find(property_);
        if (it != keyframes_[fromIdx].properties.end()) return it->second;
        return AnimatedValue{};
    }

    auto fromIt = keyframes_[fromIdx].properties.find(property_);
    auto toIt = keyframes_[toIdx].properties.find(property_);

    if (fromIt == keyframes_[fromIdx].properties.end() ||
        toIt == keyframes_[toIdx].properties.end()) {
        return AnimatedValue{};
    }

    return interpolateValues(fromIt->second, toIt->second, localT);
}

void KeyframeEffect::apply(float progress) {
    if (!target_) return;

    auto value = sample(progress);
    if (value.type == AnimatedValue::Type::None) return;

    applyToStyle(target_->computed(), property_, value);
}

void KeyframeEffect::applyToStyle(ComputedValues& cv, const std::string& prop,
                                      const AnimatedValue& value) {
    if (prop == "opacity") {
        cv.opacity = std::clamp(value.number, 0.0f, 1.0f);
    } else if (prop == "transform") {
        // Serialize the animated TransformMatrix into CSS matrix() notation
        // for the paint pipeline. The 2D subset uses 6 values: a,b,c,d,tx,ty
        const auto& m = value.transform;
        char buf[256];
        snprintf(buf, sizeof(buf), "matrix(%.6g,%.6g,%.6g,%.6g,%.6g,%.6g)",
                 m.m[0][0], m.m[1][0], m.m[0][1], m.m[1][1], m.m[3][0], m.m[3][1]);
        cv.transform = buf;
    } else if (prop == "width") {
        cv.width = std::max(0.0f, value.number);
        cv.widthAuto = false;
    } else if (prop == "height") {
        cv.height = std::max(0.0f, value.number);
        cv.heightAuto = false;
    } else if (prop == "top") {
        cv.top = value.number;
        cv.topAuto = false;
    } else if (prop == "left") {
        cv.left = value.number;
        cv.leftAuto = false;
    } else if (prop == "right") {
        cv.right = value.number;
        cv.rightAuto = false;
    } else if (prop == "bottom") {
        cv.bottom = value.number;
        cv.bottomAuto = false;
    } else if (prop == "margin-top") {
        cv.marginTop = value.number;
    } else if (prop == "margin-bottom") {
        cv.marginBottom = value.number;
    } else if (prop == "margin-left") {
        cv.marginLeft = value.number;
    } else if (prop == "margin-right") {
        cv.marginRight = value.number;
    } else if (prop == "padding-top") {
        cv.paddingTop = value.number;
    } else if (prop == "padding-bottom") {
        cv.paddingBottom = value.number;
    } else if (prop == "padding-left") {
        cv.paddingLeft = value.number;
    } else if (prop == "padding-right") {
        cv.paddingRight = value.number;
    } else if (prop == "color") {
        cv.color = value.color;
    } else if (prop == "background-color") {
        cv.backgroundColor = value.color;
    } else if (prop == "border-top-color") {
        cv.borderTopColor = value.color;
    } else if (prop == "border-right-color") {
        cv.borderRightColor = value.color;
    } else if (prop == "border-bottom-color") {
        cv.borderBottomColor = value.color;
    } else if (prop == "border-left-color") {
        cv.borderLeftColor = value.color;
    } else if (prop == "border-top-width") {
        cv.borderTopWidth = value.number;
    } else if (prop == "border-right-width") {
        cv.borderRightWidth = value.number;
    } else if (prop == "border-bottom-width") {
        cv.borderBottomWidth = value.number;
    } else if (prop == "border-left-width") {
        cv.borderLeftWidth = value.number;
    } else if (prop == "font-size") {
        cv.fontSize = std::max(0.0f, value.number);
    } else if (prop == "letter-spacing") {
        cv.letterSpacing = value.number;
    } else if (prop == "word-spacing") {
        cv.wordSpacing = value.number;
    } else if (prop == "border-top-left-radius") {
        cv.borderTopLeftRadius = value.number;
    } else if (prop == "border-top-right-radius") {
        cv.borderTopRightRadius = value.number;
    } else if (prop == "border-bottom-right-radius") {
        cv.borderBottomRightRadius = value.number;
    } else if (prop == "border-bottom-left-radius") {
        cv.borderBottomLeftRadius = value.number;
    } else if (prop == "flex-grow") {
        cv.flexGrow = value.number;
    } else if (prop == "flex-shrink") {
        cv.flexShrink = value.number;
    } else if (prop == "z-index") {
        cv.zIndex = static_cast<int>(value.number);
        cv.zIndexAuto = false;
    }
}

// ==================================================================
// Animation timing model
// ==================================================================

uint32_t Animation::nextId_ = 1;

Animation::Animation() : id_(nextId_++) {}
Animation::~Animation() {}

void Animation::play() {
    if (playState_ == PlayState::Paused) {
        // Resume from hold time
        playState_ = PlayState::Pending;
    } else {
        startTime_ = -1;
        holdTime_ = 0;
        playState_ = PlayState::Pending;
    }
}

void Animation::pause() {
    if (playState_ == PlayState::Running) {
        holdTime_ = currentTime();
        playState_ = PlayState::Paused;
    }
}

void Animation::cancel() {
    playState_ = PlayState::Idle;
    startTime_ = -1;
    holdTime_ = -1;
    currentIteration_ = 0;
}

void Animation::finish() {
    if (duration_ <= 0) return;

    double endTime = delay_ + duration_ * iterations_ + endDelay_;
    setCurrentTime(playbackRate_ >= 0 ? endTime : 0);
    playState_ = PlayState::Finished;

    if (onFinish_) onFinish_();
}

void Animation::reverse() {
    playbackRate_ = -playbackRate_;
    play();
}

void Animation::setCurrentTime(double ms) {
    holdTime_ = ms;
    if (playState_ == PlayState::Idle) {
        playState_ = PlayState::Paused;
    }
}

double Animation::currentTime() const {
    if (holdTime_ >= 0) return holdTime_;
    if (startTime_ < 0) return 0;
    return 0; // Would use timeline.currentTime - startTime
}

void Animation::tick(double timestamp) {
    if (playState_ == PlayState::Idle || playState_ == PlayState::Finished) return;

    if (playState_ == PlayState::Pending) {
        startTime_ = timestamp - (holdTime_ >= 0 ? holdTime_ : 0);
        holdTime_ = -1;
        playState_ = PlayState::Running;
    }

    if (playState_ != PlayState::Running) return;

    // Compute local time
    double localTime = (timestamp - startTime_) * playbackRate_;

    // Active time
    double activeTime = computeActiveTime(localTime);

    if (activeTime < 0) return; // Before active interval

    // Overall progress
    double overallProgress = computeOverallProgress(activeTime);

    // Current iteration
    if (duration_ > 0) {
        currentIteration_ = std::floor(activeTime / duration_);
        if (currentIteration_ >= iterations_ && iterations_ > 0) {
            currentIteration_ = iterations_ - 1;
        }
    }

    // Directed progress
    double directed = computeDirectedProgress(overallProgress, currentIteration_);

    // Apply to effect
    if (effect_) {
        effect_->apply(static_cast<float>(directed));
    }

    // Check if finished
    double activeDuration = duration_ * iterations_;
    if (activeTime >= activeDuration && activeDuration > 0) {
        // Fill check
        if (fill_ == FillMode::Forwards || fill_ == FillMode::Both) {
            if (effect_) {
                float finalProgress = static_cast<float>(
                    computeDirectedProgress(1.0, iterations_ - 1));
                effect_->apply(finalProgress);
            }
        }
        playState_ = PlayState::Finished;
        if (onFinish_) onFinish_();
    }
}

double Animation::computeActiveTime(double localTime) const {
    // Before delay
    if (localTime < delay_) {
        if (fill_ == FillMode::Backwards || fill_ == FillMode::Both) {
            return 0; // Fill backwards
        }
        return -1; // Inactive
    }

    double activeDuration = duration_ * iterations_;
    double activeTime = localTime - delay_;

    // After active interval
    if (activeTime > activeDuration && activeDuration > 0) {
        if (fill_ == FillMode::Forwards || fill_ == FillMode::Both) {
            return activeDuration;
        }
        return -1;
    }

    return activeTime;
}

double Animation::computeOverallProgress(double activeTime) const {
    if (duration_ <= 0) return 0;
    return activeTime / duration_;
}

double Animation::computeDirectedProgress(double overallProgress, double iteration) const {
    // Simple iteration progress
    double iterProgress = std::fmod(overallProgress, 1.0);
    if (overallProgress > 0 && iterProgress == 0 && iteration > 0) {
        iterProgress = 1.0;
    }

    bool forward = true;
    switch (direction_) {
        case PlaybackDirection::Normal:
            forward = true;
            break;
        case PlaybackDirection::Reverse:
            forward = false;
            break;
        case PlaybackDirection::Alternate:
            forward = (static_cast<int>(iteration) % 2 == 0);
            break;
        case PlaybackDirection::AlternateReverse:
            forward = (static_cast<int>(iteration) % 2 != 0);
            break;
    }

    return forward ? iterProgress : (1.0 - iterProgress);
}

double Animation::computeTransformedProgress(double directedProgress) const {
    // Apply timing function via the effect's keyframe easing
    return directedProgress;
}

// ==================================================================
// AnimationTimeline
// ==================================================================

AnimationTimeline& AnimationTimeline::instance() {
    static AnimationTimeline inst;
    return inst;
}

void AnimationTimeline::add(std::shared_ptr<Animation> anim) {
    animations_.push_back(std::move(anim));
}

void AnimationTimeline::remove(uint32_t id) {
    animations_.erase(
        std::remove_if(animations_.begin(), animations_.end(),
                        [id](const std::shared_ptr<Animation>& a) { return a->id() == id; }),
        animations_.end());
}

void AnimationTimeline::tick(double timestamp) {
    for (auto& anim : animations_) {
        anim->tick(timestamp);
    }
}

void AnimationTimeline::collectGarbage() {
    animations_.erase(
        std::remove_if(animations_.begin(), animations_.end(),
                        [](const std::shared_ptr<Animation>& a) { return a->finished(); }),
        animations_.end());
}

// ==================================================================
// TransitionManager
// ==================================================================

TransitionManager& TransitionManager::instance() {
    static TransitionManager inst;
    return inst;
}

std::vector<CSSTransitionDef> TransitionManager::parseTransitions(const std::string& css) {
    std::vector<CSSTransitionDef> defs;
    if (css.empty() || css == "none") return defs;

    // Split on comma for multiple transitions
    std::istringstream stream(css);
    std::string segment;
    while (std::getline(stream, segment, ',')) {
        // Trim
        while (!segment.empty() && std::isspace(segment.front())) segment.erase(segment.begin());
        while (!segment.empty() && std::isspace(segment.back())) segment.pop_back();

        CSSTransitionDef def;
        std::istringstream ss(segment);
        std::string token;

        // property
        if (ss >> token) def.property = token;

        // duration
        if (ss >> token) {
            def.duration = std::strtof(token.c_str(), nullptr);
            if (token.back() == 's' && token.find("ms") == std::string::npos) {
                def.duration *= 1000; // seconds to ms
            }
        }

        // easing
        if (ss >> token) {
            def.easing = TimingFunction::parse(token);
        } else {
            def.easing = TimingFunction::ease();
        }

        // delay
        if (ss >> token) {
            def.delay = std::strtof(token.c_str(), nullptr);
            if (token.back() == 's' && token.find("ms") == std::string::npos) {
                def.delay *= 1000;
            }
        }

        defs.push_back(def);
    }

    return defs;
}

bool TransitionManager::shouldTransition(BoxNode* /*node*/, const std::string& /*property*/,
                                              const AnimatedValue& oldVal,
                                              const AnimatedValue& newVal) {
    if (oldVal.type != newVal.type) return false;
    if (oldVal.type == AnimatedValue::Type::None) return false;

    // Values must be different
    switch (oldVal.type) {
        case AnimatedValue::Type::Number:
        case AnimatedValue::Type::Length:
            return std::abs(oldVal.number - newVal.number) > 0.01f;
        case AnimatedValue::Type::Color:
            return oldVal.color != newVal.color;
        case AnimatedValue::Type::Transform:
            // Always transition if transforms differ
            return true;
        default:
            return false;
    }
}

std::shared_ptr<Animation> TransitionManager::startTransition(
    BoxNode* node, const std::string& property,
    const AnimatedValue& from, const AnimatedValue& to,
    const CSSTransitionDef& def) {

    cancelTransition(node, property);

    auto effect = std::make_shared<KeyframeEffect>();
    effect->setTarget(node);
    effect->setProperty(property);

    Keyframe kf0;
    kf0.offset = 0;
    kf0.easing = def.easing;
    kf0.properties[property] = from;

    Keyframe kf1;
    kf1.offset = 1;
    kf1.properties[property] = to;

    effect->setKeyframes({kf0, kf1});

    auto anim = std::make_shared<Animation>();
    anim->setEffect(effect);
    anim->setDuration(def.duration);
    anim->setDelay(def.delay);
    anim->setFill(Animation::FillMode::Both);
    anim->play();

    AnimationTimeline::instance().add(anim);

    ActiveTransition at;
    at.node = node;
    at.property = property;
    at.animation = anim;
    active_.push_back(at);

    return anim;
}

void TransitionManager::cancelTransition(BoxNode* node, const std::string& property) {
    active_.erase(
        std::remove_if(active_.begin(), active_.end(),
                        [node, &property](const ActiveTransition& t) {
                            if (t.node == node && t.property == property) {
                                AnimationTimeline::instance().remove(t.animation->id());
                                return true;
                            }
                            return false;
                        }),
        active_.end());
}

} // namespace Web
} // namespace NXRender
