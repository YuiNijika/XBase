#pragma once

#include <XBase/Targeting.h>

#include <algorithm>
#include <cmath>

namespace XBase::Detail::TargetingBackend {

class Interaction {
public:
    enum class State { Idle, Focused, Locked, Operating };
    struct Result {
        bool selected = false;
        int action = 0;
        Targeting::Target target{};
    };

    Result Update(const Targeting::Target* candidate, bool middleDown, bool cancel,
        float dx, float dy, double time) {
        Result result;
        if (m_waitForRelease) {
            m_middleDown = middleDown;
            if (!middleDown) m_waitForRelease = false;
            return result;
        }
        const bool pressed = middleDown && !m_middleDown;
        const bool released = !middleDown && m_middleDown;
        m_middleDown = middleDown;
        if (cancel && IsLocked()) {
            result = {false, 3, m_target};
            Reset();
            return result;
        }
        if (pressed && candidate) {
            m_target = *candidate;
            m_started = time;
            m_x = m_y = 0.0f;
            m_action = 0;
            m_state = State::Locked;
            result = {true, 0, m_target};
        } else if (IsLocked()) {
            if (!candidate || candidate->kind != m_target.kind
                || candidate->id.value != m_target.id.value
                || candidate->modelId != m_target.modelId) {
                result = {false, 3, m_target};
                Reset();
                return result;
            }
            m_target = *candidate;
        } else if (!middleDown) {
            m_state = candidate ? State::Focused : State::Idle;
            m_target = candidate ? *candidate : Targeting::Target{};
        }
        if (IsLocked() && middleDown) {
            m_x = std::clamp(m_x + dx, -kPointerLimit, kPointerLimit);
            m_y = std::clamp(m_y + dy, -kPointerLimit, kPointerLimit);
            if (time - m_started >= kHoldSeconds) {
                m_state = State::Operating;
                m_action = 0;
                if (m_x * m_x + m_y * m_y >= kDeadZone * kDeadZone) {
                    float angle = std::atan2(m_y, m_x) + kPi * 0.5f;
                    if (angle < 0.0f) angle += kPi * 2.0f;
                    const int sector = static_cast<int>(
                        std::floor((angle + kPi / 6.0f) / (kPi / 3.0f))) % 6;
                    constexpr int actions[] = {1, 4, 5, 2, 6, 7};
                    m_action = actions[sector];
                }
            }
        }
        if (released && IsLocked()) {
            result = {false, m_state == State::Operating && m_action ? m_action : 3, m_target};
            m_state = State::Idle;
            m_target = {};
            m_action = 0;
            m_x = m_y = 0.0f;
        }
        return result;
    }

    void Reset() {
        m_state = State::Idle;
        m_target = {};
        m_waitForRelease = true;
        m_action = 0;
        m_x = m_y = 0.0f;
    }

    State GetState() const { return m_state; }
    bool IsLocked() const { return m_state == State::Locked || m_state == State::Operating; }
    bool HasFocus() const { return m_state != State::Idle; }
    const Targeting::Target& Focus() const { return m_target; }
    int Action() const { return m_action; }
    float PointerX() const { return m_x; }
    float PointerY() const { return m_y; }

private:
    static constexpr float kPi = 3.14159265359f;
    static constexpr double kHoldSeconds = 0.18;
    static constexpr float kDeadZone = 24.0f;
    static constexpr float kPointerLimit = 110.0f;
    State m_state = State::Idle;
    Targeting::Target m_target{};
    bool m_middleDown = false;
    bool m_waitForRelease = true;
    double m_started = 0.0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    int m_action = 0;
};

} // namespace XBase::Detail::TargetingBackend
