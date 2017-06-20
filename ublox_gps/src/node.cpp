//==============================================================================
// Copyright (c) 2012, Johannes Meyer, TU Darmstadt
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the Flight Systems and Automatic Control group,
//       TU Darmstadt, nor the names of its contributors may be used to
//       endorse or promote products derived from this software without
//       specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//==============================================================================

#include <ublox_gps/gps.h>
#include <ublox_gps/utils.h>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/serial_port.hpp>

#include <boost/regex.hpp>

#include <ros/ros.h>
#include <ros/serialization.h>
#include <ublox_msgs/CfgGNSS.h>
#include <ublox_msgs/NavPOSLLH.h>
#include <ublox_msgs/NavSOL.h>
#include <ublox_msgs/NavSTATUS.h>
#include <ublox_msgs/NavVELNED.h>
#include <ublox_msgs/ublox_msgs.h>

#include <geometry_msgs/TwistWithCovarianceStamped.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <sensor_msgs/NavSatFix.h>

#include <diagnostic_updater/diagnostic_updater.h>
#include <diagnostic_updater/publisher.h>

const static uint32_t kROSQueueSize = 1;

using namespace ublox_gps;

boost::shared_ptr<ros::NodeHandle> nh;
boost::shared_ptr<diagnostic_updater::Updater> updater;
boost::shared_ptr<diagnostic_updater::TopicDiagnostic> freq_diag;
Gps gps;
std::map<std::string, bool> enabled;
std::string frame_id;

ublox_msgs::NavPVT last_nav_pos;


int fix_status_service;

void publishNavPVT(const ublox_msgs::NavPVT& m) {
  static ros::Publisher publisher =
      nh->advertise<ublox_msgs::NavPVT>("navpvt", kROSQueueSize);
  publisher.publish(m);

  /** Fix message */
  static ros::Publisher fixPublisher =
      nh->advertise<sensor_msgs::NavSatFix>("fix", kROSQueueSize);
  // timestamp
  sensor_msgs::NavSatFix fix;
  fix.header.stamp.sec = toUtcSeconds(m);
  fix.header.stamp.nsec = m.nano;

  bool fixOk = m.flags & m.FLAGS_GNSS_FIX_OK;
  uint8_t cpSoln = m.flags & m.CARRIER_PHASE_FIXED;

  fix.header.frame_id = frame_id;
  fix.latitude = m.lat * 1e-7;
  fix.longitude = m.lon * 1e-7;
  fix.altitude = m.height * 1e-3;
  if (fixOk && m.fixType >= m.FIX_TYPE_2D) {
      fix.status.status = fix.status.STATUS_FIX;
      if(cpSoln == m.CARRIER_PHASE_FIXED)
        fix.status.status = fix.status.STATUS_GBAS_FIX;
  }
  else {
      fix.status.status = fix.status.STATUS_NO_FIX;
  }
  //  calculate covariance (convert from mm to m too)
  const double stdH = (m.hAcc / 1000.0);
  const double stdV = (m.vAcc / 1000.0);
  fix.position_covariance[0] = stdH * stdH;
  fix.position_covariance[4] = stdH * stdH;
  fix.position_covariance[8] = stdV * stdV;
  fix.position_covariance_type =
      sensor_msgs::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;

  fix.status.service = fix_status_service;
  fixPublisher.publish(fix);

  /** Fix Velocity */
  static ros::Publisher velocityPublisher =
      nh->advertise<geometry_msgs::TwistWithCovarianceStamped>("fix_velocity",
                                                               kROSQueueSize);
  geometry_msgs::TwistWithCovarianceStamped velocity;
  velocity.header.stamp = fix.header.stamp;
  velocity.header.frame_id = frame_id;

  // convert to XYZ linear velocity in ENU
  velocity.twist.twist.linear.x = m.velE * 1e-3;
  velocity.twist.twist.linear.y = m.velN * 1e-3;
  velocity.twist.twist.linear.z = -m.velD * 1e-3;

  const double covSpeed = pow(m.sAcc * 1e-3, 2);

  const int cols = 6;
  velocity.twist.covariance[cols * 0 + 0] = covSpeed;
  velocity.twist.covariance[cols * 1 + 1] = covSpeed;
  velocity.twist.covariance[cols * 2 + 2] = covSpeed;
  velocity.twist.covariance[cols * 3 + 3] = -1;  //  angular rate unsupported

  velocityPublisher.publish(velocity);

  /** Update diagnostics **/
  last_nav_pos = m;
  freq_diag->tick(fix.header.stamp);
  updater->update();
}

template <typename MessageT>
void publish(const MessageT& m, const std::string& topic) {
  static ros::Publisher publisher =
      nh->advertise<MessageT>(topic, kROSQueueSize);
  publisher.publish(m);
}

void pollMessages(const ros::TimerEvent& event) {
  static std::vector<uint8_t> payload(1, 1);
  if (enabled["aid_alm"]) {
    gps.poll(ublox_msgs::Class::AID, ublox_msgs::Message::AID::ALM, payload);
  }
  if (enabled["aid_eph"]) {
    gps.poll(ublox_msgs::Class::AID, ublox_msgs::Message::AID::EPH, payload);
  }
  if (enabled["aid_hui"]) {
    gps.poll(ublox_msgs::Class::AID, ublox_msgs::Message::AID::HUI);
  }
  payload[0]++;
  if (payload[0] > 32) {
    payload[0] = 1;
  }
}

void fix_diagnostic(diagnostic_updater::DiagnosticStatusWrapper& stat) {
  //  check the last message, convert to diagnostic
  if (last_nav_pos.fixType == ublox_msgs::NavSTATUS::GPS_NO_FIX) {
    stat.level = diagnostic_msgs::DiagnosticStatus::ERROR;
    stat.message = "No fix";
  } else if (last_nav_pos.fixType == 
             ublox_msgs::NavSTATUS::GPS_DEAD_RECKONING_ONLY) {
    stat.level = diagnostic_msgs::DiagnosticStatus::WARN;
    stat.message = "Dead reckoning only";
  } else if (last_nav_pos.fixType == ublox_msgs::NavSTATUS::GPS_2D_FIX) {
    stat.level = diagnostic_msgs::DiagnosticStatus::OK;
    stat.message = "2D fix";
  } else if (last_nav_pos.fixType == ublox_msgs::NavSTATUS::GPS_3D_FIX) {
    stat.level = diagnostic_msgs::DiagnosticStatus::OK;
    stat.message = "3D fix";
  } else if (last_nav_pos.fixType ==
             ublox_msgs::NavSTATUS::GPS_GPS_DEAD_RECKONING_COMBINED) {
    stat.level = diagnostic_msgs::DiagnosticStatus::OK;
    stat.message = "GPS and dead reckoning combined";
  } else if (last_nav_pos.fixType == ublox_msgs::NavSTATUS::GPS_TIME_ONLY_FIX) {
    stat.level = diagnostic_msgs::DiagnosticStatus::WARN;
    stat.message = "Time fix only";
  }

  //  append last fix position
  stat.add("iTOW", last_nav_pos.iTOW);
  stat.add("lon", last_nav_pos.lon);
  stat.add("lat", last_nav_pos.lat);
  stat.add("height", last_nav_pos.height);
  stat.add("hMSL", last_nav_pos.hMSL);
  stat.add("hAcc", last_nav_pos.hAcc);
  stat.add("vAcc", last_nav_pos.vAcc);
  stat.add("numSV", last_nav_pos.numSV);
}

int main(int argc, char** argv) {
  boost::asio::io_service io_service;
  ros::Timer poller;
  boost::shared_ptr<boost::asio::ip::tcp::socket> tcp_handle;
  boost::shared_ptr<boost::asio::serial_port> serial_handle;
  bool setup_ok = true;

  ros::init(argc, argv, "ublox_gps");
  nh.reset(new ros::NodeHandle("~"));
  if (!nh->hasParam("diagnostic_period")) {
    nh->setParam("diagnostic_period", 0.2);  //  5Hz diagnostic period
  }
  updater.reset(new diagnostic_updater::Updater());
  updater->setHardwareID("ublox");

  std::string device;
  int baudrate;
  int rate, meas_rate;
  bool enable_gps, enable_sbas, enable_galileo, enable_beidou, enable_imes; 
  bool enable_qzss, enable_glonass, enable_ppp;

  std::string dynamic_model, fix_mode;
  int dr_limit;
  int ublox_version;
  uint8_t num_trk_ch_use;
  int qzss_sig_cfg;
  int qzss_sig_cfg_default = ublox_msgs::CfgGNSS_Block::SIG_CFG_QZSS_L1CA;
  ros::NodeHandle param_nh("~");
  param_nh.param("device", device, std::string("/dev/ttyACM0"));
  param_nh.param("frame_id", frame_id, std::string("gps"));
  param_nh.param("baudrate", baudrate, 9600);
  param_nh.param("rate", rate, 4);  //  in Hz
  param_nh.param("enable_gps", enable_gps, true);
  param_nh.param("enable_sbas", enable_sbas, false);
  param_nh.param("enable_galileo", enable_galileo, false);
  param_nh.param("enable_beidou", enable_beidou, false);
  param_nh.param("enable_imes", enable_imes, false);
  param_nh.param("enable_qzss", enable_qzss, false);
  param_nh.param("qzss_sig_cfg", qzss_sig_cfg, 
                 qzss_sig_cfg_default);
  param_nh.param("enable_glonass", enable_glonass, false);
  param_nh.param("enable_ppp", enable_ppp, false);
  param_nh.param("dynamic_model", dynamic_model, std::string("portable"));
  param_nh.param("fix_mode", fix_mode, std::string("both"));
  param_nh.param("dr_limit", dr_limit, 0);
  param_nh.param("ublox_version", ublox_version, 6);
  // const uint8_t default_num_trk_ch_use = 0xFF;
  // param_nh.param("num_trk_ch_use", num_trk_ch_use, default_num_trk_ch_use);

  fix_status_service = sensor_msgs::NavSatStatus::SERVICE_GPS 
       + (enable_glonass ? 1 : 0) * sensor_msgs::NavSatStatus::SERVICE_GLONASS
       + (enable_beidou ? 1 : 0) * sensor_msgs::NavSatStatus::SERVICE_COMPASS
       + (enable_galileo ? 1 : 0) * sensor_msgs::NavSatStatus::SERVICE_GALILEO;

  if (enable_ppp) {
    ROS_WARN("Warning: PPP is enabled - this is an expert setting.");
  }

  if (rate <= 0) {
    ROS_ERROR("Invalid settings: rate must be > 0");
    return 1;
  }
  //  measurement rate param for ublox, units of ms
  meas_rate = 1000 / rate;

  if (dr_limit < 0 || dr_limit > 255) {
    ROS_ERROR("Invalid settings: dr_limit must be between 0 and 255");
    return 1;
  }

  DynamicModel dmodel;
  FixMode fmode;
  try {
    dmodel = ublox_gps::modelFromString(dynamic_model);
    fmode = ublox_gps::fixModeFromString(fix_mode);
  } catch (std::exception& e) {
    ROS_ERROR("Invalid settings: %s", e.what());
    return 1;
  }

  //  configure diagnostic updater for frequency
  updater->add("fix", &fix_diagnostic);
  updater->force_update();

  const double target_freq = 1000.0 / meas_rate;  //  actual update frequency
  double min_freq = target_freq;
  double max_freq = target_freq;
  diagnostic_updater::FrequencyStatusParam freq_param(&min_freq, &max_freq,
                                                      0.05, 10);
  diagnostic_updater::TimeStampStatusParam time_param(0,
                                                      meas_rate * 1e-3 * 0.05);
  freq_diag.reset(new diagnostic_updater::TopicDiagnostic(
      std::string("fix"), *updater, freq_param, time_param));

  boost::smatch match;
  if (boost::regex_match(device, match,
                         boost::regex("(tcp|udp)://(.+):(\\d+)"))) {
    std::string proto(match[1]);
    std::string host(match[2]);
    std::string port(match[3]);
    ROS_INFO("Connecting to %s://%s:%s ...", proto.c_str(), host.c_str(),
             port.c_str());

    if (proto == "tcp") {
      boost::asio::ip::tcp::resolver::iterator endpoint;

      try {
        boost::asio::ip::tcp::resolver resolver(io_service);
        endpoint =
            resolver.resolve(boost::asio::ip::tcp::resolver::query(host, port));
      } catch (std::runtime_error& e) {
        ROS_ERROR("Could not resolve %s:%s: %s", host.c_str(), port.c_str(),
                  e.what());
        return 1;  //  exit
      }

      boost::asio::ip::tcp::socket* socket =
          new boost::asio::ip::tcp::socket(io_service);
      tcp_handle.reset(socket);

      try {
        socket->connect(*endpoint);
      } catch (std::runtime_error& e) {
        ROS_ERROR("Could not connect to %s:%s: %s",
                  endpoint->host_name().c_str(),
                  endpoint->service_name().c_str(), e.what());
        return 1;  //  exit
      }

      ROS_INFO("Connected to %s:%s.", endpoint->host_name().c_str(),
               endpoint->service_name().c_str());
      gps.initialize(*socket, io_service);
    } else {
      ROS_ERROR("Protocol '%s' is unsupported", proto.c_str());
      return 1;  //  exit
    }
  } else {
    boost::asio::serial_port* serial = new boost::asio::serial_port(io_service);
    serial_handle.reset(serial);

    // open serial port
    try {
      serial->open(device);
    } catch (std::runtime_error& e) {
      ROS_ERROR("Could not open serial port %s: %s", device.c_str(), e.what());
      return 1;  //  exit
    }

    ROS_INFO("Opened serial port %s", device.c_str());
    gps.setBaudrate(baudrate);
    gps.initialize(*serial, io_service);
  }

  //  apply all requested settings
  try {
    if (!gps.isInitialized()) {
      throw std::runtime_error("Failed to initialize.");
    }
    ublox_msgs::MonVER monVer;
    if (gps.poll(monVer)) {
      ROS_INFO("Mon VER %s, %s", &(monVer.swVersion), &(monVer.hwVersion));
      for(std::size_t i = 0; i < monVer.extension.size(); ++i) {
        // TODO print in way that doesn't cause warning
        ROS_INFO("Mon VER %s, %s", &(monVer.extension[i]));
      }
    } else {
      ROS_WARN("failed to poll MonVER");
    }
    if (!gps.setMeasRate(meas_rate)) {
      std::stringstream ss;
      ss << "Failed to set measurement rate to " << meas_rate << "ms.";
      throw std::runtime_error(ss.str());
    }
    // if (!gps.enableSBAS(enable_sbas)) {
    //   throw std::runtime_error(std::string("Failed to ") +
    //                            ((enable_sbas) ? "enable" : "disable") +
    //                            " SBAS.");
    // }
    if (!gps.setPPPEnabled(enable_ppp)) {
      throw std::runtime_error(std::string("Failed to ") +
                               ((enable_ppp) ? "enable" : "disable") + " PPP.");
    }
    if (!gps.setDynamicModel(dmodel)) {
      throw std::runtime_error("Failed to set model: " + dynamic_model + ".");
    }
    if (!gps.setFixMode(fmode)) {
      throw std::runtime_error("Failed to set fix mode: " + fix_mode + ".");
    }
    if (!gps.setDeadReckonLimit(dr_limit)) {
      std::stringstream ss;
      ss << "Failed to set dead reckoning limit: " << dr_limit << ".";
      throw std::runtime_error(ss.str());
    }

    if (ublox_version == 7) {
      ublox_msgs::CfgGNSS cfgGNSSRead;
      if (gps.poll(cfgGNSSRead)) {
        ROS_INFO("Read GNSS config.");
        ROS_INFO("Num. tracking channels in hardware: %i", cfgGNSSRead.numTrkChHw);
        ROS_INFO("Num. tracking channels to use: %i", cfgGNSSRead.numTrkChUse);
      } else {
        throw std::runtime_error("Failed to read the GNSS config.");
      }

      ublox_msgs::CfgGNSS cfgGNSSWrite;
      cfgGNSSWrite.numConfigBlocks = 1;  // do services one by one
      cfgGNSSWrite.numTrkChHw = cfgGNSSRead.numTrkChHw;
      cfgGNSSWrite.numTrkChUse = cfgGNSSRead.numTrkChUse;
      cfgGNSSWrite.msgVer = 0;
      // configure glonass
      ublox_msgs::CfgGNSS_Block block;
      block.gnssId = block.GNSS_ID_GLONASS;
      block.resTrkCh = 8;  //  taken as defaults from ublox manual
      block.maxTrkCh = 14;
      block.flags = enable_glonass | block.SIG_CFG_GLONASS_L1OF;
      cfgGNSSWrite.blocks.push_back(block);
      if (!gps.configure(cfgGNSSWrite)) {
        throw std::runtime_error(std::string("Failed to ") +
                                 ((enable_glonass) ? "enable" : "disable") +
                                 " GLONASS.");
      }
      ROS_WARN("ublox_version < 8, ignoring BeiDou Settings");
    } else if(ublox_version >= 8) {
      ublox_msgs::CfgGNSS cfgGNSSRead;
      if (gps.poll(cfgGNSSRead)) {
        ROS_INFO("Read GNSS config.");
        ROS_INFO("Num. tracking channels in hardware: %i", cfgGNSSRead.numTrkChHw);
        ROS_INFO("Num. tracking channels to use: %i", cfgGNSSRead.numTrkChUse);
        for(std::size_t i = 0; i < cfgGNSSRead.blocks.size(); ++i) {
          bool enabled = cfgGNSSRead.blocks[i].flags 
                         &  ublox_msgs::CfgGNSS_Block::FLAGS_ENABLE;
          uint32_t sigCfg = cfgGNSSRead.blocks[i].flags 
                        & ublox_msgs::CfgGNSS_Block::FLAGS_SIG_CFG_MASK;
          ROS_INFO("gnssId, enabled, resTrkCh, maxTrkCh: %u, %u, %u, %u, %u",
                   cfgGNSSRead.blocks[i].gnssId,
                   enabled,
                   cfgGNSSRead.blocks[i].resTrkCh,
                   cfgGNSSRead.blocks[i].maxTrkCh,
                   sigCfg
                   );
        }
      } else {
        throw std::runtime_error("Failed to read the GNSS config.");
      }

      ublox_msgs::CfgGNSS cfgGNSSWrite;
      cfgGNSSWrite.numTrkChHw = cfgGNSSRead.numTrkChHw;
      cfgGNSSWrite.numTrkChUse = cfgGNSSRead.numTrkChUse;
      cfgGNSSWrite.msgVer = 0;
      cfgGNSSWrite.numTrkChUse = 28;
      // Configure GPS
      ublox_msgs::CfgGNSS_Block gps_block;
      gps_block.gnssId = gps_block.GNSS_ID_GPS;
      gps_block.resTrkCh = gps_block.RES_TRK_CH_GPS;
      gps_block.maxTrkCh = gps_block.MAX_TRK_CH_GPS;
      gps_block.flags = enable_gps | gps_block.SIG_CFG_GPS_L1CA;
      cfgGNSSWrite.blocks.push_back(gps_block);
      // Configure SBAS
      ublox_msgs::CfgGNSS_Block sbas_block;
      sbas_block.gnssId = sbas_block.GNSS_ID_SBAS;
      sbas_block.maxTrkCh = sbas_block.MAX_TRK_CH_MAJOR_MIN;
      sbas_block.flags = enable_sbas | sbas_block.SIG_CFG_SBAS_L1CA;
      cfgGNSSWrite.blocks.push_back(sbas_block);
      // Configure Galileo
      ublox_msgs::CfgGNSS_Block galileo_block;
      galileo_block.gnssId = galileo_block.GNSS_ID_GALILEO;
      galileo_block.maxTrkCh = galileo_block.MAX_TRK_CH_MAJOR_MIN;
      galileo_block.flags = enable_galileo | galileo_block.SIG_CFG_GALILEO_E1OS;
      cfgGNSSWrite.blocks.push_back(galileo_block);
      // Configure Beidou
      ublox_msgs::CfgGNSS_Block beidou_block;
      beidou_block.gnssId = beidou_block.GNSS_ID_BEIDOU;
      beidou_block.maxTrkCh = beidou_block.MAX_TRK_CH_MAJOR_MIN;
      beidou_block.flags = enable_beidou | beidou_block.SIG_CFG_BEIDOU_B1I;
      cfgGNSSWrite.blocks.push_back(beidou_block);
      // Configure IMES
      ublox_msgs::CfgGNSS_Block imes_block;
      imes_block.gnssId = imes_block.GNSS_ID_IMES;
      imes_block.maxTrkCh = imes_block.MAX_TRK_CH_MAJOR_MIN;
      imes_block.flags = enable_imes | imes_block.SIG_CFG_IMES_L1;
      cfgGNSSWrite.blocks.push_back(imes_block);
      // Configure QZSS
      ublox_msgs::CfgGNSS_Block qzss_block;
      qzss_block.gnssId = qzss_block.GNSS_ID_QZSS;
      qzss_block.resTrkCh = qzss_block.RES_TRK_CH_QZSS;
      qzss_block.maxTrkCh = qzss_block.MAX_TRK_CH_QZSS;
      qzss_block.flags = enable_qzss | qzss_sig_cfg; 
      cfgGNSSWrite.blocks.push_back(qzss_block);
      // Configure GLONASS
      ublox_msgs::CfgGNSS_Block glonass_block;
      glonass_block.gnssId = glonass_block.GNSS_ID_GLONASS;
      glonass_block.resTrkCh = glonass_block.RES_TRK_CH_GLONASS;
      glonass_block.maxTrkCh = glonass_block.MAX_TRK_CH_GLONASS;
      glonass_block.flags = enable_glonass | glonass_block.SIG_CFG_GLONASS_L1OF;
      cfgGNSSWrite.blocks.push_back(glonass_block);
      cfgGNSSWrite.numConfigBlocks = cfgGNSSWrite.blocks.size(); 
      if (!gps.configure(cfgGNSSWrite)) {
        throw std::runtime_error(std::string("Failed to Configure GNSS"));
      }
    } else {
      ROS_WARN("ublox_version < 7, ignoring GNSS settings");
    }
  } catch (std::exception& e) {
    setup_ok = false;
    ROS_ERROR("Error configuring device: %s", e.what());
  }

  if (setup_ok) {
    ROS_INFO("U-Blox configured successfully.");

    // subscribe messages
    param_nh.param("all", enabled["all"], false);
    param_nh.param("rxm", enabled["rxm"], false);
    param_nh.param("aid", enabled["aid"], false);

    param_nh.param("nav_sol", enabled["nav_sol"], true);
    if (enabled["nav_sol"])
      gps.subscribe<ublox_msgs::NavSOL>(
          boost::bind(&publish<ublox_msgs::NavSOL>, _1, "navsol"), 1);
    
    param_nh.param("nav_pvt", enabled["nav_pvt"], true);
    if (enabled["nav_pvt"])
      gps.subscribe<ublox_msgs::NavPVT>(&publishNavPVT, 1);
    
    param_nh.param("nav_status", enabled["nav_status"], true);
    if (enabled["nav_status"])
      gps.subscribe<ublox_msgs::NavSTATUS>(
          boost::bind(&publish<ublox_msgs::NavSTATUS>, _1, "navstatus"), 1);
    
    param_nh.param("nav_svinfo", enabled["nav_svinfo"], enabled["all"]);
    if (enabled["nav_svinfo"])
      gps.subscribe<ublox_msgs::NavSVINFO>(
          boost::bind(&publish<ublox_msgs::NavSVINFO>, _1, "navsvinfo"), 20);
    
    param_nh.param("nav_clk", enabled["nav_clk"], enabled["all"]);
    if (enabled["nav_clk"])
      gps.subscribe<ublox_msgs::NavCLOCK>(
          boost::bind(&publish<ublox_msgs::NavCLOCK>, _1, "navclock"), 1);
    
    param_nh.param("rxm_raw", enabled["rxm_raw"],
                   enabled["all"] || enabled["rxm"]);
    if (enabled["rxm_raw"]) {
      if(ublox_version >= 8)
        gps.subscribe<ublox_msgs::RxmRAWX>(
            boost::bind(&publish<ublox_msgs::RxmRAWX>, _1, "rxmraw"), 1);
      else
        gps.subscribe<ublox_msgs::RxmRAW>(
            boost::bind(&publish<ublox_msgs::RxmRAW>, _1, "rxmraw"), 1);
    }

    param_nh.param("rxm_sfrb", enabled["rxm_sfrb"],
                   enabled["all"] || enabled["rxm"]);
    if (enabled["rxm_sfrb"]) {
      if(ublox_version >= 8)
        gps.subscribe<ublox_msgs::RxmSFRBX>(
            boost::bind(&publish<ublox_msgs::RxmSFRBX>, _1, "rxmsfrb"), 1);
      else
        gps.subscribe<ublox_msgs::RxmSFRB>(
            boost::bind(&publish<ublox_msgs::RxmSFRB>, _1, "rxmsfrb"), 1);
    }

    param_nh.param("rxm_eph", enabled["rxm_eph"],
                   enabled["all"] || enabled["rxm"]);
    if (enabled["rxm_eph"])
      gps.subscribe<ublox_msgs::RxmEPH>(
          boost::bind(&publish<ublox_msgs::RxmEPH>, _1, "rxmeph"), 1);

    param_nh.param("rxm_alm", enabled["rxm_alm"],
                   enabled["all"] || enabled["rxm"]);
    if (enabled["rxm_alm"])
      gps.subscribe<ublox_msgs::RxmALM>(
          boost::bind(&publish<ublox_msgs::RxmALM>, _1, "rxmalm"), 1);
    
    param_nh.param("nav_posllh", enabled["nav_posllh"], true);
    if (enabled["nav_posllh"])
      gps.subscribe<ublox_msgs::NavPOSLLH>(
          boost::bind(&publish<ublox_msgs::NavPOSLLH>, _1, "navposllh"), 1);
    
    param_nh.param("nav_posecef", enabled["nav_posecef"], true);
    if (enabled["nav_posecef"])
      gps.subscribe<ublox_msgs::NavPOSECEF>(
          boost::bind(&publish<ublox_msgs::NavPOSECEF>, _1, "navposecef"), 1);
    
    param_nh.param("nav_velned", enabled["nav_velned"], true);
    if (enabled["nav_velned"])
      gps.subscribe<ublox_msgs::NavVELNED>(
          boost::bind(&publish<ublox_msgs::NavVELNED>, _1, "navvelned"), 1);
    
    param_nh.param("aid_alm", enabled["aid_alm"],
                   enabled["all"] || enabled["aid"]);
    if (enabled["aid_alm"]) 
      gps.subscribe<ublox_msgs::AidALM>(
          boost::bind(&publish<ublox_msgs::AidALM>, _1, "aidalm"), 1);
    
    param_nh.param("aid_eph", enabled["aid_eph"],
                   enabled["all"] || enabled["aid"]);
    if (enabled["aid_eph"]) 
      gps.subscribe<ublox_msgs::AidEPH>(
          boost::bind(&publish<ublox_msgs::AidEPH>, _1, "aideph"), 1);
    
    param_nh.param("aid_hui", enabled["aid_hui"],
                   enabled["all"] || enabled["aid"]);
    if (enabled["aid_hui"]) 
      gps.subscribe<ublox_msgs::AidHUI>(
          boost::bind(&publish<ublox_msgs::AidHUI>, _1, "aidhui"), 1);

    poller = nh->createTimer(ros::Duration(1.0), &pollMessages);
    poller.start();
    ros::spin();
  }

  if (gps.isInitialized()) {
    gps.close();
    ROS_INFO("Closed connection to %s.", device.c_str());
  }
  return 0;
}
