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

#include <gmock/gmock.h>

#include <mesos/master/allocator.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include <process/metrics/metrics.hpp>

#include "master/flags.hpp"
#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace mesos::internal::master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using process::metrics::internal::MetricsProcess;

using process::Clock;
using process::Future;
using process::PID;

using std::string;

using testing::_;
using testing::Eq;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


// This test case covers tests related to framework API rate limiting
// which includes metrics exporting for API call rates.
class RateLimitingTest : public MesosTest
{
public:
  virtual master::Flags CreateMasterFlags()
  {
    master::Flags flags = MesosTest::CreateMasterFlags();

    RateLimits limits;
    RateLimit* limit = limits.mutable_limits()->Add();
    limit->set_principal(DEFAULT_CREDENTIAL.principal());
    // Set 1qps so that the half-second Clock::advance()s for
    // metrics endpoint (because it also throttles requests but at
    // 2qps) don't mess with framework rate limiting.
    limit->set_qps(1);
    flags.rate_limits = limits;

    return flags;
  }
};


// Verify that message counters for a framework are added when a
// framework registers, removed when it terminates and count messages
// correctly when it is given unlimited rate.
TEST_F(RateLimitingTest, NoRateLimiting)
{
  // Give the framework unlimited rate explicitly by specifying a
  // RateLimit entry without 'qps'
  master::Flags flags = CreateMasterFlags();
  RateLimits limits;
  RateLimit* limit = limits.mutable_limits()->Add();
  limit->set_principal(DEFAULT_CREDENTIAL.principal());
  flags.rate_limits = limits;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  Clock::pause();

  // Settle to make sure master is ready for incoming requests, i.e.,
  // '_recover()' completes.
  Clock::settle();

  // Advance before the test so that the 1st call to Metrics endpoint
  // is not throttled. MetricsProcess which hosts the endpoint
  // throttles requests at 2qps and its singleton instance is shared
  // across tests.
  Clock::advance(Milliseconds(501));

  // Message counters not present before the framework registers.
  {
    JSON::Object metrics = Metrics();

    EXPECT_EQ(
        0u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_received"));

    EXPECT_EQ(
        0u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_processed"));
  }

  MockScheduler sched;
  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver = new MesosSchedulerDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(driver, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call.
  Future<mesos::scheduler::Call> subscribeCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver->start());

  AWAIT_READY(subscribeCall);
  AWAIT_READY(frameworkRegisteredMessage);

  const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

  // For metrics endpoint.
  Clock::advance(Milliseconds(501));

  // Send a duplicate subscribe call. Master sends
  // FrameworkRegisteredMessage back after processing it.
  {
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), subscribeCall.get());
    AWAIT_READY(duplicateFrameworkRegisteredMessage);

    // Verify that one message is received and processed (after
    // registration).
    JSON::Object metrics = Metrics();

    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(1, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  Future<Nothing> removeFramework =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::removeFramework);

  driver->stop();
  driver->join();
  delete driver;

  // The fact that the teardown call (the 2nd call from the scheduler
  // that reaches Master after its registration) gets processed
  // without Clock advances proves that the framework is given
  // unlimited rate.
  AWAIT_READY(removeFramework);

  // For metrics endpoint.
  Clock::advance(Milliseconds(501));

  // Message counters removed after the framework is unregistered.
  {
    JSON::Object metrics = Metrics();

    EXPECT_EQ(
        0u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_received"));

    EXPECT_EQ(
        0u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_processed"));
  }

  Shutdown();
}


// Verify that a framework is being correctly throttled at the
// configured rate.
TEST_F(RateLimitingTest, RateLimitingEnabled)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Clock::pause();

  // Settle to make sure master is ready for incoming requests, i.e.,
  // '_recover()' completes.
  Clock::settle();

  // Advance before the test so that the 1st call to Metrics endpoint
  // is not throttled. MetricsProcess which hosts the endpoint
  // throttles requests at 2qps and its singleton instance is shared
  // across tests.
  Clock::advance(Milliseconds(501));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call.
  Future<mesos::scheduler::Call> subscribeCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver.start());

  AWAIT_READY(subscribeCall);
  AWAIT_READY(frameworkRegisteredMessage);

  const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

  // Keep sending duplicate subscribe call. Master sends
  // FrameworkRegisteredMessage back after processing each of them.
  {
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), subscribeCall.get());

    // The first message is not throttled because it's at the head of
    // the queue.
    AWAIT_READY(duplicateFrameworkRegisteredMessage);

    // Verify that one message is received and processed (after
    // registration).
    JSON::Object metrics = Metrics();

    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(1, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // The 2nd message is throttled for a second.
  Future<process::Message> duplicateFrameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                   master.get(),
                   _);

  process::post(schedulerPid, master.get(), subscribeCall.get());

  // Advance for half a second and verify that the message is still
  // not processed.
  Clock::advance(Milliseconds(501));

  // Settle to make sure all events not delayed are processed.
  Clock::settle();

  {
    JSON::Object metrics = Metrics();

    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));

    // The 2nd message is received and but not processed after half
    // a second because of throttling.
    EXPECT_EQ(2, metrics.values[messages_received].as<JSON::Number>().value);
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
    EXPECT_TRUE(duplicateFrameworkRegisteredMessage.isPending());
  }

  // After another half a second the message should be processed.
  Clock::advance(Milliseconds(501));
  AWAIT_READY(duplicateFrameworkRegisteredMessage);

  // Verify counters after processing of the message.
  JSON::Object metrics = Metrics();

  const string& messages_received =
    "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
  EXPECT_EQ(1u, metrics.values.count(messages_received));
  const string& messages_processed =
    "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
  EXPECT_EQ(1u, metrics.values.count(messages_processed));

  EXPECT_EQ(2, metrics.values[messages_received].as<JSON::Number>().value);
  EXPECT_EQ(2, metrics.values[messages_processed].as<JSON::Number>().value);

  EXPECT_EQ(DRIVER_STOPPED, driver.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver.join());

  Shutdown();
}


// Verify that framework message counters and rate limiters work with
// frameworks of different principals which are throttled at
// different rates.
TEST_F(RateLimitingTest, DifferentPrincipalFrameworks)
{
  master::Flags flags = CreateMasterFlags();

  // Configure RateLimits to be 1qps and 0.5qps for two frameworks.
  // Rate for the second framework is implicitly specified via
  // 'aggregate_default_qps'.
  RateLimits limits;
  RateLimit* limit1 = limits.mutable_limits()->Add();
  limit1->set_principal("framework1");
  limit1->set_qps(1);
  limits.set_aggregate_default_qps(0.5);
  flags.rate_limits = limits;

  flags.authenticate_frameworks = false;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  Clock::pause();

  // Settle to make sure master is ready for incoming requests, i.e.,
  // '_recover()' completes.
  Clock::settle();

  // Advance before the test so that the 1st call to Metrics endpoint
  // is not throttled. MetricsProcess which hosts the endpoint
  // throttles requests at 2qps and its singleton instance is shared
  // across tests.
  Clock::advance(Milliseconds(501));

  // 1. Register two frameworks.

  // 1.1. Create the first framework.
  FrameworkInfo frameworkInfo1 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo1.set_principal("framework1");

  MockScheduler sched1;
  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver1 =
    new MesosSchedulerDriver(&sched1, frameworkInfo1, master.get());

  EXPECT_CALL(sched1, registered(driver1, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call for sched1.
  Future<mesos::scheduler::Call> subscribeCall1 = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage1 = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver1->start());

  AWAIT_READY(subscribeCall1);
  AWAIT_READY(frameworkRegisteredMessage1);

  const process::UPID sched1Pid = frameworkRegisteredMessage1.get().to;

  // 1.2. Create the second framework.
  FrameworkInfo frameworkInfo2 = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo2.set_principal("framework2");

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get());

  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call for sched2.
  Future<mesos::scheduler::Call> subscribeCall2 = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage2 = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver2.start());

  AWAIT_READY(subscribeCall2);
  AWAIT_READY(frameworkRegisteredMessage2);

  const process::UPID sched2Pid = frameworkRegisteredMessage2.get().to;

  // 2. Send duplicate subscribe call from the two schedulers to
  // Master.

  // The first messages are not throttled because they are at the
  // head of the queue.
  {
    Future<process::Message> duplicateFrameworkRegisteredMessage1 =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     sched1Pid);
    Future<process::Message> duplicateFrameworkRegisteredMessage2 =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     sched2Pid);

    process::post(sched1Pid, master.get(), subscribeCall1.get());
    process::post(sched2Pid, master.get(), subscribeCall2.get());

    AWAIT_READY(duplicateFrameworkRegisteredMessage1);
    AWAIT_READY(duplicateFrameworkRegisteredMessage2);
  }

  // Send the second batch of messages which should be throttled.
  {
    Future<process::Message> duplicateFrameworkRegisteredMessage1 =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     sched1Pid);
    Future<process::Message> duplicateFrameworkRegisteredMessage2 =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     sched2Pid);

    process::post(sched1Pid, master.get(), subscribeCall1.get());
    process::post(sched2Pid, master.get(), subscribeCall2.get());

    // Settle to make sure the pending futures below are indeed due
    // to throttling.
    Clock::settle();

    EXPECT_TRUE(duplicateFrameworkRegisteredMessage1.isPending());
    EXPECT_TRUE(duplicateFrameworkRegisteredMessage2.isPending());

    {
      // Verify counters also indicate that messages are received but
      // not processed.
      JSON::Object metrics = Metrics();

      EXPECT_EQ(
          1u, metrics.values.count("frameworks/framework1/messages_received"));
      EXPECT_EQ(
          1u, metrics.values.count("frameworks/framework1/messages_processed"));
      EXPECT_EQ(
          1u, metrics.values.count("frameworks/framework2/messages_received"));
      EXPECT_EQ(
          1u, metrics.values.count("frameworks/framework2/messages_processed"));

      EXPECT_EQ(
          2,
          metrics.values["frameworks/framework1/messages_received"]
            .as<JSON::Number>().value);
      EXPECT_EQ(
          2,
          metrics.values["frameworks/framework2/messages_received"]
            .as<JSON::Number>().value);
      EXPECT_EQ(
          1,
          metrics.values["frameworks/framework1/messages_processed"]
            .as<JSON::Number>().value);
      EXPECT_EQ(
          1,
          metrics.values["frameworks/framework2/messages_processed"]
            .as<JSON::Number>().value);
    }

    // Advance for a second so the message from framework1 (1qps)
    // should be processed.
    Clock::advance(Seconds(1));
    AWAIT_READY(duplicateFrameworkRegisteredMessage1);
    EXPECT_TRUE(duplicateFrameworkRegisteredMessage2.isPending());

    // Framework1's message is processed and framework2's is not
    // because it's throttled at a lower rate.
    JSON::Object metrics = Metrics();
    EXPECT_EQ(
        2,
        metrics.values["frameworks/framework1/messages_processed"]
          .as<JSON::Number>().value);
    EXPECT_EQ(
        1,
        metrics.values["frameworks/framework2/messages_processed"]
          .as<JSON::Number>().value);

    // After another half a second framework2 (0.2qps)'s message is
    // processed as well.
    Clock::advance(Seconds(1));
    AWAIT_READY(duplicateFrameworkRegisteredMessage2);
  }

  // 2. Counters confirm that both frameworks' messages are processed.
  {
    JSON::Object metrics = Metrics();

    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework1/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework1/messages_processed"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework2/messages_received"));
    EXPECT_EQ(
        1u, metrics.values.count("frameworks/framework2/messages_processed"));
    EXPECT_EQ(
        2,
        metrics.values["frameworks/framework1/messages_received"]
          .as<JSON::Number>().value);
    EXPECT_EQ(
        2,
        metrics.values["frameworks/framework2/messages_received"]
          .as<JSON::Number>().value);
    EXPECT_EQ(
        2,
        metrics.values["frameworks/framework1/messages_processed"]
          .as<JSON::Number>().value);
    EXPECT_EQ(
        2,
        metrics.values["frameworks/framework2/messages_processed"]
          .as<JSON::Number>().value);
  }

  // 3. Remove a framework and its message counters are deleted while
  // the other framework's counters stay.
  Future<Nothing> removeFramework =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::removeFramework);

  driver1->stop();
  driver1->join();
  delete driver1;

  // No need to advance again because we already advanced 1sec for
  // sched2 so the RateLimiter for sched1 doesn't impose a delay this
  // time.
  AWAIT_READY(removeFramework);

  // Settle to avoid the race between the removal of the counters and
  // the metrics endpoint query.
  Clock::settle();

  // Advance for Metrics rate limiting.
  Clock::advance(Milliseconds(501));

  JSON::Object metrics = Metrics();

  EXPECT_EQ(
      0u, metrics.values.count("frameworks/framework1/messages_received"));
  EXPECT_EQ(
      0u, metrics.values.count("frameworks/framework1/messages_processed"));
  EXPECT_EQ(
      1u, metrics.values.count("frameworks/framework2/messages_received"));
  EXPECT_EQ(
      1u, metrics.values.count("frameworks/framework2/messages_processed"));

  driver2.stop();
  driver2.join();

  Shutdown();
}


// Verify that if multiple frameworks use the same principal, they
// share the same counters, are throtted at the same rate and
// removing one framework doesn't remove the counters.
TEST_F(RateLimitingTest, SamePrincipalFrameworks)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Clock::pause();

  // Settle to make sure master is ready for incoming requests, i.e.,
  // '_recover()' completes.
  Clock::settle();

  // Advance before the test so that the 1st call to Metrics endpoint
  // is not throttled. MetricsProcess which hosts the endpoint
  // throttles requests at 2qps and its singleton instance is shared
  // across tests.
  Clock::advance(Milliseconds(501));

  // 1. Register two frameworks.

  // 1.1. Create the first framework.
  MockScheduler sched1;
  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver1 = new MesosSchedulerDriver(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(driver1, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call for sched1.
  Future<mesos::scheduler::Call> subscribeCall1 = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage1 = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver1->start());

  AWAIT_READY(subscribeCall1);
  AWAIT_READY(frameworkRegisteredMessage1);

  const process::UPID sched1Pid = frameworkRegisteredMessage1.get().to;

  // 1.2. Create the second framework.

  // 'sched2' uses the same principal "test-principal".
  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(&driver2, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call for sched2.
  Future<mesos::scheduler::Call> subscribeCall2 = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage2 = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver2.start());

  AWAIT_READY(subscribeCall2);
  AWAIT_READY(frameworkRegisteredMessage2);

  const process::UPID sched2Pid = frameworkRegisteredMessage2.get().to;

  // Message counters added after both frameworks are registered.
  {
    JSON::Object metrics = Metrics();

    EXPECT_EQ(
        1u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_received"));
    EXPECT_EQ(
        1u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_processed"));
  }

  // The 1st message from sched1 is not throttled as it's at the head
  // of the queue but the 1st message from sched2 is because it's
  // throttled by the same RateLimiter.
  Future<process::Message> duplicateFrameworkRegisteredMessage1 =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                   master.get(),
                   sched1Pid);
  Future<process::Message> duplicateFrameworkRegisteredMessage2 =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                   master.get(),
                   sched2Pid);

  process::post(sched1Pid, master.get(), subscribeCall1.get());
  process::post(sched2Pid, master.get(), subscribeCall2.get());

  AWAIT_READY(duplicateFrameworkRegisteredMessage1);

  // Settle to make sure the pending future is indeed caused by
  // throttling.
  Clock::settle();
  EXPECT_TRUE(duplicateFrameworkRegisteredMessage2.isPending());

  // For metrics endpoint.
  Clock::advance(Milliseconds(501));

  {
    JSON::Object metrics = Metrics();

    // Two messages received and one processed.
    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(2, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // Advance for another half a second to make sure throttled
  // message is processed.
  Clock::advance(Milliseconds(501));

  AWAIT_READY(duplicateFrameworkRegisteredMessage2);

  Future<Nothing> removeFramework =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::removeFramework);

  driver1->stop();
  driver1->join();
  delete driver1;

  // Advance to let the teardown call come through.
  Clock::settle();
  Clock::advance(Seconds(1));

  AWAIT_READY(removeFramework);

  // Message counters are not removed after the first framework is
  // unregistered.

  // For metrics endpoint.
  Clock::advance(Milliseconds(501));

  {
    JSON::Object metrics = Metrics();

    EXPECT_EQ(
        1u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_received"));
    EXPECT_EQ(
        1u,
        metrics.values.count("frameworks/" + DEFAULT_CREDENTIAL.principal() +
                             "/messages_processed"));
  }

  driver2.stop();
  driver2.join();

  Shutdown();
}


// Verify that when a scheduler fails over, the new scheduler
// instance continues to use the same counters and RateLimiter.
TEST_F(RateLimitingTest, SchedulerFailover)
{
  Try<PID<Master> > master = StartMaster();
  ASSERT_SOME(master);

  Clock::pause();

  // Settle to make sure master is ready for incoming requests, i.e.,
  // '_recover()' completes.
  Clock::settle();

  // Advance before the test so that the 1st call to Metrics endpoint
  // is not throttled. MetricsProcess which hosts the endpoint
  // throttles requests at 2qps and its singleton instance is shared
  // across tests.
  Clock::advance(Milliseconds(501));

  // 1. Launch the first (i.e., failing) scheduler and verify its
  // counters.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  {
    // Grab the stuff we need to replay the subscribe call.
    Future<mesos::scheduler::Call> subscribeCall = FUTURE_CALL(
        mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

    Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
        Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

    driver1.start();

    AWAIT_READY(subscribeCall);
    AWAIT_READY(frameworkRegisteredMessage);
    AWAIT_READY(frameworkId);

    const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

    // Send a duplicate subscribe call. Master replies with a
    // duplicate FrameworkRegisteredMessage.
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), subscribeCall.get());

    // Now one message has been received and processed by Master in
    // addition to the subscribe call.
    AWAIT_READY(duplicateFrameworkRegisteredMessage);

    // Settle to make sure message_processed counters are updated.
    Clock::settle();

    // Verify the message counters.
    JSON::Object metrics = Metrics();

    // One message received and processed after the framework is
    // registered.
    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(1, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // 2. Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and verify that
  // its counters are not reset to zero.

  MockScheduler sched2;

  FrameworkInfo framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(
      &sched2, framework2, master.get(), DEFAULT_CREDENTIAL);

  // Scheduler driver ignores duplicate FrameworkRegisteredMessages.
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .Times(1);

  Future<Nothing> sched1Error;
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"))
    .WillOnce(FutureSatisfy(&sched1Error));

  // Grab the stuff we need to replay the subscribe call.
  Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  Future<mesos::scheduler::Call> subscribeCall2 = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  driver2.start();

  AWAIT_READY(subscribeCall2);
  AWAIT_READY(sched1Error);
  AWAIT_READY(frameworkRegisteredMessage);

  const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

  Future<process::Message> duplicateFrameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                   master.get(),
                   _);

  // Sending a duplicate subscribe call to test the message counters
  // with the new scheduler instance.
  process::post(schedulerPid, master.get(), subscribeCall2.get());

  // Settle to make sure everything not delayed is processed.
  Clock::settle();

  // Throttled because the same RateLimiter instance is
  // throttling the new scheduler instance.
  EXPECT_TRUE(duplicateFrameworkRegisteredMessage.isPending());

  // Advance for metrics.
  Clock::advance(Milliseconds(501));

  {
    JSON::Object metrics = Metrics();

    // Verify that counters correctly indicates the message is
    // received but not processed.
    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(2, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // Need another half a second to have it processed.
  Clock::advance(Milliseconds(501));

  AWAIT_READY(duplicateFrameworkRegisteredMessage);

  // Advance for metrics.
  Clock::advance(Milliseconds(501));

  {
    JSON::Object metrics = Metrics();

    // Another message after sched2 is reregistered plus the one from
    // the sched1.
    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(2, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(2, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());

  Shutdown();
}


TEST_F(RateLimitingTest, CapacityReached)
{
  master::Flags flags = CreateMasterFlags();
  RateLimits limits;
  RateLimit* limit = limits.mutable_limits()->Add();
  limit->set_principal(DEFAULT_CREDENTIAL.principal());
  limit->set_qps(1);
  limit->set_capacity(2);
  flags.rate_limits = limits;

  Try<PID<Master> > master = StartMaster(flags);
  ASSERT_SOME(master);

  Clock::pause();

  // Advance before the test so that the 1st call to Metrics endpoint
  // is not throttled. MetricsProcess which hosts the endpoint
  // throttles requests at 2qps and its singleton instance is shared
  // across tests.
  Clock::advance(Milliseconds(501));

  MockScheduler sched;

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  // Use a long failover timeout so the master doesn't unregister the
  // framework right away when it aborts.
  frameworkInfo.set_failover_timeout(10);

  // Create MesosSchedulerDriver on the heap because of the need to
  // destroy it during the test due to MESOS-1456.
  MesosSchedulerDriver* driver = new MesosSchedulerDriver(
      &sched, frameworkInfo, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(driver, _, _))
    .Times(1);

  // Grab the stuff we need to replay the subscribe call.
  Future<mesos::scheduler::Call> subscribeCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::SUBSCRIBE, _, _);

  Future<process::Message> frameworkRegisteredMessage = FUTURE_MESSAGE(
      Eq(FrameworkRegisteredMessage().GetTypeName()), master.get(), _);

  ASSERT_EQ(DRIVER_RUNNING, driver->start());

  AWAIT_READY(subscribeCall);
  AWAIT_READY(frameworkRegisteredMessage);

  const process::UPID schedulerPid = frameworkRegisteredMessage.get().to;

  // Keep sending duplicate subscribe calls. Master sends
  // FrameworkRegisteredMessage back after processing each of them.
  {
    Future<process::Message> duplicateFrameworkRegisteredMessage =
      FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()),
                     master.get(),
                     _);

    process::post(schedulerPid, master.get(), subscribeCall.get());

    // The first message is not throttled because it's at the head of
    // the queue.
    AWAIT_READY(duplicateFrameworkRegisteredMessage);

    // Verify that one message is received and processed (after
    // registration).
    JSON::Object metrics = Metrics();

    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(1, metrics.values[messages_received].as<JSON::Number>().value);

    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // The subsequent messages are going to be throttled.
  Future<process::Message> frameworkErrorMessage =
    FUTURE_MESSAGE(Eq(FrameworkErrorMessage().GetTypeName()),
                   master.get(),
                   _);

  // Send two messages which will be queued up. This will reach but not
  // exceed the capacity.
  for (int i = 0; i < 2; i++) {
    process::post(schedulerPid, master.get(), subscribeCall.get());
  }

  // Settle to make sure no error is sent just yet.
  Clock::settle();
  EXPECT_TRUE(frameworkErrorMessage.isPending());

  // The 3rd message results in an immediate error.
  Future<Nothing> error;
  EXPECT_CALL(sched, error(driver, _))
    .WillOnce(FutureSatisfy(&error));

  process::post(schedulerPid, master.get(), subscribeCall.get());
  AWAIT_READY(frameworkErrorMessage);

  // Settle to make sure scheduler aborts and its
  // DeactivateFrameworkMessage is received by master.
  Clock::settle();

  AWAIT_READY(error);

  // Stop the driver but indicate it wants to failover.
  EXPECT_EQ(DRIVER_ABORTED, driver->stop(true));
  EXPECT_EQ(DRIVER_STOPPED, driver->join());
  delete driver;

  // Wait for half a second for metrics endpoint.
  Clock::advance(Milliseconds(501));

  {
    JSON::Object metrics = Metrics();

    const string& messages_received =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
    EXPECT_EQ(1u, metrics.values.count(messages_received));
    EXPECT_EQ(5, metrics.values[messages_received].as<JSON::Number>().value);
    const string& messages_processed =
      "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
    EXPECT_EQ(1u, metrics.values.count(messages_processed));
    // Four messages not processed, two in the queue and two dropped.
    EXPECT_EQ(1, metrics.values[messages_processed].as<JSON::Number>().value);
  }

  // Advance three times for the two pending messages and the exited
  // event to be processed.
  for (int i = 0; i < 3; i++) {
    Clock::advance(Milliseconds(1001));
    Clock::settle();
  }

  // Counters are not removed because the scheduler is not
  // unregistered and the master expects it to failover.
  JSON::Object metrics = Metrics();

  const string& messages_received =
    "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_received";
  EXPECT_EQ(1u, metrics.values.count(messages_received));
  EXPECT_EQ(5, metrics.values[messages_received].as<JSON::Number>().value);
  const string& messages_processed =
    "frameworks/" + DEFAULT_CREDENTIAL.principal() + "/messages_processed";
  EXPECT_EQ(1u, metrics.values.count(messages_processed));
  // Two messages are dropped.
  EXPECT_EQ(3, metrics.values[messages_processed].as<JSON::Number>().value);

  Shutdown();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
