#pragma once

#include <cstdint>

enum class MdnsMaintainAction : uint8_t
{
    None,
    CheckStart,
    RecoverInterface,
    RefreshHttpService,
};

struct MdnsMaintainState
{
    bool sawConnected = false;
    // Remaining best-effort API calls, not confirmed announcements. The
    // installed ESPmDNS can return ESP_OK after dropping its queue send.
    uint8_t announceAttempts = 0;
    uint32_t lastMaintain = 0;
    bool httpServiceApiOk = true;
};

struct MdnsMaintainDecision
{
    MdnsMaintainState state;
    MdnsMaintainAction action = MdnsMaintainAction::None;
    bool checkHostname = false;
};

inline MdnsMaintainDecision mdnsMaintainTransition(MdnsMaintainState state, bool wifiConnected, bool mdnsStarted,
                                                    uint32_t now)
{
    MdnsMaintainDecision decision{state};
    if (!wifiConnected)
    {
        decision.state.sawConnected = false;
        return decision;
    }

    constexpr uint32_t kAnnounceRetryInterval = 1000UL;
    constexpr uint8_t kAnnounceAttempts = 3;
    constexpr uint32_t kRefreshInterval = 60000UL;
    constexpr uint32_t kRecoveryInterval = 15000UL;

    if (!mdnsStarted)
    {
        if (state.lastMaintain && static_cast<uint32_t>(now - state.lastMaintain) < kRecoveryInterval)
            return decision;
        decision.state.lastMaintain = now;
        decision.action = MdnsMaintainAction::CheckStart;
        return decision;
    }

    if (!state.sawConnected)
    {
        decision.state.sawConnected = true;
        decision.state.announceAttempts = kAnnounceAttempts;
        decision.state.lastMaintain = now - kAnnounceRetryInterval;
    }

    if (decision.state.announceAttempts == 0)
    {
        const uint32_t interval = decision.state.httpServiceApiOk ? kRefreshInterval : kAnnounceRetryInterval;
        if (static_cast<uint32_t>(now - decision.state.lastMaintain) < interval)
            return decision;
        decision.state.lastMaintain = now;
        decision.action = MdnsMaintainAction::RefreshHttpService;
        decision.checkHostname = true;
        return decision;
    }

    if (static_cast<uint32_t>(now - decision.state.lastMaintain) < kAnnounceRetryInterval)
        return decision;

    decision.action = MdnsMaintainAction::RecoverInterface;
    decision.checkHostname = decision.state.announceAttempts == kAnnounceAttempts;
    return decision;
}

// Bound best-effort API calls without claiming that ESPmDNS queued or sent an
// announcement. Definite synchronous failures retain the remaining call budget.
inline MdnsMaintainState mdnsMaintainComplete(MdnsMaintainState state, uint32_t now, bool apiAccepted = true)
{
    if (apiAccepted && state.announceAttempts > 0)
        --state.announceAttempts;
    state.lastMaintain = now;
    return state;
}

inline MdnsMaintainState mdnsMaintainRetryLater(MdnsMaintainState state, uint32_t now)
{
    state.lastMaintain = now;
    return state;
}

inline MdnsMaintainState mdnsHttpRefreshResult(MdnsMaintainState state, uint32_t now, bool apiOk)
{
    state.httpServiceApiOk = apiOk;
    state.lastMaintain = now;
    return state;
}
