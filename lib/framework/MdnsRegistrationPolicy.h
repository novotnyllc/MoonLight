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
