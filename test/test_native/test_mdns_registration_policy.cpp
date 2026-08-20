/**
    @title     MoonLight Unit Tests — mDNS registration policy
    @file      test_mdns_registration_policy.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Focused native tests for the pure mDNS maintenance state machine.
    Run with: pio test -e native
**/

#include "doctest.h"

#include "MdnsRegistrationPolicy.h"

TEST_CASE("mDNS missing STA netif retains recovery calls without a 20 ms spin") {
  const MdnsMaintainDecision reconnect =
      mdnsMaintainTransition({false, 0, 0, true}, true, true, 2000);
  REQUIRE(reconnect.action == MdnsMaintainAction::RecoverInterface);
  REQUIRE_EQ(reconnect.state.announceAttempts, 3);

  const MdnsMaintainState deferred = mdnsMaintainRetryLater(reconnect.state, 2000);
  CHECK_EQ(deferred.announceAttempts, 3);
  CHECK(mdnsMaintainTransition(deferred, true, true, 2019).action == MdnsMaintainAction::None);
  CHECK(mdnsMaintainTransition(deferred, true, true, 2999).action == MdnsMaintainAction::None);
  CHECK(mdnsMaintainTransition(deferred, true, true, 3000).action == MdnsMaintainAction::RecoverInterface);
}

TEST_CASE("mDNS definite recovery API failure retains the bounded call budget") {
  const MdnsMaintainState failed = mdnsMaintainComplete({true, 3, 1000, true}, 2000, false);
  CHECK_EQ(failed.announceAttempts, 3);
  CHECK_EQ(failed.lastMaintain, 2000);
  CHECK(mdnsMaintainTransition(failed, true, true, 2999).action == MdnsMaintainAction::None);

  const MdnsMaintainState nominal = mdnsMaintainComplete(failed, 3000, true);
  CHECK_EQ(nominal.announceAttempts, 2);
  CHECK_EQ(nominal.lastMaintain, 3000);
}

TEST_CASE("mDNS successful HTTP refresh stays exactly sixty seconds apart") {
  const MdnsMaintainDecision due =
      mdnsMaintainTransition({true, 0, 4000, true}, true, true, 64000);
  REQUIRE(due.action == MdnsMaintainAction::RefreshHttpService);

  const MdnsMaintainState refreshed = mdnsHttpRefreshResult(due.state, 64000, true);
  CHECK(refreshed.httpServiceApiOk);
  CHECK(mdnsMaintainTransition(refreshed, true, true, 123999).action == MdnsMaintainAction::None);
  CHECK(mdnsMaintainTransition(refreshed, true, true, 124000).action == MdnsMaintainAction::RefreshHttpService);
}

TEST_CASE("mDNS HTTP refresh failure is observable and retried after one second") {
  const MdnsMaintainDecision due =
      mdnsMaintainTransition({true, 0, 4000, true}, true, true, 64000);
  REQUIRE(due.action == MdnsMaintainAction::RefreshHttpService);

  const MdnsMaintainState failed = mdnsHttpRefreshResult(due.state, 64000, false);
  CHECK_FALSE(failed.httpServiceApiOk);
  CHECK(mdnsMaintainTransition(failed, true, true, 64999).action == MdnsMaintainAction::None);
  CHECK(mdnsMaintainTransition(failed, true, true, 65000).action == MdnsMaintainAction::RefreshHttpService);
}
