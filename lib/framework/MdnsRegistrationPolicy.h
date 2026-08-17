#pragma once

template <typename RegisterHandler, typename IsConnected, typename Announce>
inline void registerMdnsStaGotIp(RegisterHandler registerHandler, IsConnected isConnected, Announce announce)
{
    registerHandler(announce);
    if (isConnected())
        announce();
}
