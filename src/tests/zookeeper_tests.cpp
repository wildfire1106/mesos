/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <zookeeper.h>

#include <gmock/gmock.h>

#include <string>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/strings.hpp>

#include "zookeeper/authentication.hpp"
#include "zookeeper/contender.hpp"
#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"

#include "tests/zookeeper.hpp"

using namespace mesos::internal;
using namespace mesos::internal::tests;
using namespace process;
using namespace zookeeper;


TEST_F(ZooKeeperTest, Auth)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  authenticatedZk.create("/test",
                         "42",
                         zookeeper::EVERYONE_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("42", &authenticatedZk, "/test");

  ZooKeeper unauthenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  ASSERT_ZK_GET("42", &unauthenticatedZk, "/test");
  ASSERT_EQ(ZNOAUTH, unauthenticatedZk.set("/test", "", -1));

  ZooKeeper nonOwnerZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  ASSERT_ZK_GET("42", &nonOwnerZk, "/test");
  ASSERT_EQ(ZNOAUTH, nonOwnerZk.set("/test", "", -1));
}


TEST_F(ZooKeeperTest, SessionTimeoutNegotiation)
{
  server->setMinSessionTimeout(Seconds(8));
  server->setMaxSessionTimeout(Seconds(20));
  EXPECT_EQ(Seconds(8), server->getMinSessionTimeout());
  EXPECT_EQ(Seconds(20), server->getMaxSessionTimeout());

  ZooKeeperTest::TestWatcher watcher;
  ZooKeeper zk1(server->connectString(), Seconds(7), &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);

  // The requested timeout is less than server's min value so the
  // negotiated result is the sever's min value.
  EXPECT_EQ(Seconds(8), zk1.getSessionTimeout());

  ZooKeeper zk2(server->connectString(), Seconds(22), &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);

  // The requested timeout is greater than server's max value so the
  // negotiated result is the sever's max value.
  EXPECT_EQ(Seconds(20), zk2.getSessionTimeout());
}


TEST_F(ZooKeeperTest, Create)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  EXPECT_EQ(ZOK, authenticatedZk.create("/foo/bar",
                                        "",
                                        zookeeper::EVERYONE_READ_CREATOR_ALL,
                                        0,
                                        NULL,
                                        true));
  authenticatedZk.create("/foo/bar/baz",
                         "43",
                         zookeeper::EVERYONE_CREATE_AND_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("43", &authenticatedZk, "/foo/bar/baz");

  ZooKeeper nonOwnerZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  EXPECT_EQ(ZNODEEXISTS, nonOwnerZk.create("/foo/bar/baz",
                                           "",
                                           zookeeper::EVERYONE_READ_CREATOR_ALL,
                                           0,
                                           NULL,
                                           true));
  EXPECT_EQ(ZOK, nonOwnerZk.create("/foo/bar/baz/bam",
                                   "44",
                                   zookeeper::EVERYONE_READ_CREATOR_ALL,
                                   0,
                                   NULL,
                                   true));
  ASSERT_ZK_GET("44", &nonOwnerZk, "/foo/bar/baz/bam");

  std::string result;
  EXPECT_EQ(ZOK, nonOwnerZk.create("/foo/bar/baz/",
                                   "",
                                   zookeeper::EVERYONE_READ_CREATOR_ALL,
                                   ZOO_SEQUENCE | ZOO_EPHEMERAL,
                                   &result,
                                   true));
  EXPECT_TRUE(strings::startsWith(result, "/foo/bar/baz/0"));
}


TEST_F(ZooKeeperTest, LeaderDetector)
{
  Group group(server->connectString(), NO_TIMEOUT, "/test/");

  // Initialize two members.
  Future<Group::Membership> membership1 =
    group.join("member 1");
  AWAIT_READY(membership1);
  Future<Group::Membership> membership2 =
    group.join("member 2");
  AWAIT_READY(membership2);

  LeaderDetector detector(&group);

  // Detect the leader.
  Future<Option<Group::Membership> > leader =
    detector.detect(None());
  AWAIT_READY(leader);
  ASSERT_SOME_EQ(membership1.get(), leader.get());

  // Detect next leader change.
  leader = detector.detect(leader.get());
  EXPECT_TRUE(leader.isPending());

  // Leader doesn't change after cancelling the follower.
  Future<bool> cancellation = group.cancel(membership2.get());
  AWAIT_READY(cancellation);
  EXPECT_TRUE(cancellation.get());
  EXPECT_TRUE(leader.isPending());

  // Join member 2 back.
  membership2 = group.join("member 2");
  AWAIT_READY(membership2);
  EXPECT_TRUE(leader.isPending());

  // Cancelling the incumbent leader allows member 2 to be elected.
  cancellation = group.cancel(membership1.get());
  AWAIT_READY(cancellation);
  EXPECT_TRUE(cancellation.get());
  AWAIT_READY(leader);
  EXPECT_SOME_EQ(membership2.get(), leader.get());

  // Cancelling the only member results in no leader elected.
  leader = detector.detect(leader.get().get());
  EXPECT_TRUE(leader.isPending());
  cancellation = group.cancel(membership2.get());

  AWAIT_READY(cancellation);
  EXPECT_TRUE(cancellation.get());
  AWAIT_READY(leader);
  ASSERT_TRUE(leader.get().isNone());
}


TEST_F(ZooKeeperTest, LeaderDetectorTimeoutHandling)
{
  Seconds timeout(10);
  Group group(server->connectString(), timeout, "/test/");
  LeaderDetector detector(&group);

  Future<Group::Membership> membership1 = group.join("member 1");
  AWAIT_READY(membership1);
  Future<bool> cancelled = membership1.get().cancelled();

  Future<Option<Group::Membership> > leader = detector.detect();

  AWAIT_READY(leader);
  EXPECT_SOME(leader.get());

  leader = detector.detect(leader.get());

  server->shutdownNetwork();

  Clock::pause();

  // We may need to advance multiple times because we could have
  // advanced the clock before the timer in Group starts.
  while (cancelled.isPending()) {
    Clock::advance(timeout);
    Clock::settle();
  }
  Clock::resume();

  // The detect operation times out but the group internally
  // recreates a new ZooKeeper client and hides the error from the
  // detector.
  EXPECT_TRUE(leader.isPending());

  Future<Nothing> connected = FUTURE_DISPATCH(
      group.process->self(),
      &GroupProcess::connected);
  server->startNetwork();

  // When the service is restored, all sessions/memberships are gone.
  AWAIT_READY(connected);
  AWAIT_READY(leader);
  EXPECT_TRUE(leader.get().isNone());

  AWAIT_READY(group.join("member 1"));

  leader = detector.detect(leader.get());
  AWAIT_READY(leader);
  EXPECT_SOME(leader.get());

  // Cancel the member and join another.
  AWAIT_READY(group.cancel(leader.get().get()));
  leader = detector.detect(leader.get());
  AWAIT_READY(leader);
  EXPECT_TRUE(leader.get().isNone());

  AWAIT_READY(group.join("member 2"));

  // Detect a new leader.
  leader = detector.detect(leader.get());
  AWAIT_READY(leader);
  EXPECT_SOME(leader.get());
}


TEST_F(ZooKeeperTest, LeaderContender)
{
  Seconds timeout(10);
  Group group(server->connectString(), timeout, "/test/");

  Owned<LeaderContender> contender(
      new LeaderContender(&group, "candidate 1"));

  // Calling withdraw before contending returns 'false' because there
  // is nothing to withdraw.
  Future<bool> withdrawn = contender->withdraw();
  AWAIT_READY(withdrawn);
  EXPECT_FALSE(withdrawn.get());

  contender->contend();

  // Immediately withdrawing after contending leads to delayed
  // cancellation.
  withdrawn = contender->withdraw();
  AWAIT_READY(withdrawn);
  EXPECT_TRUE(withdrawn.get());

  // Normal workflow.
  contender = Owned<LeaderContender>(
      new LeaderContender(&group, "candidate 1"));

  Future<Future<Nothing> > candidated = contender->contend();
  AWAIT_READY(candidated);

  Future<Nothing> lostCandidacy = candidated.get();
  EXPECT_TRUE(lostCandidacy.isPending());

  // Expire the Group session while we are watching for updates from
  // the contender and the candidacy will be lost.
  Future<Option<int64_t> > session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());

  Future<Nothing> connected = FUTURE_DISPATCH(
      group.process->self(),
      &GroupProcess::connected);
  server->expireSession(session.get().get());
  AWAIT_READY(lostCandidacy);

  // Withdraw directly returns because candidacy is lost and there
  // is nothing to cancel.
  withdrawn = contender->withdraw();
  AWAIT_READY(withdrawn);
  EXPECT_FALSE(withdrawn.get());

  // Contend again.
  contender = Owned<LeaderContender>(
      new LeaderContender(&group, "candidate 1"));
  candidated = contender->contend();

  AWAIT_READY(connected);
  session = group.session();
  AWAIT_READY(session);
  ASSERT_SOME(session.get());

  server->expireSession(session.get().get());

  Clock::pause();
  // The retry timeout.
  Clock::advance(GroupProcess::RETRY_INTERVAL);
  Clock::settle();
  Clock::resume();

  // The contender weathered the expiration and succeeded in a retry.
  AWAIT_READY(candidated);

  withdrawn = contender->withdraw();
  AWAIT_READY(withdrawn);

  // Contend (3) and shutdown the network this time.
  contender = Owned<LeaderContender>(
      new LeaderContender(&group, "candidate 1"));
  candidated = contender->contend();
  AWAIT_READY(candidated);
  lostCandidacy = candidated.get();

  server->shutdownNetwork();

  Clock::pause();

  // We may need to advance multiple times because we could have
  // advanced the clock before the timer in Group starts.
  while (lostCandidacy.isPending()) {
    Clock::advance(timeout);
    Clock::settle();
  }

  // Server failure results in candidacy loss.
  AWAIT_READY(lostCandidacy);

  Clock::resume();

  server->startNetwork();

  // Contend again (4).
  contender = Owned<LeaderContender>(
      new LeaderContender(&group, "candidate 1"));
  candidated = contender->contend();
  AWAIT_READY(candidated);
}
