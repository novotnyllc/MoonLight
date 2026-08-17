#pragma once

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
