////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2020 NovAtel Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include <novatel_oem7_driver/oem7_message_handler_if.hpp>

#include <ros/ros.h>


#include <tf2_geometry_msgs/tf2_geometry_msgs.h>


#include <novatel_oem7_driver/oem7_ros_messages.hpp>

#include "sensor_msgs/Imu.h"
#include "novatel_oem7_msgs/CORRIMU.h"
#include "novatel_oem7_msgs/IMURATECORRIMU.h"
#include "novatel_oem7_msgs/INSSTDEV.h"
#include "novatel_oem7_msgs/INSCONFIG.h"
#include "novatel_oem7_msgs/INSPVA.h"
#include "novatel_oem7_msgs/INSPVAX.h"

#include <boost/scoped_ptr.hpp>
#include <oem7_ros_publisher.hpp>

#include <math.h>
#include <map>

namespace
{
  typedef unsigned int imu_type_t; ///< Type of IMU used
  typedef int imu_rate_t; ///< IMU message rate

  const imu_type_t IMU_TYPE_UNKNOWN = 0;
}



namespace novatel_oem7_driver
{
  /***
   * Converts degrees to Radians
   *
   * @return radians
   */
  inline double degreesToRadians(double degrees)
  {
    return degrees * M_PI / 180.0;
  }

  const double DATA_NOT_AVAILABLE = -1.0; ///< Used to initialized unpopulated fields.

  class INSHandler: public Oem7MessageHandlerIf
  {
    ros::NodeHandle nh_;

    Oem7RosPublisher       imu_pub_;
    Oem7RosPublisher       corrimu_pub_;
    Oem7RosPublisher       insstdev_pub_;
    Oem7RosPublisher       inspvax_pub_;
    Oem7RosPublisher       insconfig_pub_;

    boost::shared_ptr<novatel_oem7_msgs::INSPVA>   inspva_;
    boost::shared_ptr<novatel_oem7_msgs::CORRIMU>  corrimu_;
    boost::shared_ptr<novatel_oem7_msgs::INSSTDEV> insstdev_;

    int imu_rate_;
    std::string frame_id_;

    typedef std::map<std::string, std::string> imu_config_map_t;
    imu_config_map_t imu_config_map;


    void getImuParam(imu_type_t imu_type, const std::string& name, std::string& param)
    {
      std::string ns = ros::this_node::getNamespace();
      std::string param_name = ns + "/supported_imus/" + std::to_string(imu_type) + "/" + name;
      if(!nh_.getParam(param_name, param))
      {
        ROS_FATAL_STREAM("INS: IMU type= " << imu_type << " is not supported.");
      }
    }

    int getImuRate(imu_type_t imu_type)
    {
      std::string rate;
      getImuParam(imu_type, "rate", rate);

      return std::stoi(rate);
    }

    void getImuDescription(imu_type_t imu_type, std::string& desc)
    {
      getImuParam(imu_type, "name", desc);
    }


    void processInsConfigMsg(Oem7RawMessageIf::ConstPtr msg)
    {
      boost::shared_ptr<novatel_oem7_msgs::INSCONFIG> insconfig;
      MakeROSMessage(msg, insconfig);
      insconfig_pub_.publish(insconfig);

      if(imu_rate_ == 0)
      {
        std::string imu_desc;
        getImuDescription(insconfig->imu_type, imu_desc);

        imu_rate_ = getImuRate(insconfig->imu_type);

        ROS_LOG_STREAM(imu_rate_ == 0 ? ::ros::console::levels::Error :
                                        ::ros::console::levels::Info,
                       ROSCONSOLE_DEFAULT_NAME,
                       "IMU: '" << imu_desc << "', rate= " << imu_rate_);
      }
    }

    void publishInsPVAXMsg(Oem7RawMessageIf::ConstPtr msg)
    {
      boost::shared_ptr<novatel_oem7_msgs::INSPVAX> inspvax;
      MakeROSMessage(msg, inspvax);

      inspvax_pub_.publish(inspvax);
    }

    void publishCorrImuMsg(Oem7RawMessageIf::ConstPtr msg)
    {
      MakeROSMessage(msg, corrimu_);
      corrimu_pub_.publish(corrimu_);
    }


    void publishImuMsg()
    {
      if(!imu_pub_.isEnabled())
      {
        return;
      }

      boost::shared_ptr<sensor_msgs::Imu> imu(new sensor_msgs::Imu);

      if(inspva_)
      {
        tf2::Quaternion tf_orientation;
        tf_orientation.setRPY(
                           degreesToRadians(inspva_->roll),
                          -degreesToRadians(inspva_->pitch),
                          -degreesToRadians(inspva_->azimuth));
        imu->orientation = tf2::toMsg(tf_orientation);
      }
      else
      {
        ROS_WARN_THROTTLE(10, "INSPVA not available; 'Imu' message not generated.");
        return;
      }

      if(insstdev_)
      {
        imu->orientation_covariance[0] = std::pow(insstdev_->pitch_stdev,   2);
        imu->orientation_covariance[4] = std::pow(insstdev_->roll_stdev,    2);
        imu->orientation_covariance[8] = std::pow(insstdev_->azimuth_stdev, 2);
      }

      if(corrimu_ && imu_rate_ > 0)
      {
        imu->angular_velocity.x = corrimu_->pitch_rate * imu_rate_;
        imu->angular_velocity.y = corrimu_->roll_rate  * imu_rate_;
        imu->angular_velocity.z = corrimu_->yaw_rate   * imu_rate_;

        imu->linear_acceleration.x = corrimu_->lateral_acc      * imu_rate_;
        imu->linear_acceleration.y = corrimu_->longitudinal_acc * imu_rate_;
        imu->linear_acceleration.z = corrimu_->vertical_acc     * imu_rate_;


        imu->angular_velocity_covariance[0]    = 1e-3;
        imu->angular_velocity_covariance[4]    = 1e-3;
        imu->angular_velocity_covariance[8]    = 1e-3;


        imu->linear_acceleration_covariance[0] = 1e-3;
        imu->linear_acceleration_covariance[4] = 1e-3;
        imu->linear_acceleration_covariance[8] = 1e-3;
      }
      else
      {
        imu->angular_velocity_covariance[0]    = DATA_NOT_AVAILABLE;
        imu->linear_acceleration_covariance[0] = DATA_NOT_AVAILABLE;
      }

      imu_pub_.publish(imu);
    }

    void publishInsStDevMsg(Oem7RawMessageIf::ConstPtr msg)
    {
      MakeROSMessage(msg, insstdev_);
      insstdev_pub_.publish(insstdev_);
    }


  public:
    INSHandler():
      imu_rate_(0)
    {
    }

    ~INSHandler()
    {
    }

    void initialize(ros::NodeHandle& nh)
    {
      nh_ = nh;

      imu_pub_.setup<sensor_msgs::Imu>(                  "IMU",        nh);
      corrimu_pub_.setup<  novatel_oem7_msgs::CORRIMU>(  "CORRIMU",    nh);
      insstdev_pub_.setup< novatel_oem7_msgs::INSSTDEV>( "INSSTDEV",   nh);
      inspvax_pub_.setup<  novatel_oem7_msgs::INSPVAX>(  "INSPVAX",    nh);
      insconfig_pub_.setup<novatel_oem7_msgs::INSCONFIG>("INSCONFIG",  nh);

      nh.getParam("imu_rate", imu_rate_); // User rate override
      if(imu_rate_ > 0)
      {
        ROS_INFO_STREAM("INS: IMU rate overriden to " << imu_rate_);
      }
    }

    const std::vector<int>& getMessageIds()
    {
      static const std::vector<int> MSG_IDS(
                                      {
                                        CORRIMUS_OEM7_MSGID,
                                        IMURATECORRIMUS_OEM7_MSGID,
                                        INSPVAS_OEM7_MSGID,
                                        INSPVAX_OEM7_MSGID,
                                        INSSTDEV_OEM7_MSGID,
                                        INSCONFIG_OEM7_MSGID
                                      }
                                    );
      return MSG_IDS;
    }

    void handleMsg(Oem7RawMessageIf::ConstPtr msg)
    {
      ROS_DEBUG_STREAM("INS < [id= " <<  msg->getMessageId() << "]");

      if(msg->getMessageId()== INSPVAS_OEM7_MSGID)
      {
        MakeROSMessage(msg, inspva_); // Cache
      }
      else if(msg->getMessageId() == INSSTDEV_OEM7_MSGID)
      {
        publishInsStDevMsg(msg);
      }
      else if(msg->getMessageId() == CORRIMUS_OEM7_MSGID ||
              msg->getMessageId() == IMURATECORRIMUS_OEM7_MSGID)
      {
        publishCorrImuMsg(msg);

        publishImuMsg();
      }
      else if(msg->getMessageId() == INSCONFIG_OEM7_MSGID)
      {
        processInsConfigMsg(msg);
      }
      else if(msg->getMessageId() == INSPVAX_OEM7_MSGID)
      {
        publishInsPVAXMsg(msg);
      }
      else
      {
        assert(false);
      }
    }
  };

}

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(novatel_oem7_driver::INSHandler, novatel_oem7_driver::Oem7MessageHandlerIf)
