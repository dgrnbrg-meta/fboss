/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwLinkStateDependentTest.h"
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/hw/test/HwTestPortUtils.h"
#include "fboss/agent/hw/test/TrafficPolicyUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestOlympicUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestQosUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestWatermarkUtils.h"
#include "fboss/agent/packet/EthHdr.h"
#include "fboss/agent/packet/IPv6Hdr.h"
#include "fboss/agent/packet/TCPHeader.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/ResourceLibUtil.h"

#include <folly/IPAddress.h>

namespace facebook::fboss {

class HwAqmTest : public HwLinkStateDependentTest {
 private:
  static constexpr auto kDefaultTxPayloadBytes{7000};

  cfg::SwitchConfig initialConfig() const override {
    auto cfg = utility::oneL3IntfConfig(
        getHwSwitch(), masterLogicalPortIds()[0], cfg::PortLoopbackMode::MAC);
    if (isSupported(HwAsic::Feature::L3_QOS)) {
      auto streamType =
          *(getPlatform()->getAsic()->getQueueStreamTypes(false).begin());
      utility::addOlympicQueueConfig(
          &cfg, streamType, getPlatform()->getAsic());
      utility::addOlympicQosMaps(cfg);
    }
    return cfg;
  }

  cfg::SwitchConfig wredDropConfig() const {
    auto cfg = utility::oneL3IntfConfig(
        getHwSwitch(), masterLogicalPortIds()[0], cfg::PortLoopbackMode::MAC);
    if (isSupported(HwAsic::Feature::L3_QOS)) {
      auto streamType =
          *(getPlatform()->getAsic()->getQueueStreamTypes(false).begin());
      utility::addQueueWredDropConfig(&cfg, streamType);
      utility::addOlympicQosMaps(cfg);
    }
    return cfg;
  }

  cfg::SwitchConfig configureQueue2WithWredThreshold() const {
    auto cfg = utility::oneL3IntfConfig(
        getHwSwitch(), masterLogicalPortIds()[0], cfg::PortLoopbackMode::MAC);
    if (isSupported(HwAsic::Feature::L3_QOS)) {
      auto streamType =
          *(getPlatform()->getAsic()->getQueueStreamTypes(false).begin());
      utility::addOlympicQueueConfig(
          &cfg, streamType, getPlatform()->getAsic(), true /*add wred*/);
      utility::addOlympicQosMaps(cfg);
    }
    return cfg;
  }

  uint8_t kSilverDscp() const {
    return 0;
  }

  uint8_t kDscp() const {
    return 5;
  }

  template <typename ECMP_HELPER>
  void disableTTLDecrements(const ECMP_HELPER& ecmpHelper) {
    for (const auto& nextHop : ecmpHelper.getNextHops()) {
      utility::disableTTLDecrements(
          getHwSwitch(), ecmpHelper.getRouterId(), nextHop);
    }
  }

  void sendPkt(
      uint8_t dscpVal,
      bool isEcn,
      bool ensure = false,
      int payloadLen = kDefaultTxPayloadBytes) {
    auto kECT1 = 0x01; // ECN capable transport ECT(1)

    dscpVal = static_cast<uint8_t>(dscpVal << 2);
    if (isEcn) {
      dscpVal |= kECT1;
    }

    auto vlanId = utility::firstVlanID(initialConfig());
    auto intfMac = getIntfMac();
    auto srcMac = utility::MacAddressGenerator().get(intfMac.u64NBO() + 1);
    auto txPacket = utility::makeTCPTxPacket(
        getHwSwitch(),
        vlanId,
        srcMac,
        intfMac,
        folly::IPAddressV6("2620:0:1cfe:face:b00c::3"),
        folly::IPAddressV6("2620:0:1cfe:face:b00c::4"),
        8000,
        8001,
        dscpVal,
        255,
        std::vector<uint8_t>(payloadLen, 0xff));

    if (ensure) {
      getHwSwitchEnsemble()->ensureSendPacketSwitched(std::move(txPacket));
    } else {
      getHwSwitch()->sendPacketSwitchedSync(std::move(txPacket));
    }
  }

  /*
   * For congestion detection queue length of minLength = 128, and maxLength =
   * 128, a packet count of 128 has been enough to cause ECN marking. Inject
   * 128 * 2 packets to avoid test noise.
   */
  void sendPkts(
      uint8_t dscpVal,
      bool isEcn,
      int cnt = 256,
      int payloadLen = kDefaultTxPayloadBytes) {
    for (int i = 0; i < cnt; i++) {
      sendPkt(dscpVal, isEcn, false /* ensure */, payloadLen);
    }
  }
  folly::MacAddress getIntfMac() const {
    auto vlanId = utility::firstVlanID(initialConfig());
    return utility::getInterfaceMac(getProgrammedState(), vlanId);
  }

  void queueEcnWredThresholdSetup(
      bool isEcn,
      std::vector<int> queueIds,
      cfg::SwitchConfig& cfg) {
    for (auto queueId : queueIds) {
      if (isEcn) {
        utility::addQueueEcnConfig(
            &cfg,
            queueId,
            utility::kQueueConfigAqmsEcnThresholdMinMax,
            utility::kQueueConfigAqmsEcnThresholdMinMax);
      } else {
        utility::addQueueWredConfig(
            &cfg,
            queueId,
            utility::kQueueConfigAqmsWredThresholdMinMax,
            utility::kQueueConfigAqmsWredThresholdMinMax,
            utility::kQueueConfigAqmsWredDropProbability);
      }
    }
  }

  /*
   * Ensure that the number of dropped packets is as expected. Allow for
   * an error to account for more / less drops while its worked out.
   */
  void verifyWredDroppedPacketCount(
      HwPortStats& after,
      HwPortStats& before,
      int expectedDroppedPkts) {
    constexpr auto kAcceptableError = 2;
    auto deltaWredDroppedPackets =
        *after.wredDroppedPackets__ref() - *before.wredDroppedPackets__ref();
    XLOG(DBG0) << "Delta WRED dropped pkts: " << deltaWredDroppedPackets;
    EXPECT_GT(deltaWredDroppedPackets, expectedDroppedPkts - kAcceptableError);
    EXPECT_LT(deltaWredDroppedPackets, expectedDroppedPkts + kAcceptableError);
  }

  /*
   * Ensure that the number of marked packets is as expected. Allow for
   * an error to account for more / less marking while its worked out.
   */
  void verifyEcnMarkedPacketCount(
      HwPortStats& after,
      HwPortStats& before,
      int expectedMarkedPkts) {
    constexpr auto kAcceptableError = 2;
    auto deltaEcnMarkedPackets =
        *after.outEcnCounter__ref() - *before.outEcnCounter__ref();
    XLOG(DBG0) << "Delta ECN marked pkts: " << deltaEcnMarkedPackets;
    EXPECT_GT(deltaEcnMarkedPackets, expectedMarkedPkts - kAcceptableError);
    EXPECT_LT(deltaEcnMarkedPackets, expectedMarkedPkts + kAcceptableError);
  }

 protected:
  void runTest(bool isEcn) {
    if (!isSupported(HwAsic::Feature::L3_QOS)) {
#if defined(GTEST_SKIP)
      GTEST_SKIP();
#endif
      return;
    }
    auto setup = [=]() {
      applyNewConfig(configureQueue2WithWredThreshold());
      auto kEcmpWidthForTest = 1;
      utility::EcmpSetupAnyNPorts6 ecmpHelper6{
          getProgrammedState(), getIntfMac()};
      resolveNeigborAndProgramRoutes(ecmpHelper6, kEcmpWidthForTest);
      if (isEcn) {
        // Assert that ECT capable packets are not counted by port ECN
        // counter and on congestion encountered packets are counted.
        sendPkt(kDscp(), isEcn, true);
        auto portStats = getLatestPortStats(masterLogicalPortIds()[0]);
        EXPECT_EQ(*portStats.outEcnCounter__ref(), 0);
      }
      disableTTLDecrements(ecmpHelper6);
    };
    auto verify = [=]() {
      sendPkts(kDscp(), isEcn);

      auto countIncremented = [&](const auto& newStats) {
        auto portStatsIter = newStats.find(masterLogicalPortIds()[0]);
        auto increment = isEcn
            ? portStatsIter->second.get_outEcnCounter_()
            : portStatsIter->second.get_wredDroppedPackets_();
        XLOG(DBG0) << (isEcn ? " ECN " : "WRED ") << " counter: " << increment;
        XLOG(DBG0)
            << "Queue watermark : "
            << portStatsIter->second.get_queueWatermarkBytes_().find(2)->second;
        return increment > 0;
      };

      // There can be delay before stats are synced.
      // So, add retries to avoid flakiness.
      EXPECT_TRUE(getHwSwitchEnsemble()->waitPortStatsCondition(
          countIncremented, 20, std::chrono::milliseconds(200)));
    };

    verifyAcrossWarmBoots(setup, verify);
  }

  void runWredDropTest() {
    if (!isSupported(HwAsic::Feature::L3_QOS)) {
#if defined(GTEST_SKIP)
      GTEST_SKIP();
#endif
      return;
    }
    auto setup = [=]() {
      applyNewConfig(wredDropConfig());
      auto kEcmpWidthForTest = 1;
      utility::EcmpSetupAnyNPorts6 ecmpHelper6{
          getProgrammedState(), getIntfMac()};
      resolveNeigborAndProgramRoutes(ecmpHelper6, kEcmpWidthForTest);
      disableTTLDecrements(ecmpHelper6);
    };
    auto verify = [=]() {
      // Send packets to queue0 and queue2 (both configured to the same weight).
      // Queue0 is configured with 0% drop probability and queue2 is configured
      // with 5% drop probability.
      sendPkts(kDscp(), false);
      sendPkts(kSilverDscp(), false);

      // Avoid flakiness
      sleep(1);

      constexpr auto queue2Id = 2;
      constexpr auto queue0Id = 0;

      // Verify queue2 watermark being updated.
      auto queueId = queue2Id;

      auto countIncremented = [&](const auto& newStats) {
        auto portStatsIter = newStats.find(masterLogicalPortIds()[0]);
        auto queueWatermark = portStatsIter->second.get_queueWatermarkBytes_()
                                  .find(queueId)
                                  ->second;
        XLOG(DBG0) << "Queue " << queueId << " watermark : " << queueWatermark;
        return queueWatermark > 0;
      };

      EXPECT_TRUE(
          getHwSwitchEnsemble()->waitPortStatsCondition(countIncremented));

      // Verify queue0 watermark being updated.
      queueId = queue0Id;
      EXPECT_TRUE(
          getHwSwitchEnsemble()->waitPortStatsCondition(countIncremented));

      auto watermarkBytes = getHwSwitchEnsemble()
                                ->getLatestPortStats(masterLogicalPortIds()[0])
                                .get_queueWatermarkBytes_();

      // Queue0 watermark should be higher than queue2 since it drops less
      // packets.
      auto queue0Watermark = watermarkBytes.find(queue0Id)->second;
      auto queue2Watermark = watermarkBytes.find(queue2Id)->second;

      XLOG(DBG0) << "Expecting queue 0 watermark " << queue0Watermark
                 << " larger than queue 2 watermark : " << queue2Watermark;
      EXPECT_TRUE(queue0Watermark > queue2Watermark);
    };

    verifyAcrossWarmBoots(setup, verify);
  }

  void waitForExpectedThresholdTestStats(
      bool isEcn,
      PortID port,
      const int queueId,
      const uint64_t expectedOutPkts,
      HwPortStats& before) {
    auto waitForExpectedOutPackets = [&](const auto& newStats) {
      uint64_t outPackets{0}, wredDrops{0}, ecnMarking{0};
      auto portStatsIter = newStats.find(port);
      if (portStatsIter != newStats.end()) {
        auto portStats = portStatsIter->second;
        outPackets = (*portStats.queueOutPackets__ref())[queueId] -
            (*before.queueOutPackets__ref())[queueId];
        if (isEcn) {
          ecnMarking =
              *portStats.outEcnCounter__ref() - *before.outEcnCounter__ref();
        } else {
          wredDrops = *portStats.wredDroppedPackets__ref() -
              *before.wredDroppedPackets__ref();
        }
      }
      /*
       * Check for outpackets as expected and ensure that ECN or WRED
       * counters are incrementing too!
       */
      return (outPackets >= expectedOutPkts) &&
          ((isEcn && ecnMarking) || (!isEcn && wredDrops));
    };

    constexpr auto kNumRetries{5};
    getHwSwitchEnsemble()->waitPortStatsCondition(
        waitForExpectedOutPackets,
        kNumRetries,
        std::chrono::milliseconds(std::chrono::seconds(1)));
  }

  void runEcnWredThresholdTest(bool isEcn) {
    constexpr auto kQueueId{0};
    constexpr auto kPayloadLength{200};
    constexpr auto kDroppedPackets{50};
    constexpr auto kMarkedPackets{50};

    const int kTxPacketLen =
        kPayloadLength + EthHdr::SIZE + IPv6Hdr::size() + TCPHeader::size();
    const int kThresholdBytes = isEcn
        ? utility::kQueueConfigAqmsEcnThresholdMinMax
        : utility::kQueueConfigAqmsWredThresholdMinMax;
    /*
     * The ECN/WRED threshold are rounded down for TAJO as opposed to
     * being rounded up to the next cell size for Broadcom.
     */
    bool roundUp = getAsic()->getAsicType() == HwAsic::AsicType::ASIC_TYPE_TAJO
        ? false
        : true;

    /*
     * Send enough packets such that the queue gets filled up to the
     * configured ECN/WRED threshold, then send a fixed number of
     * additional packets to get marked / dropped.
     */
    int numPacketsToSend = utility::getRoundedBufferThreshold(
                               getHwSwitch(), kThresholdBytes, roundUp) /
            utility::getEffectiveBytesPerPacket(getHwSwitch(), kTxPacketLen) +
        (isEcn ? kMarkedPackets : kDroppedPackets);
    XLOG(DBG3) << "Rounded threshold: "
               << utility::getRoundedBufferThreshold(
                      getHwSwitch(), kThresholdBytes)
               << ", effective bytes per pkt: "
               << utility::getEffectiveBytesPerPacket(
                      getHwSwitch(), kTxPacketLen)
               << ", kTxPacketLen: " << kTxPacketLen
               << ", pkts to send: " << numPacketsToSend;

    auto setup = [=]() {
      auto config{initialConfig()};
      queueEcnWredThresholdSetup(isEcn, {kQueueId}, config);
      applyNewConfig(config);

      // No traffic loop needed, so send traffic to a different MAC
      auto kEcmpWidthForTest = 1;
      utility::EcmpSetupAnyNPorts6 ecmpHelper6{
          getProgrammedState(),
          utility::MacAddressGenerator().get(getIntfMac().u64NBO() + 10)};
      resolveNeigborAndProgramRoutes(ecmpHelper6, kEcmpWidthForTest);
    };

    auto verify = [=]() {
      auto sendPackets = [=](PortID /* port */, int numPacketsToSend) {
        // Single port config, traffic gets forwarded out of the same!
        sendPkts(
            utility::kOlympicQueueToDscp().at(kQueueId).front(),
            isEcn,
            numPacketsToSend,
            kPayloadLength);
      };

      // Send traffic with queue buildup and get the stats at the start!
      auto before = utility::sendPacketsWithQueueBuildup(
          sendPackets,
          getHwSwitchEnsemble(),
          masterLogicalPortIds()[0],
          numPacketsToSend);

      // For ECN all packets are sent out, for WRED, account for drops!
      const uint64_t kExpectedOutPackets =
          isEcn ? numPacketsToSend : numPacketsToSend - kDroppedPackets;

      waitForExpectedThresholdTestStats(
          isEcn,
          masterLogicalPortIds()[0],
          kQueueId,
          kExpectedOutPackets,
          before);
      auto after =
          getHwSwitchEnsemble()->getLatestPortStats(masterLogicalPortIds()[0]);
      auto deltaOutPackets = (*after.queueOutPackets__ref())[kQueueId] -
          (*before.queueOutPackets__ref())[kQueueId];
      /*
       * Might see more outPackets than expected due to
       * utility::sendPacketsWithQueueBuildup()
       */
      EXPECT_GE(deltaOutPackets, kExpectedOutPackets);
      XLOG(DBG0) << "Delta out pkts: " << deltaOutPackets;

      if (isEcn) {
        verifyEcnMarkedPacketCount(after, before, kMarkedPackets);
      } else {
        verifyWredDroppedPacketCount(after, before, kDroppedPackets);
      }
    };

    verifyAcrossWarmBoots(setup, verify);
  }
};

TEST_F(HwAqmTest, verifyEcn) {
  runTest(true);
}

TEST_F(HwAqmTest, verifyWred) {
  runTest(false);
}

TEST_F(HwAqmTest, verifyWredDrop) {
  runWredDropTest();
}

TEST_F(HwAqmTest, verifyWredThreshold) {
  runEcnWredThresholdTest(false /* isEcn */);
}

} // namespace facebook::fboss
