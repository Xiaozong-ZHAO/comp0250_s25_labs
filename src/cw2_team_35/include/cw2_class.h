#ifndef CW2_CLASS_H_
#define CW2_CLASS_H_

// ROS
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/PointStamped.h>
#include <ros/package.h>
// MoveIt
#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/planning_interface/planning_interface.h>

// PCL / TF
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/point_types.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/voxel_grid.h>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

// 特征估计 & 识别
#include <pcl/features/normal_3d_omp.h>
#include <pcl/features/board.h>
#include <pcl/features/shot_omp.h>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/recognition/cg/hough_3d.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/registration/icp.h>
#include <pcl/common/transforms.h>
#include <Eigen/Core>

// Service definitions
#include "cw2_world_spawner/Task1Service.h"
#include "cw2_world_spawner/Task2Service.h"
#include "cw2_world_spawner/Task3Service.h"

// PCL 类型简化别名
typedef pcl::PointCloud<pcl::PointXYZRGB> PointCloudRGB;
typedef pcl::PointCloud<pcl::PointXYZ> PointCloudXYZ;
typedef pcl::SHOT352 DescriptorType;
typedef pcl::PointCloud<DescriptorType> DescriptorCloud;
typedef pcl::Normal NormalType;

class cw2
{
public:
  cw2(ros::NodeHandle nh);

  // 三个任务的服务回调
  bool t1_callback(cw2_world_spawner::Task1Service::Request &request,
                   cw2_world_spawner::Task1Service::Response &response);
  bool t2_callback(cw2_world_spawner::Task2Service::Request &request,
                   cw2_world_spawner::Task2Service::Response &response);
  bool t3_callback(cw2_world_spawner::Task3Service::Request &request,
                   cw2_world_spawner::Task3Service::Response &response);

  // 控制函数
  bool move_arm(const geometry_msgs::PointStamped& target);
  bool move_gripper(float width);
  bool rotate_end_effector(double yaw);

  // 点云回调
  void pointCloudCallback(const sensor_msgs::PointCloud2ConstPtr& msg);

  // ============== 模板版过滤函数 ==============
  template <typename PointT>
  typename pcl::PointCloud<PointT>::Ptr
  filterPassThrough(const typename pcl::PointCloud<PointT>::Ptr& cloud_in,
                    const std::string& field_name = "z",
                    float limit_min = 0.0, 
                    float limit_max = 0.4);

  template <typename PointT>
  typename pcl::PointCloud<PointT>::Ptr
  filterVoxelGrid(const typename pcl::PointCloud<PointT>::Ptr& cloud_in,
                  float leaf_size = 0.001f);

  // ============== 模板版发布函数 ==============
  template <typename PointT>
  void publishCloud(const typename pcl::PointCloud<PointT>::Ptr& cloud,
                    const std::string& frame_id);

  // ============== 模板版位姿估计函数(示例) ==============
  template <typename PointT>
  bool estimatePoseByMatchingGeneric(
      const typename pcl::PointCloud<PointT>::Ptr& scene_cloud,
      const std::string& model_path,
      Eigen::Matrix4f& transform_out);

private:
  ros::NodeHandle nh_;
  // 服务
  ros::ServiceServer t1_service_;
  ros::ServiceServer t2_service_;
  ros::ServiceServer t3_service_;

  // 点云相关
  ros::Subscriber cloud_sub_;
  ros::Publisher filtered_cloud_pub_;

  // 分别保存 RGB 和 XYZ 的最新点云
  PointCloudRGB::Ptr latest_cloud_rgb;
  PointCloudXYZ::Ptr latest_cloud_xyz;
  PointCloudXYZ::Ptr model_cloud;

  bool cloud_received_;
  tf2_ros::Buffer tf_buffer_;
  tf2_ros::TransformListener tf_listener_;

  // MoveIt groups
  moveit::planning_interface::MoveGroupInterface arm_group_;
  moveit::planning_interface::MoveGroupInterface hand_group_;
};

#include "cw2_class_impl.hpp" // 建议把模板实现放到 .hpp 或 .ipp 里

#endif // CW2_CLASS_H_
