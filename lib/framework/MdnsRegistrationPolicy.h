#pragma once

#include <cstddef>

template <typename RegisterHandler, typename IsConnected, typename Announce>
inline void registerMdnsStaGotIp(RegisterHandler registerHandler, IsConnected isConnected, Announce announce)
{
    registerHandler(announce);
    if (isConnected())
        announce();
}

template <typename EventAction, typename Send>
inline void maintainMdnsIp4(EventAction enable, EventAction announce, Send send)
{
    send(static_cast<EventAction>(enable | announce));
}

inline bool mdnsShouldStart(bool alreadyStarted, bool safeMode, bool wifiConnected, size_t largestInternal, size_t minInternal)
{
    return !alreadyStarted && !safeMode && wifiConnected && largestInternal >= minInternal;
}

// First-boot begin() may run before STA has an address. Still refuse if DMA is gone.
inline bool mdnsShouldStartAtBoot(bool alreadyStarted, bool safeMode, size_t largestInternal, size_t minInternal)
{
    return !alreadyStarted && !safeMode && largestInternal >= minInternal;
}

inline bool mdnsShouldAnnounce(bool alreadyStarted, bool safeMode, bool wifiConnected, size_t largestInternal = SIZE_MAX,
                               size_t minInternal = 0)
{
    return alreadyStarted && !safeMode && wifiConnected && largestInternal >= minInternal;
}

inline bool mdnsAnnounceSucceeded(int enableResult, int announceResult)
{
    return enableResult == 0 && announceResult == 0;
}

// Started but never announced, or no successful announce within staleMs — restart the responder.
inline bool mdnsShouldRestart(bool alreadyStarted, bool safeMode, bool wifiConnected, size_t largestInternal,
                              uint32_t now, uint32_t lastAnnounceOk, uint32_t staleMs, size_t minRestartInternal)
{
    if (!alreadyStarted || safeMode || !wifiConnected || largestInternal < minRestartInternal)
        return false;
    if (lastAnnounceOk == 0)
        return true;
    return (now - lastAnnounceOk) > staleMs;
}

inline uint32_t mdnsMaintainIntervalMs(bool alreadyStarted, uint32_t now, uint32_t lastAnnounceOk,
                                       uint32_t healthyIntervalMs, uint32_t recoveryIntervalMs, uint32_t staleMs)
{
    if (!alreadyStarted)
        return recoveryIntervalMs;
    if (lastAnnounceOk == 0 || (now - lastAnnounceOk) > staleMs)
        return recoveryIntervalMs;
    return healthyIntervalMs;
}
