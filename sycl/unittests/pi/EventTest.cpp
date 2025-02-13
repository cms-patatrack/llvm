//==---- EventTest.cpp --- PI unit tests --------------------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CL/sycl/detail/pi.hpp"
#include <detail/plugin.hpp>
#include <atomic>
#include <gtest/gtest.h>
#include <thread>

using namespace cl::sycl;

namespace pi {
class DISABLED_EventTest : public ::testing::Test {
protected:
  std::vector<detail::plugin> Plugins;

  pi_platform _platform;
  pi_context _context;
  pi_queue _queue;
  pi_device _device;
  pi_result _result;

  DISABLED_EventTest()
      : _context{nullptr}, _queue{nullptr}, _device{nullptr},
        _result{PI_INVALID_VALUE} {
    Plugins = detail::pi::initialize();
  }

  ~DISABLED_EventTest() override = default;

  void SetUp() override {
    pi_uint32 numPlatforms = 0;
    ASSERT_FALSE(Plugins.empty());

    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piPlatformsGet>(
                  0, nullptr, &numPlatforms)),
              PI_SUCCESS)
        << "piPlatformsGet failed.\n";

    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piPlatformsGet>(
                  numPlatforms, &_platform, nullptr)),
              PI_SUCCESS)
        << "piPlatformsGet failed.\n";
    (void)numPlatforms; // Deal with unused variable warning

    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piDevicesGet>(
                  _platform, PI_DEVICE_TYPE_GPU, 1, &_device, nullptr)),
              PI_SUCCESS);

    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piContextCreate>(
                  nullptr, 1, &_device, nullptr, nullptr, &_context)),
              PI_SUCCESS);

    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piQueueCreate>(
                  _context, _device, 0, &_queue)),
              PI_SUCCESS);

    _result = PI_INVALID_VALUE;
  }

  void TearDown() override {

    ASSERT_EQ(
        (Plugins[0].call_nocheck<detail::PiApiKind::piQueueRelease>(_queue)),
        PI_SUCCESS);

    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piContextRelease>(
                  _context)),
              PI_SUCCESS);
  }
};

// TODO: need more negative tests to show errors being reported when expected
// (invalid arguments etc).

TEST_F(DISABLED_EventTest, PICreateEvent) {
  pi_event foo;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventCreate>(_context,
                                                                       &foo)),
            PI_SUCCESS);
  ASSERT_NE(foo, nullptr);

  EXPECT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventRelease>(foo)),
            PI_SUCCESS);
}

struct callback_user_data {
  pi_int32 event_type;
  int index;
};

TEST_F(DISABLED_EventTest, piEventSetCallback) {

  constexpr size_t event_type_count = 3;

  static bool triggered_flag[event_type_count] = {false, false, false};

  pi_int32 event_callback_types[event_type_count] = {
      PI_EVENT_SUBMITTED, PI_EVENT_RUNNING, PI_EVENT_COMPLETE};

  callback_user_data user_data[event_type_count];

  auto callback = [](pi_event event, pi_int32 status, void *data) {
    ASSERT_NE(data, nullptr);

    callback_user_data *pdata = static_cast<callback_user_data *>(data);

#ifndef NDEBUG
    printf("\tEvent callback %d of type %d triggered\n", pdata->index,
           pdata->event_type);
#endif

    triggered_flag[pdata->index] = true;
  };

  // gate event lets us register callbacks before letting the enqueued work be
  // executed.
  pi_event gateEvent;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventCreate>(
                _context, &gateEvent)),
            PI_SUCCESS);

  constexpr const size_t dataCount = 1000u;
  std::vector<int> data(dataCount);
  auto size_in_bytes = data.size() * sizeof(int);

  pi_mem memObj;
  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piMemBufferCreate>(
          _context, PI_MEM_FLAGS_ACCESS_RW, size_in_bytes, nullptr, &memObj)),
      PI_SUCCESS);

  pi_event syncEvent;
  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEnqueueMemBufferWrite>(
          _queue, memObj, false, 0, size_in_bytes, data.data(), 1, &gateEvent,
          &syncEvent)),
      PI_SUCCESS);

  for (size_t i = 0; i < event_type_count; i++) {
    user_data[i].event_type = event_callback_types[i];
    user_data[i].index = i;
    ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventSetCallback>(
                  syncEvent, event_callback_types[i], callback, user_data + i)),
              PI_SUCCESS);
  }

  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventSetStatus>(
                gateEvent, PI_EVENT_COMPLETE)),
            PI_SUCCESS);
  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEventsWait>(1, &syncEvent)),
      PI_SUCCESS);
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piQueueFinish>(_queue)),
            PI_SUCCESS);

  for (size_t k = 0; k < event_type_count; ++k) {
    EXPECT_TRUE(triggered_flag[k]);
  }

  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEventRelease>(gateEvent)),
      PI_SUCCESS);
  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEventRelease>(syncEvent)),
      PI_SUCCESS);
}

TEST_F(DISABLED_EventTest, piEventGetInfo) {

  pi_event foo;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventCreate>(_context,
                                                                       &foo)),
            PI_SUCCESS);
  ASSERT_NE(foo, nullptr);

  pi_uint64 paramValue = 0;
  pi_uint64 retSize = 0;
  EXPECT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
                foo, PI_EVENT_INFO_COMMAND_EXECUTION_STATUS, sizeof(paramValue),
                &paramValue, &retSize)),
            PI_SUCCESS);

  EXPECT_EQ(retSize, sizeof(pi_int32));
  EXPECT_EQ(paramValue, PI_EVENT_SUBMITTED);

  EXPECT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventRelease>(foo)),
            PI_SUCCESS);
}

TEST_F(DISABLED_EventTest, piEventSetStatus) {

  pi_event foo;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventCreate>(_context,
                                                                       &foo)),
            PI_SUCCESS);
  ASSERT_NE(foo, nullptr);

  pi_event_status paramValue = PI_EVENT_QUEUED;
  size_t retSize = 0u;
  Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
      foo, PI_EVENT_INFO_COMMAND_EXECUTION_STATUS, sizeof(paramValue),
      &paramValue, &retSize);

  EXPECT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventSetStatus>(
                foo, PI_EVENT_COMPLETE)),
            PI_SUCCESS);

  paramValue = {};
  retSize = 0u;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
                foo, PI_EVENT_INFO_COMMAND_EXECUTION_STATUS, sizeof(paramValue),
                &paramValue, &retSize)),
            PI_SUCCESS);
  ASSERT_EQ(paramValue, PI_EVENT_COMPLETE);

  EXPECT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventRelease>(foo)),
            PI_SUCCESS);
}

TEST_F(DISABLED_EventTest, WaitForManualEventOnOtherThread) {

  pi_event foo;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventCreate>(_context,
                                                                       &foo)),
            PI_SUCCESS);
  ASSERT_NE(foo, nullptr);

  pi_event_status paramValue = {};
  size_t retSize = 0u;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
                foo, PI_EVENT_INFO_COMMAND_EXECUTION_STATUS, sizeof(paramValue),
                &paramValue, &retSize)),
            PI_SUCCESS);
  ASSERT_EQ(paramValue, PI_EVENT_SUBMITTED);

  std::atomic<bool> started{false};

  auto tWaiter = std::thread([&]() {
    started = true;
    ASSERT_EQ(
        (Plugins[0].call_nocheck<detail::PiApiKind::piEventsWait>(1, &foo)),
        PI_SUCCESS);
  });

  while (!started) {
  };

  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventSetStatus>(
                foo, PI_EVENT_COMPLETE)),
            PI_SUCCESS);

  tWaiter.join();

  paramValue = {};
  retSize = 0u;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
                foo, PI_EVENT_INFO_COMMAND_EXECUTION_STATUS, sizeof(paramValue),
                &paramValue, &retSize)),
            PI_SUCCESS);
  ASSERT_EQ(paramValue, PI_EVENT_COMPLETE);

  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventRelease>(foo)),
            PI_SUCCESS);
}

TEST_F(DISABLED_EventTest, piEnqueueEventsWait) {

  constexpr const size_t dataCount = 10u;
  int output[dataCount] = {};
  const int data[dataCount] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  constexpr const size_t bytes = sizeof(data);

  pi_mem memObj;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piMemBufferCreate>(
                _context, PI_MEM_FLAGS_ACCESS_RW, bytes, nullptr, &memObj)),
            PI_SUCCESS);

  pi_event events[4] = {nullptr, nullptr, nullptr, nullptr};

  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEnqueueMemBufferWrite>(
          _queue, memObj, true, 0, bytes, data, 0, nullptr, &events[0])),
      PI_SUCCESS);
  ASSERT_NE(events[0], nullptr);

  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEnqueueMemBufferRead>(
          _queue, memObj, true, 0, bytes, output, 0, nullptr, &events[1])),
      PI_SUCCESS);
  ASSERT_NE(events[1], nullptr);

  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventCreate>(
                _context, &events[2])),
            PI_SUCCESS);
  ASSERT_NE(events[2], nullptr);

  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEnqueueEventsWait>(
                _queue, 3, events, &events[3])),
            PI_SUCCESS);
  ASSERT_NE(events[3], nullptr);

  pi_event_status paramValue = {};
  size_t retSize = 0u;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
                events[3], PI_EVENT_INFO_COMMAND_EXECUTION_STATUS,
                sizeof(paramValue), &paramValue, &retSize)),
            PI_SUCCESS);
  ASSERT_NE(paramValue, PI_EVENT_COMPLETE);

  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventSetStatus>(
                events[2], PI_EVENT_COMPLETE)),
            PI_SUCCESS);

  ASSERT_EQ(
      (Plugins[0].call_nocheck<detail::PiApiKind::piEventsWait>(1, &events[3])),
      PI_SUCCESS);

  paramValue = {};
  retSize = 0u;
  ASSERT_EQ((Plugins[0].call_nocheck<detail::PiApiKind::piEventGetInfo>(
                events[3], PI_EVENT_INFO_COMMAND_EXECUTION_STATUS,
                sizeof(paramValue), &paramValue, &retSize)),
            PI_SUCCESS);
  ASSERT_EQ(paramValue, PI_EVENT_COMPLETE);
}

} // namespace pi
