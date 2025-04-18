#include <cw2_class.h>
#include <geometry_msgs/Pose.h>

// PCL transform utils
#include <pcl/common/transforms.h>
#include <pcl_ros/transforms.h>  // 如果你需要 transformPointCloud

///////////////////////////////////////////////////////////////////////////////
// 构造函数
///////////////////////////////////////////////////////////////////////////////
cw2::cw2(ros::NodeHandle nh)
  : nh_(nh),
    arm_group_("panda_arm"),
    hand_group_("hand"),
    tf_listener_(tf_buffer_),
    cloud_received_(false)
{
  // 注册服务
  t1_service_ = nh_.advertiseService("/task1_start", &cw2::t1_callback, this);
  t2_service_ = nh_.advertiseService("/task2_start", &cw2::t2_callback, this);
  t3_service_ = nh_.advertiseService("/task3_start", &cw2::t3_callback, this);

  // 发布滤波后点云
  filtered_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>(
      "/r200/camera/depth_registered/filtered_cloud", 1, true);
  // 订阅点云
  cloud_sub_ = nh_.subscribe("/r200/camera/depth_registered/points", 1,
    &cw2::pointCloudCallback, this);
  marker_pub_ = nh_.advertise<visualization_msgs::Marker>("/convex_hull_marker", 1);
  grasp_point_pub_ = nh_.advertise<visualization_msgs::Marker>("/grasp_point_marker",1);
  grasp_arrow_pub_ = nh_.advertise<visualization_msgs::Marker>("/grasp_orientation_arrow", 1);
  centroid_pub_ = nh_.advertise<visualization_msgs::Marker>("/centroid_marker", 1);
  octomap_pub_ = nh_.advertise<octomap_msgs::Octomap>("/constructed_octomap", 1, true);
  accumulated_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("/accumulated_cloud", 1, true);
  cluster_marker_pub_ = nh_.advertise<visualization_msgs::MarkerArray>("/cluster_centroids", 1);
  basket_pub_ = nh_.advertise<visualization_msgs::Marker>("/basket_marker", 1);
  latest_cloud_rgb.reset(new pcl::PointCloud<pcl::PointXYZRGB>);
  latest_cloud_xyz.reset(new pcl::PointCloud<pcl::PointXYZ>);
  model_cloud.reset(new pcl::PointCloud<pcl::PointXYZ>);
  accumulated_cloud_.reset(new pcl::PointCloud<pcl::PointXYZRGB>);
  srand(static_cast<unsigned>(time(nullptr)));
}

///////////////////////////////////////////////////////////////////////////////
// Task 1 回调函数
///////////////////////////////////////////////////////////////////////////////
bool cw2::t1_callback(cw2_world_spawner::Task1Service::Request &request,
                      cw2_world_spawner::Task1Service::Response &response)
{
  ROS_INFO("=== Task 1 Callback Triggered ===");

  // 1. 保存 shape_type，用于加载模型
  std::string pkg_path = ros::package::getPath("cw2_team_35");
  std::string model_path;

  // 2. 移动到物体正上方
  if (!move_to_pose(request.object_point, 0.5,true)) {
    ROS_ERROR("Failed to move above the object.");
    return false;
  }

  // 3. 等待点云
  ros::Rate rate(10);
  int wait = 0;
  while (!cloud_received_ && wait++ < 50) {
    ROS_INFO_THROTTLE(1.0, "Waiting for point cloud...");
    rate.sleep();
  }
  if (!cloud_received_) {
    ROS_ERROR("No point cloud received after arm moved.");
    return false;
  }

  auto pass_filtered = filterPassThrough<pcl::PointXYZRGB>(
      latest_cloud_rgb,    // 输入点云
      "z",                 // 过滤字段
      0.0f,                // z最小
      0.4f                 // z最大
  );

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed(new pcl::PointCloud<pcl::PointXYZRGB>);
  pcl_ros::transformPointCloud("world", *pass_filtered, *transformed, tf_buffer_);

  // 发布滤波后的点云
  publishCloud<pcl::PointXYZRGB>(transformed, "world");
  auto [hull, polygon] = computeConvexHull(transformed);
  std::vector<geometry_msgs::PointStamped> corners = extract_corner_points(hull, polygon, 1, "world");
  publishConvexHullMarker(corners, marker_pub_, 0);
  geometry_msgs::Point centroid = computeCentroid(hull);
  geometry_msgs::PointStamped grasp_point = computeGraspPoint(centroid, corners, request.shape_type);
  publishGraspPointMarker(grasp_point, grasp_point_pub_, 1);
  geometry_msgs::Vector3 placement_offset = computeOffsetVector(grasp_point, centroid);
  geometry_msgs::PointStamped place_point = computeAdjustedPlacementPoint(request.goal_point, placement_offset);

  double yaw = compute_orientation(hull, grasp_point);
  double yaw_offset = compute_yaw_offset(request.shape_type);
  publishOrientationArrow(grasp_point, yaw, grasp_arrow_pub_, 1);

  if (!cartesian_grasp_and_place(grasp_point, place_point, yaw+yaw_offset)) {
    ROS_ERROR("Cartesian grasping failed!");
    return false;
  }

  return true;
}

bool cw2::cartesian_grasp_and_place(
  const geometry_msgs::PointStamped& grasp_point,
  const geometry_msgs::PointStamped& place_point,
  double rotate_angle)
{
  // 1. 打开夹爪
  move_gripper(0.10);

  // 2. 移动到抓取点上方
  if (!move_to_pose(grasp_point, 0.15, false)) {
    ROS_ERROR("Failed to move above the object.");
    return false;
  }

  // 3. 旋转末端执行器
  if (!rotate_end_effector(rotate_angle)) {
    ROS_ERROR("Failed to rotate end-effector.");
    return false;
  }

  // 4. 降低准备抓取
  if (!move_to_pose(grasp_point, 0.08, false)) {
    ROS_ERROR("Failed to move to grasp height.");
    return false;
  }

  // 5. 闭合夹爪
  move_gripper(0.03);

  // 6. 记录当前抓取后的姿态作为统一姿态
  geometry_msgs::PoseStamped base_pose_stamped = arm_group_.getCurrentPose();
  geometry_msgs::Pose base_pose = base_pose_stamped.pose;

  // 7. 构造 Cartesian waypoints
  std::vector<geometry_msgs::Pose> waypoints;

  geometry_msgs::Pose lift_pose = base_pose;
  lift_pose.position.z = grasp_point.point.z + 0.2;
  lift_pose.orientation = base_pose.orientation;
  waypoints.push_back(lift_pose);

  geometry_msgs::Pose move_y_pose = lift_pose;
  move_y_pose.position.y = place_point.point.y;
  move_y_pose.orientation = base_pose.orientation;
  waypoints.push_back(move_y_pose);

  geometry_msgs::Pose move_x_pose = move_y_pose;
  move_x_pose.position.x = place_point.point.x;
  move_x_pose.orientation = base_pose.orientation;
  waypoints.push_back(move_x_pose);

  geometry_msgs::Pose lower_pose = move_x_pose;
  lower_pose.position.z = place_point.point.z + 0.2;
  lower_pose.orientation = base_pose.orientation;
  waypoints.push_back(lower_pose);

  // 8. 计算笛卡尔路径（只尝试一次）
  const double eef_step = 0.01;
  const double jump_threshold = 0.0;

  moveit_msgs::RobotTrajectory trajectory;
  double fraction = arm_group_.computeCartesianPath(waypoints, eef_step, trajectory, true);
  if (fraction >= 0.95) {
    moveit::planning_interface::MoveGroupInterface::Plan plan;
    plan.trajectory_ = trajectory;
    arm_group_.execute(plan);
  } else {
    ROS_WARN("Cartesian path planning failed (%.2f%%). Falling back to point-to-point planning.", fraction * 100.0);

    // fallback：使用 move_to_pose 分段移动
    // geometry_msgs::PointStamped mid1 = place_point;
    geometry_msgs::PointStamped mid1 = grasp_point;
    mid1.point.z = grasp_point.point.z + 0.2;

    geometry_msgs::PointStamped mid2 = place_point;
    mid2.point.z = place_point.point.z + 0.2;

    if (!move_to_pose(mid1, 0.0, false)) {
      ROS_ERROR("Fallback move_to_pose: mid1 failed.");
      return false;
    }

    if (!move_to_pose(mid2, 0.0, false)) {
      ROS_ERROR("Fallback move_to_pose: mid2 failed.");
      return false;
    }
  }

  // 9. 放下物体
  move_gripper(0.10);

  // 10. 抬起离开
  geometry_msgs::PointStamped lift_after_place = place_point;
  lift_after_place.point.z = place_point.point.z + 0.5;

  if (!move_to_pose(lift_after_place, 0.0, false)) {
    ROS_ERROR("Failed to lift after placing.");
    return false;
  }

  return true;
}




geometry_msgs::Vector3 cw2::computeOffsetVector(
  const geometry_msgs::PointStamped& grasp_point,
  const geometry_msgs::Point& centroid)
{
  geometry_msgs::Vector3 offset;
  offset.x =  grasp_point.point.x - centroid.x;
  offset.y =  grasp_point.point.y - centroid.y;
  offset.z =  grasp_point.point.z - centroid.z;
  return offset;
}

geometry_msgs::PointStamped cw2::computeAdjustedPlacementPoint(
  const geometry_msgs::PointStamped& original_place_point,
  const geometry_msgs::Vector3& offset)
{
  geometry_msgs::PointStamped adjusted;
  adjusted.header = original_place_point.header;
  adjusted.point.x = original_place_point.point.x + offset.x;
  adjusted.point.y = original_place_point.point.y + offset.y;
  adjusted.point.z = original_place_point.point.z + offset.z;
  return adjusted;
}

geometry_msgs::Point cw2::computeCentroid(const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& hull)
{
  geometry_msgs::Point centroid_point;
  Eigen::Vector4f centroid;
  pcl::compute3DCentroid(*hull, centroid);

  centroid_point.x = centroid[0];
  centroid_point.y = centroid[1];
  centroid_point.z = centroid[2];

  return centroid_point;
}

geometry_msgs::PointStamped cw2::computeGraspPoint(
  const geometry_msgs::Point& centroid,
  const std::vector<geometry_msgs::PointStamped>& corners,
  const std::string& shape_type)
{
  geometry_msgs::PointStamped grasp_point;
  grasp_point.header.frame_id = "world";
  grasp_point.header.stamp = ros::Time::now();

  if (shape_type == "nought") {
    geometry_msgs::Point y_max_pt;
    float max_y = -std::numeric_limits<float>::max();
    for (const auto& pt : corners) {
      if (pt.point.y > max_y) {
        max_y = pt.point.y;
        y_max_pt = pt.point;
      }
    }

    Eigen::Vector2f vec(y_max_pt.x - centroid.x,
                        y_max_pt.y - centroid.y);

    float theta = M_PI / 4.0f;
    Eigen::Matrix2f R;
    R << std::cos(theta), -std::sin(theta),
         std::sin(theta),  std::cos(theta);

    Eigen::Vector2f rotated = R * vec * (std::sqrt(2.0f) / 2.0f);

    grasp_point.point.x = centroid.x + 0.95 * rotated[0];
    grasp_point.point.y = centroid.y + 0.95 * rotated[1];
    grasp_point.point.z = centroid.z + 0.015;

  } else if (shape_type == "cross") {
    geometry_msgs::Point y_max_pt;
    float max_y = -std::numeric_limits<float>::max();
    for (const auto& pt : corners) {
      if (pt.point.y > max_y) {
        max_y = pt.point.y;
        y_max_pt = pt.point;
      }
    }

    grasp_point.point.x = (centroid.x + y_max_pt.x) / 2.0;
    grasp_point.point.y = (centroid.y + y_max_pt.y) / 2.0;
    grasp_point.point.z = centroid.z + 0.015;
  }

  return grasp_point;
}


double cw2::compute_orientation(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& hull,
  const geometry_msgs::PointStamped& grasp_point)
{
  Eigen::Vector4f centroid;
  pcl::compute3DCentroid(*hull, centroid);

  double dx = grasp_point.point.x - centroid[0];
  double dy = grasp_point.point.y - centroid[1];

  double yaw = std::atan2(dy, dx);
  return yaw;
}


double cw2::compute_yaw_offset(const std::string& shape_type)
{
  if (shape_type == "nought") {
    return M_PI / 4.0;
  } else if (shape_type == "cross") {
    return -M_PI / 4.0;
  } else {
    return 0.0;  // 默认无偏移
  }
}

void cw2::publishGraspPointMarker(
  const geometry_msgs::PointStamped& grasp_point,
  ros::Publisher& marker_pub,
  int id)
{
  visualization_msgs::Marker marker;
  marker.header.frame_id = grasp_point.header.frame_id;
  marker.header.stamp = ros::Time::now();  // 可也用 grasp_point.header.stamp
  marker.ns = "grasp_point";
  marker.id = id;
  marker.type = visualization_msgs::Marker::SPHERE;
  marker.action = visualization_msgs::Marker::ADD;

  marker.pose.position = grasp_point.point;
  marker.pose.orientation.w = 1.0;

  marker.scale.x = 0.02;
  marker.scale.y = 0.02;
  marker.scale.z = 0.02;

  marker.color.r = 1.0;
  marker.color.g = 1.0;
  marker.color.b = 1.0;
  marker.color.a = 1.0;

  marker.lifetime = ros::Duration(0);  // 永久显示

  marker_pub.publish(marker);
}


void cw2::publishConvexHullMarker(
  const std::vector<geometry_msgs::PointStamped>& corners,
  ros::Publisher& marker_pub,
  int id /*= 0*/)
{
  if (corners.empty()) {
    ROS_WARN("No corner points to publish!");
    return;
  }

  visualization_msgs::Marker marker;
  marker.header.frame_id = corners[0].header.frame_id;  // 从第一个点获取 frame
  marker.header.stamp = ros::Time::now();
  marker.ns = "convex_hull_vertices";
  marker.id = id;
  marker.type = visualization_msgs::Marker::POINTS;
  marker.action = visualization_msgs::Marker::ADD;

  marker.scale.x = 0.05;
  marker.scale.y = 0.05;

  marker.color.r = 1.0f;
  marker.color.g = 1.0f;
  marker.color.b = 1.0f;
  marker.color.a = 1.0f;

  marker.pose.orientation.w = 1.0;
  marker.lifetime = ros::Duration(0);
  marker.frame_locked = true;

  // 提取每个 PointStamped 的 .point 部分
  for (const auto& stamped_point : corners) {
    marker.points.push_back(stamped_point.point);
  }

  marker_pub.publish(marker);
}

std::vector<geometry_msgs::PointStamped> cw2::extract_corner_points(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& cloud,
  const std::vector<pcl::Vertices>& polygons,
  size_t top_k,
  const std::string& frame_id)  // 新增参数：frame
{
  std::vector<geometry_msgs::PointStamped> corners;
  if (polygons.empty()) return corners;

  const std::vector<uint32_t>& indices = polygons[0].vertices;
  size_t n = indices.size();
  if (n < 3) return corners;

  std::vector<std::pair<float, geometry_msgs::PointStamped>> candidates;

  for (size_t i = 0; i < n; ++i)
  {
    const pcl::PointXYZRGB& A = cloud->points[indices[(i + n - 1) % n]];
    const pcl::PointXYZRGB& B = cloud->points[indices[i]];
    const pcl::PointXYZRGB& C = cloud->points[indices[(i + 1) % n]];

    float ABx = B.x - A.x;
    float ABy = B.y - A.y;
    float BCx = C.x - B.x;
    float BCy = C.y - B.y;

    float cross = ABx * BCy - ABy * BCx;
    float abs_cross = std::abs(cross);

    geometry_msgs::PointStamped pt;
    pt.header.frame_id = frame_id;
    pt.header.stamp = ros::Time::now();  // 可选：设置当前时间
    pt.point.x = B.x;
    pt.point.y = B.y;
    pt.point.z = B.z;

    candidates.emplace_back(abs_cross, pt);

  }

  std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) {
              return a.first > b.first;
            });

  for (size_t i = 0; i < std::min(top_k, candidates.size()); ++i) {
    corners.push_back(candidates[i].second);
  }

  return corners;
}

std::pair<
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr,
  std::vector<pcl::Vertices>
>
cw2::computeConvexHull(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& input_cloud)
{
pcl::PointCloud<pcl::PointXYZRGB>::Ptr hull(new pcl::PointCloud<pcl::PointXYZRGB>);
pcl::ConvexHull<pcl::PointXYZRGB> chull;
std::vector<pcl::Vertices> polygons;

chull.setInputCloud(input_cloud);
chull.setDimension(2);  // 通常我们假设物体在同一个平面上
chull.reconstruct(*hull, polygons);

return {hull, polygons};
}

///////////////////////////////////////////////////////////////////////////////
// Task 2 回调函数
///////////////////////////////////////////////////////////////////////////////
bool cw2::t2_callback(cw2_world_spawner::Task2Service::Request &request,
  cw2_world_spawner::Task2Service::Response &response)
{
ROS_INFO("Task 2 callback triggered.");

std::vector<std::string> ref_shapes;

// ============================
//     检测 Reference Objects
// ============================
for (size_t i = 0; i < request.ref_object_points.size(); ++i)
{
const auto& pt = request.ref_object_points[i];
ROS_INFO_STREAM("Processing reference object #" << i);

if (!move_to_pose(pt, 0.5, true)) {
  ROS_ERROR_STREAM("Failed to move to reference point #" << i);
ref_shapes.push_back("unknown");
continue;
}

// 点云变换和滤波
pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed(new pcl::PointCloud<pcl::PointXYZRGB>);
pcl_ros::transformPointCloud("world", *latest_cloud_rgb, *transformed, tf_buffer_);
auto pass_filtered = filterTopLayer(transformed);

// 凸包和质心
auto [hull, polygons] = computeConvexHull(pass_filtered);
geometry_msgs::Point centroid = computeCentroid(hull);

// 可视化
publishCentroidCircleMarker(centroid, 0.005, centroid_pub_, 10 + i);

// 分类
std::string shape = classifyShapeByCentroidRegion(pass_filtered, centroid, 0.005, 20);
ref_shapes.push_back(shape);
}

// ============================
//        检测 Mystery Object
// ============================

ROS_INFO("Processing mystery object...");

std::string mystery_shape = "unknown";

if (move_to_pose(request.mystery_object_point, 0.5, true)) {
pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed(new pcl::PointCloud<pcl::PointXYZRGB>);
pcl_ros::transformPointCloud("world", *latest_cloud_rgb, *transformed, tf_buffer_);
auto pass_filtered = filterTopLayer(transformed);

auto [hull, polygons] = computeConvexHull(pass_filtered);
geometry_msgs::Point centroid = computeCentroid(hull);
publishCentroidCircleMarker(centroid, 0.005, centroid_pub_, 99);

mystery_shape = classifyShapeByCentroidRegion(pass_filtered, centroid, 0.005, 20);
} else {
ROS_ERROR("Failed to move to mystery object point.");
return false;
}

// ============================
//         匹配和输出结果
// ============================

bool matched = false;
int64_t mystery_object_num = 0;

for (size_t i = 0; i < ref_shapes.size(); ++i) {
if (mystery_shape == ref_shapes[i]) {
mystery_object_num = static_cast<int64_t>(i + 1);
matched = true;
break;
}
}

response.mystery_object_num = mystery_object_num;

// ============================
//           打印总结
// ============================

ROS_INFO("/////////////////////////////////////////////////////////////////////");
for (size_t i = 0; i < ref_shapes.size(); ++i) {
ROS_INFO_STREAM("Reference object #" << i + 1 << ": " << ref_shapes[i]);
}
ROS_INFO_STREAM("Mystery object detected as: " << mystery_shape);
if (matched) {
ROS_INFO_STREAM("=> Mystery object matches reference object #" << mystery_object_num);
} else {
ROS_WARN("=> Mystery object does not match any reference object.");
}
ROS_INFO("/////////////////////////////////////////////////////////////////////");

return true;
}

std::string cw2::classifyShapeByCentroidRegion(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& cloud,
  const geometry_msgs::Point& centroid,
  double radius,
  int threshold)
{
int count = 0;
double r2 = radius * radius;

for (const auto& pt : cloud->points) {
  if (std::isnan(pt.x) || std::isnan(pt.y) || std::isnan(pt.z)) continue;

  double dx = pt.x - centroid.x;
  double dy = pt.y - centroid.y;
  double dz = pt.z - centroid.z;

  // 可以只考虑XY平面，也可以考虑3D球形区域
  double dist2 = dx * dx + dy * dy;  // 或 dx*dx + dy*dy + dz*dz;

  if (dist2 <= r2) {
    count++;
  }
}

if (count < threshold) {
  return "nought";
} else {
  return "cross";
}
}


void cw2::publishCentroidCircleMarker(
  const geometry_msgs::Point& centroid,
  double radius,
  ros::Publisher& marker_pub,
  int id
)
{
// 1. 构造一个Marker
visualization_msgs::Marker marker;
marker.header.frame_id = "world";  // 如果你的centroid是相对于world的，就写world
marker.header.stamp = ros::Time::now();
marker.ns = "centroid_circle";
marker.id = id;
marker.type = visualization_msgs::Marker::LINE_STRIP;
marker.action = visualization_msgs::Marker::ADD;

// 2. 设置圆环的可视化属性
// 这里我们通过LINE_STRIP的方式，将一系列连续点首尾相接绘制为圆
marker.scale.x = 0.002; // 线条粗细，单位: 米
marker.color.r = 1.0;
marker.color.g = 1.0;
marker.color.b = 0.0;
marker.color.a = 1.0;
marker.pose.orientation.x = 0.0;
marker.pose.orientation.y = 0.0;
marker.pose.orientation.z = 0.0;
marker.pose.orientation.w = 1.0;

// 3. 生成圆上各个点坐标（假设在XY平面上，Z固定）
const int SEGMENTS = 36;  // 细分段数越多圆越平滑
for (int i = 0; i <= SEGMENTS; i++) {
  double angle = 2.0 * M_PI * static_cast<double>(i) / SEGMENTS;
  geometry_msgs::Point p;
  p.x = centroid.x + radius * std::cos(angle);
  p.y = centroid.y + radius * std::sin(angle);
  p.z = centroid.z; 
  marker.points.push_back(p);
}

marker.lifetime = ros::Duration(0.0);  // 0表示永久显示

// 4. 发布
marker_pub.publish(marker);

}

pcl::PointCloud<pcl::PointXYZRGB>::Ptr cw2::filterTopLayer(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr &input_cloud)
{
  float z_max = -std::numeric_limits<float>::max();

  // 第一步：找最高点
  for (const auto& pt : input_cloud->points) {
      if (!std::isnan(pt.z)) {
          if (pt.z > z_max) {
              z_max = pt.z;
          }
      }
  }

  float z_min = z_max - 0.035f;

  // 第二步：保留 z 在 [z_min, z_max] 之间的点
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
  output_cloud->header = input_cloud->header;
  output_cloud->is_dense = false;
  output_cloud->height = 1;

  for (const auto& pt : input_cloud->points) {
      if (!std::isnan(pt.z) && pt.z >= z_min && pt.z <= z_max) {
          output_cloud->points.push_back(pt);
      }
  }

  output_cloud->width = static_cast<uint32_t>(output_cloud->points.size());

  return output_cloud;
}


pcl::PointCloud<pcl::PointXYZRGB>::Ptr cw2::filterOutGreenPoints(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr &input_cloud)
{
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
  output_cloud->header = input_cloud->header;
  output_cloud->is_dense = input_cloud->is_dense;
  output_cloud->width = 0;
  output_cloud->height = 1;

  // Step 1: 找出 y 最大的点及其颜色
  pcl::PointXYZRGB max_y_point;
  float max_y = -std::numeric_limits<float>::infinity();
  for (const auto& pt : input_cloud->points) {
    if (pt.y > max_y) {
      max_y = pt.y;
      max_y_point = pt;
    }
  }

  uint8_t max_r = max_y_point.r;
  uint8_t max_g = max_y_point.g;
  uint8_t max_b = max_y_point.b;

  // Step 2: 过滤掉所有颜色相同的点
  size_t kept = 0;
  for (const auto& pt : input_cloud->points) {
    if (pt.r == max_r && pt.g == max_g && pt.b == max_b) {
      continue; // 跳过颜色相同的点
    }

    output_cloud->points.push_back(pt);
    ++kept;
  }

  output_cloud->width = static_cast<uint32_t>(output_cloud->points.size());
  output_cloud->height = 1;

  return output_cloud;
}


bool cw2::rotate_end_effector_pitch_offset(double delta_pitch)
{
  geometry_msgs::PoseStamped current_pose_stamped = arm_group_.getCurrentPose();
  geometry_msgs::Pose current_pose = current_pose_stamped.pose;

  tf2::Quaternion current_q;
  tf2::fromMsg(current_pose.orientation, current_q);

  double roll, pitch, yaw;
  tf2::Matrix3x3(current_q).getRPY(roll, pitch, yaw);

  double target_pitch = pitch + delta_pitch;

  tf2::Quaternion new_q;
  new_q.setRPY(roll, target_pitch, yaw);
  new_q.normalize();

  geometry_msgs::Pose new_pose = current_pose;
  new_pose.orientation = tf2::toMsg(new_q);

  arm_group_.setPoseTarget(new_pose);
  arm_group_.setMaxVelocityScalingFactor(0.7);
  arm_group_.setMaxAccelerationScalingFactor(0.7);
  arm_group_.setPlanningTime(5.0);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  bool success = 
      (arm_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

  if (success) {
    arm_group_.move();
  } else {
    ROS_ERROR("rotate_end_effector_pitch_offset planning failed!");
  }

  return success;
}

bool cw2::rotate_end_effector_roll_offset(double delta_roll)
{
  geometry_msgs::PoseStamped current_pose_stamped = arm_group_.getCurrentPose();
  geometry_msgs::Pose current_pose = current_pose_stamped.pose;

  tf2::Quaternion current_q;
  tf2::fromMsg(current_pose.orientation, current_q);

  double roll, pitch, yaw;
  tf2::Matrix3x3(current_q).getRPY(roll, pitch, yaw);

  double target_roll = roll + delta_roll;

  tf2::Quaternion new_q;
  new_q.setRPY(target_roll, pitch, yaw);
  new_q.normalize();

  geometry_msgs::Pose new_pose = current_pose;
  new_pose.orientation = tf2::toMsg(new_q);

  arm_group_.setPoseTarget(new_pose);
  arm_group_.setMaxVelocityScalingFactor(0.7);
  arm_group_.setMaxAccelerationScalingFactor(0.7);
  arm_group_.setPlanningTime(5.0);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  bool success = 
      (arm_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

  if (success) {
    arm_group_.move();
  } else {
    ROS_ERROR("rotate_end_effector_roll_offset planning failed!");
  }

  return success;
}

bool cw2::run_fpfh_alignment(const std::string& object_path, const std::string& scene_path)
{
  typedef pcl::PointNormal PointNT;
  typedef pcl::PointCloud<PointNT> PointCloudT;
  typedef pcl::FPFHSignature33 FeatureT;
  typedef pcl::PointCloud<FeatureT> FeatureCloudT;
  typedef pcl::FPFHEstimationOMP<PointNT, PointNT, FeatureT> FeatureEstimationT;
  typedef pcl::visualization::PointCloudColorHandlerCustom<PointNT> ColorHandlerT;

  PointCloudT::Ptr object(new PointCloudT);
  PointCloudT::Ptr object_aligned(new PointCloudT);
  PointCloudT::Ptr scene_before_downsampling(new PointCloudT);
  PointCloudT::Ptr scene(new PointCloudT);
  FeatureCloudT::Ptr object_features(new FeatureCloudT);
  FeatureCloudT::Ptr scene_features(new FeatureCloudT);

  // 加载点云
  if (pcl::io::loadPCDFile<PointNT>(object_path, *object) < 0 ||
      pcl::io::loadPCDFile<PointNT>(scene_path, *scene_before_downsampling) < 0)
  {
    ROS_ERROR("Error loading PCD files.");
    return false;
  }

  // 降采样
  pcl::VoxelGrid<PointNT> grid;
  float leaf = 0.003f;
  grid.setLeafSize(leaf, leaf, leaf);
  grid.setInputCloud(object);
  grid.filter(*object);
  grid.setInputCloud(scene_before_downsampling);
  grid.filter(*scene);

  // 场景法线估计
  pcl::NormalEstimationOMP<PointNT, PointNT> nest;
  nest.setRadiusSearch(0.01);
  nest.setInputCloud(scene);
  nest.setSearchSurface(scene_before_downsampling);
  nest.compute(*scene);

  // FPFH特征估计
  FeatureEstimationT fest;
  fest.setRadiusSearch(0.025);
  fest.setInputCloud(object);
  fest.setInputNormals(object);
  fest.compute(*object_features);

  fest.setInputCloud(scene);
  fest.setInputNormals(scene);
  fest.compute(*scene_features);

  // ✅ 预览降采样 + 法线
  pcl::visualization::PCLVisualizer visu_pre("Pre-alignment Preview");
  visu_pre.setBackgroundColor(0.05, 0.05, 0.05);
  visu_pre.setSize(1600, 900);
  visu_pre.addCoordinateSystem(0.1);
  visu_pre.initCameraParameters();

  visu_pre.addPointCloud(scene, ColorHandlerT(scene, 0.0, 255.0, 0.0), "scene");
  visu_pre.addPointCloudNormals<PointNT>(scene, 10, 0.01, "scene_normals");

  visu_pre.addPointCloud(object, ColorHandlerT(object, 255.0, 0.0, 0.0), "object");
  visu_pre.addPointCloudNormals<PointNT>(object, 10, 0.01, "object_normals");

  for (int i = 0; i < 50 && !visu_pre.wasStopped(); ++i) {
    visu_pre.spinOnce(100);  // 每次刷新100ms，总共5秒左右
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  visu_pre.close();  // 自动关闭窗口（可选）

  // ✅ 开始配准
  pcl::SampleConsensusPrerejective<PointNT, PointNT, FeatureT> align;
  align.setInputSource(object);
  align.setSourceFeatures(object_features);
  align.setInputTarget(scene);
  align.setTargetFeatures(scene_features);
  align.setMaximumIterations(80000);
  align.setNumberOfSamples(3);
  align.setCorrespondenceRandomness(10);
  align.setSimilarityThreshold(0.95f);
  align.setMaxCorrespondenceDistance(2.5f * leaf);
  align.setInlierFraction(0.25f);

  {
    pcl::ScopeTime t("Alignment");
    align.align(*object_aligned);
  }

  if (align.hasConverged())
  {
    Eigen::Matrix4f transformation = align.getFinalTransformation();

    // ✅ 可视化对齐后结果
    pcl::visualization::PCLVisualizer visu("FPFH Alignment Result");
    visu.setBackgroundColor(0.05, 0.05, 0.05);
    visu.setSize(1600, 900);
    visu.addCoordinateSystem(0.1);
    visu.initCameraParameters();

    visu.addPointCloud(scene, ColorHandlerT(scene, 0.0, 255.0, 0.0), "scene");
    visu.addPointCloud(object_aligned, ColorHandlerT(object_aligned, 255.0, 0.0, 0.0), "aligned");

    visu.spin();
    return true;
  }
  else
  {
    ROS_ERROR("FPFH alignment failed to converge.");
    return false;
  }
}

///////////////////////////////////////////////////////////////////////////////
// Task 3 回调函数
///////////////////////////////////////////////////////////////////////////////
bool cw2::t3_callback(cw2_world_spawner::Task3Service::Request &,
  cw2_world_spawner::Task3Service::Response &)
{
ROS_INFO("Task-3 start.");

auto initial = make_point(0.5, 0.0, 0.5);
if (!move_to_pose(initial, 0.0, true)) return false;
save_initial_joint_and_pose();

scan_sub_area({make_point(0.5, 0.45, 0.5), make_point(0.5, -0.45, 0.5),
make_point(0.3, -0.45, 0.5), make_point(0.3, 0.45, 0.5)});
go_to_initial_state();

rotate_joint("base", M_PI / 2);
scan_sub_area({make_point(0.10, 0.45, 0.5), make_point(0.10, 0.40, 0.5),
make_point(-0.10, 0.40, 0.5), make_point(-0.10, 0.45, 0.5)});
go_to_initial_state();

rotate_joint("base", -M_PI / 2);
scan_sub_area({make_point(0.10, -0.45, 0.5), make_point(0.10, -0.40, 0.5),
make_point(-0.10, -0.40, 0.5), make_point(-0.10, -0.45, 0.5)});
go_to_initial_state();

scan_sub_area({make_point(-0.5, -0.45, 0.5), make_point(-0.5, 0.45, 0.5),
make_point(-0.40, 0.45, 0.5), make_point(-0.40, -0.45, 0.5)});

build_octomap_from_accumulated_clouds();

std::vector<DetectedObj> det;
extract_objects(*latest_octree_, true, det);

int cnt_nought = 0, cnt_cross = 0;
for (auto &d : det) {
if (d.category == "object") {
if (d.shape == "nought") ++cnt_nought;
else if (d.shape == "cross") ++cnt_cross;
}
}

if (cnt_nought == 0 && cnt_cross == 0) {
ROS_ERROR("No object detected.");
return false;
}

std::string target_shape = (cnt_nought > cnt_cross)
             ? "nought"
             : (cnt_cross > cnt_nought ? "cross"
                                       : (rand() % 2 ? "nought" : "cross"));

DetectedObj basket{}, target{};
bool basket_ok = false, target_ok = false;

for (auto &d : det) {
if (!basket_ok && d.category == "basket") {
basket = d;
basket_ok = true;
}
if (!target_ok && d.category == "object" && d.shape == target_shape) {
target = d;
target_ok = true;
}
}

if (!basket_ok || !target_ok) {
ROS_ERROR("Basket or target object missing.");
return false;
}
go_to_initial_state();

geometry_msgs::PointStamped obj_pt = make_point(target.centroid.x,
                              target.centroid.y,
                              target.centroid.z);
geometry_msgs::PointStamped bask_pt = make_point(basket.centroid.x,
                               basket.centroid.y,
                               basket.centroid.z);

if (!move_to_pose(obj_pt, 0.50, true)) return false;

ros::Rate r(10);
int w = 0;
cloud_received_ = false;
while (!cloud_received_ && w++ < 50) r.sleep();

if (!cloud_received_) {
ROS_ERROR("No cloud.");
return false;
}

// Transform to world frame
pcl::PointCloud<pcl::PointXYZRGB>::Ptr transf(new pcl::PointCloud<pcl::PointXYZRGB>);
pcl_ros::transformPointCloud("world", *latest_cloud_rgb, *transf, tf_buffer_);

// 保留顶层 & 再根据 OctoMap voxel keys 过滤
auto filtered = filterByOctomapVoxels(transf, target.voxel_keys, *latest_octree_);
// ✅ 可视化过滤后的点云
publishCloud<pcl::PointXYZRGB>(filtered, "world");

ROS_INFO("Filtered cloud published");

// hull & corner
auto [hull, poly] = computeConvexHull(filtered);
auto corners = extract_corner_points(hull, poly, 1, "world");

geometry_msgs::PointStamped grasp_pt = computeGraspPoint(target.centroid, corners, target_shape);
geometry_msgs::Vector3 offset = computeOffsetVector(grasp_pt, target.centroid);
geometry_msgs::PointStamped place_pt = computeAdjustedPlacementPoint(bask_pt, offset);
// publish the grasp point
publishConvexHullMarker(corners, marker_pub_, 0);
publishGraspPointMarker(grasp_pt, grasp_point_pub_, 1);
publishBasketMarker(bask_pt, 0);
// publish accumulated cloud
publishAccumulatedCloud();

double yaw = compute_orientation(hull, grasp_pt) + compute_yaw_offset(target_shape);

ROS_INFO("Place point: (%.3f, %.3f, %.3f)",
         place_pt.point.x, place_pt.point.y, place_pt.point.z);

if (!cartesian_grasp_and_place(grasp_pt, place_pt, yaw)) {
ROS_ERROR("Pick-place fail.");
return false;
}

ROS_INFO("Task-3 done: picked a %s and dropped into basket.", target_shape.c_str());
return true;
}


void cw2::publishBasketMarker(
  const geometry_msgs::PointStamped& basket_point,
  int id
) {
  visualization_msgs::Marker marker;
  marker.header.frame_id = "world";  // 如果你的basket_point是相对于world的，就写world
  marker.header.stamp = ros::Time::now();
  marker.ns = "basket_marker";
  marker.id = id;
  marker.type = visualization_msgs::Marker::SPHERE;
  marker.action = visualization_msgs::Marker::ADD;

  marker.pose.position = basket_point.point;
  marker.pose.orientation.w = 1.0;

  marker.scale.x = 0.05;
  marker.scale.y = 0.05;
  marker.scale.z = 0.05;

  marker.color.r = 0.0f;
  marker.color.g = 1.0f;
  marker.color.b = 1.0f;
  marker.color.a = 1.0;

  marker.lifetime = ros::Duration(0.0);

  basket_pub_.publish(marker);
}



pcl::PointCloud<pcl::PointXYZRGB>::Ptr cw2::filterByOctomapVoxels(
  const pcl::PointCloud<pcl::PointXYZRGB>::Ptr& input_cloud,
  const std::unordered_set<octomap::OcTreeKey, cw2::KeyHash>& keys,
  const octomap::OcTree& tree)
{
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr output(new pcl::PointCloud<pcl::PointXYZRGB>);
  output->header = input_cloud->header;
  output->is_dense = input_cloud->is_dense;
  output->height = 1;

  for (const auto& pt : input_cloud->points) {
    if (std::isnan(pt.x) || std::isnan(pt.y) || std::isnan(pt.z)) continue;

    octomap::OcTreeKey key;
    if (tree.coordToKeyChecked(pt.x, pt.y, pt.z, key)) {
      if (keys.count(key)) {
        output->points.push_back(pt);
      }
    }
  }

  output->width = static_cast<uint32_t>(output->points.size());
  return output;
}



void cw2::clusterAccumulatedPointCloud()
{
  if (accumulated_cloud_->empty()) {
    ROS_WARN("No data in accumulated_cloud_ for clustering.");
    return;
  }

  // 1. 创建搜索树
  pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZRGB>);
  tree->setInputCloud(accumulated_cloud_);

  // 2. 聚类配置
  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<pcl::PointXYZRGB> ec;
  ec.setClusterTolerance(0.02);  // 最大点间距
  ec.setMinClusterSize(100);     // 最小聚类点数
  ec.setMaxClusterSize(25000);
  ec.setSearchMethod(tree);
  ec.setInputCloud(accumulated_cloud_);
  ec.extract(cluster_indices);

  int cluster_id = 0;
  visualization_msgs::MarkerArray markers;

  for (const auto& indices : cluster_indices)
  {
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cluster(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::copyPointCloud(*accumulated_cloud_, indices, *cluster);

    // 3. 计算质心
    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*cluster, centroid);

    // 4. 计算高度（z_max - z_min）
    float z_min = std::numeric_limits<float>::max();
    float z_max = -std::numeric_limits<float>::max();
    for (const auto& pt : cluster->points)
    {
      if (!std::isnan(pt.z)) {
        z_min = std::min(z_min, pt.z);
        z_max = std::max(z_max, pt.z);
      }
    }
    float height = z_max - z_min;

    // 5. 输出信息
    ROS_INFO("Cluster %d: size=%lu, centroid=(%.3f, %.3f, %.3f), height=%.3f m",
             cluster_id, cluster->size(), centroid[0], centroid[1], centroid[2], height);

    // 6. 可视化：显示聚类中心点
    visualization_msgs::Marker marker;
    marker.header.frame_id = "world";
    marker.header.stamp = ros::Time::now();
    marker.ns = "/cluster_centroids";
    marker.id = cluster_id;
    marker.type = visualization_msgs::Marker::SPHERE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose.position.x = centroid[0];
    marker.pose.position.y = centroid[1];
    marker.pose.position.z = centroid[2];
    marker.pose.orientation.w = 1.0;
    marker.scale.x = 0.03;
    marker.scale.y = 0.03;
    marker.scale.z = 0.03;
    marker.color.r = 1.0;
    marker.color.g = 0.0;
    marker.color.b = 0.0;
    marker.color.a = 1.0;
    marker.lifetime = ros::Duration(0.0);
    markers.markers.push_back(marker);

    ++cluster_id;
  }

  cluster_marker_pub_.publish(markers);

  if (cluster_id == 0)
    ROS_WARN("No clusters found in accumulated cloud.");
}


void cw2::publishAccumulatedCloud()
{
  if (!accumulated_cloud_ || accumulated_cloud_->empty())
  {
    ROS_WARN("No accumulated cloud to publish.");
    return;
  }

  sensor_msgs::PointCloud2 cloud_msg;
  pcl::toROSMsg(*accumulated_cloud_, cloud_msg);
  cloud_msg.header.frame_id = "world";  // 或你使用的坐标系
  cloud_msg.header.stamp = ros::Time::now();

  accumulated_cloud_pub_.publish(cloud_msg);
  ROS_INFO("Published accumulated cloud with %zu points.", accumulated_cloud_->size());
}


///////////////////////////////////////////////////////////////////////////////
// 直接在 OctoMap 上做连通域提取并打印每个物体的高度
//  - resolution : tree.getResolution()，自动用于 Z 坐标换算
//  - neighbor26 : true = 26邻接, false = 6邻接
///////////////////////////////////////////////////////////////////////////////
// ======================================================
// 直接在 OctoMap 上做连通域提取并打印每个物体的高度
// ======================================================
bool cw2::extract_objects(const octomap::OcTree& tree,
  bool                   neighbor26,
  std::vector<DetectedObj>& out)
{
using Key = octomap::OcTreeKey;

const double res = tree.getResolution();
const int    min_voxel_threshold = 200;
const int deltas[26][3] = {            // 26‑邻接表
{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},
{-1,-1,0},{-1,1,0},{1,-1,0},{1,1,0},
{-1,0,-1},{-1,0,1},{1,0,-1},{1,0,1},
{0,-1,-1},{0,-1,1},{0,1,-1},{0,1,1},
{-1,-1,-1},{-1,-1,1},{-1,1,-1},{-1,1,1},
{1,-1,-1},{1,-1,1},{1,1,-1},{1,1,1} };

/* ---------- 1. 收集所有占据体素 ---------- */
std::unordered_set<Key, cw2::KeyHash> occ;
for (auto it = tree.begin_leafs(); it != tree.end_leafs(); ++it)
if (tree.isNodeOccupied(*it)) occ.insert(it.getKey());

if (occ.empty()) { ROS_WARN("OctoMap is empty."); return false; }

/* ---------- 2. flood‑fill ---------- */
std::unordered_set<Key, cw2::KeyHash> vis;
std::vector<Key> stack; stack.reserve(2048);

int obj_idx = 1;
for (const Key& seed : occ)
{
if (vis.count(seed)) continue;

/* -- 聚类基本属性 -- */
double min_x=1e9,max_x=-1e9,min_y=1e9,max_y=-1e9,min_z=1e9,max_z=-1e9;
size_t voxel_cnt = 0;
std::unordered_set<Key, cw2::KeyHash> cluster;
stack.clear();   stack.push_back(seed);   vis.insert(seed);

while (!stack.empty()) {
Key cur = stack.back(); stack.pop_back();
voxel_cnt++; cluster.insert(cur);
octomap::point3d p = tree.keyToCoord(cur);
min_x = std::min(min_x, (double)p.x()); max_x = std::max(max_x, (double)p.x());
min_y = std::min(min_y, (double)p.y()); max_y = std::max(max_y, (double)p.y());
min_z = std::min(min_z, (double)p.z()); max_z = std::max(max_z, (double)p.z());

int nb = neighbor26 ? 26 : 6;
for (int i = 0; i < nb; ++i) {
Key nbk(cur[0]+deltas[i][0], cur[1]+deltas[i][1], cur[2]+deltas[i][2]);
if (occ.count(nbk) && !vis.count(nbk)) { vis.insert(nbk); stack.push_back(nbk); }
}
}
if (voxel_cnt < min_voxel_threshold) continue;

/* ---------- 3. 分类 ---------- */
double height = max_z - min_z + res;
std::string category;
if      (height > 0.05) category = "obstacle";
else if (height >= 0.03) category = "basket";
else                     category = "object";

std::string shape = "N/A";
geometry_msgs::Point centroid_pt{};   // 默认 (0,0,0)

/* ---------- 4. 质心计算 ---------- */
if (category == "object")      // 原有逻辑：只用顶部表面平均
{
octomap::OcTreeKey topKey = tree.coordToKey(0,0,max_z);
std::vector<octomap::point3d> surf;
for (const Key& k : cluster)
if (k[2] == topKey[2]) surf.emplace_back(tree.keyToCoord(k));

if (!surf.empty()) {
for (const auto& p : surf) {
centroid_pt.x += p.x(); centroid_pt.y += p.y(); centroid_pt.z += p.z();
}
centroid_pt.x /= surf.size(); centroid_pt.y /= surf.size(); centroid_pt.z /= surf.size();
}

/* --- 十字 / 圆环判别 --- */
Key center_key = tree.coordToKey(centroid_pt.x, centroid_pt.y, max_z);
bool center_occ = false;
for (int dx=-1; dx<=1 && !center_occ; ++dx)
for (int dy=-1; dy<=1 && !center_occ; ++dy) {
Key ck(center_key[0]+dx, center_key[1]+dy, center_key[2]);
if (cluster.count(ck)) { center_occ = true; break; }
}
shape = center_occ ? "cross" : "nought";
}
else if (category == "basket")   // 👈 新增：篮子质心 = 包围盒中心
{
centroid_pt.x = 0.5 * (min_x + max_x);
centroid_pt.y = 0.5 * (min_y + max_y);
centroid_pt.z = 0.5 * (min_z + max_z);
}
/* obstacle 无需质心 */

/* ---------- 5. 保存结果 ---------- */
DetectedObj d;
d.centroid    = centroid_pt;
d.category    = category;
d.shape       = shape;
d.voxel_keys  = cluster;
out.push_back(d);

ROS_INFO("Obj%02d vox=%zu h=%.3f  cat=%s  shape=%s",
obj_idx++, voxel_cnt, height, category.c_str(), shape.c_str());
}
return true;
}




void cw2::build_octomap_from_accumulated_clouds()
{
  ROS_INFO("Building OctoMap from RGB accumulated cloud...");

  double resolution = 0.005;

  if (!latest_octree_)
      latest_octree_ = std::make_shared<octomap::OcTree>(resolution);
  else
      latest_octree_->clear();

  for (const auto& pt : accumulated_cloud_->points)
  {
    if (!std::isnan(pt.x) && !std::isnan(pt.y) && !std::isnan(pt.z))
    {
      latest_octree_->updateNode(octomap::point3d(pt.x, pt.y, pt.z), true);
    }
  }

  latest_octree_->updateInnerOccupancy();

  octomap_msgs::Octomap map_msg;
  map_msg.header.frame_id = "world";
  map_msg.header.stamp = ros::Time::now();
  if (octomap_msgs::fullMapToMsg(*latest_octree_, map_msg))
  {
    octomap_pub_.publish(map_msg);
    ROS_INFO("Published octomap with %zu nodes.", latest_octree_->size());
  }
  else
  {
    ROS_ERROR("Failed to convert octomap to ROS message.");
  }
}



geometry_msgs::PointStamped cw2::make_point(double x, double y, double z)
{
  geometry_msgs::PointStamped pt;
  pt.header.frame_id = "world";
  pt.header.stamp = ros::Time::now();
  pt.point.x = x;
  pt.point.y = y;
  pt.point.z = z;
  return pt;
}


bool cw2::rotate_joint(const std::string& joint_name, double delta_angle_rad)
{
  // 1. 建立关节名字到索引的映射
  std::map<std::string, int> joint_name_to_index = {
    {"base", 0},
    {"shoulder", 1},
    {"upper_arm", 2},
    {"elbow", 3},
    {"forearm", 4},
    {"wrist", 5},
    {"eef", 6}
  };

  // 2. 检查 joint_name 是否有效
  auto it = joint_name_to_index.find(joint_name);
  if (it == joint_name_to_index.end()) {
    ROS_ERROR("Invalid joint name: %s", joint_name.c_str());
    return false;
  }

  int joint_index = it->second;

  // 3. 获取当前关节角度
  std::vector<double> joint_values = arm_group_.getCurrentJointValues();
  if (joint_values.empty()) {
    ROS_ERROR("Failed to get joint values.");
    return false;
  }

  if (joint_index < 0 || joint_index >= joint_values.size()) {
    ROS_ERROR("Joint index %d is out of range.", joint_index);
    return false;
  }

  // 4. 旋转指定的关节
  joint_values[joint_index] += delta_angle_rad;

  // 5. 设置目标并执行
  arm_group_.setJointValueTarget(joint_values);
  arm_group_.setPlanningTime(5.0);
  arm_group_.setMaxVelocityScalingFactor(0.7);
  arm_group_.setMaxAccelerationScalingFactor(0.7);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  if (arm_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS) {
    arm_group_.execute(plan);
    ROS_INFO("Rotated joint [%s] by %.2f degrees.", joint_name.c_str(), delta_angle_rad * 180.0 / M_PI);
    return true;
  } else {
    ROS_ERROR("Failed to plan joint rotation.");
    return false;
  }
}

bool cw2::save_initial_joint_and_pose()
{
  initial_joint_values_ = arm_group_.getCurrentJointValues();
  initial_ee_pose_ = arm_group_.getCurrentPose().pose;

  ROS_INFO("Initial joint angles and pose saved.");
  return true;
}


bool cw2::go_to_initial_state()
{
  if (initial_joint_values_.empty()) {
    ROS_WARN("Initial joint values are not recorded yet!");
    return false;
  }

  arm_group_.setJointValueTarget(initial_joint_values_);
  arm_group_.setPlanningTime(5.0);
  arm_group_.setMaxVelocityScalingFactor(0.7);
  arm_group_.setMaxAccelerationScalingFactor(0.7);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  if (arm_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS) {
    arm_group_.execute(plan);
    ROS_INFO("Returned to initial joint state.");
    return true;
  } else {
    ROS_ERROR("Failed to plan return to initial joint state.");
    return false;
  }
}


bool cw2::scan_sub_area(const std::vector<geometry_msgs::PointStamped>& corners)
{
  if (corners.size() != 4) {
    ROS_ERROR("scan_sub_area: exactly 4 corner points are required.");
    return false;
  }

  ROS_INFO("Starting sub-area scanning with Cartesian path...");

  // 1. 使用已有函数移动到第一个角点
  if (!move_to_pose(corners[0], 0.0, true)) {
    ROS_ERROR("Failed to move to first corner.");
    return false;
  }

  ros::Duration(1.0).sleep();  // 可选等待

  // 2. 构造从第1点开始的 path：2 -> 3 -> 4
  std::vector<geometry_msgs::Pose> waypoints;

  geometry_msgs::Pose base_pose = arm_group_.getCurrentPose().pose;

  for (int i = 1; i < 4; ++i) {
    geometry_msgs::Pose pose;
    pose.position.x = corners[i].point.x;
    pose.position.y = corners[i].point.y;
    pose.position.z = corners[i].point.z;

    pose.orientation = base_pose.orientation;  // 保持当前朝向

    waypoints.push_back(pose);
  }

  // 3. 笛卡尔路径规划
  moveit_msgs::RobotTrajectory trajectory;
  const double eef_step = 0.01;
  const double jump_threshold = 0.0;

  double fraction = arm_group_.computeCartesianPath(waypoints, eef_step, trajectory, true);

  if (fraction < 0.95) {
    ROS_WARN("Cartesian path planning only %.2f%% success.", fraction * 100.0);
    return false;
  }

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  plan.trajectory_ = trajectory;
  is_scanning_ = true;
  arm_group_.execute(plan);  // 机械臂执行路径，期间回调中不断采点
  is_scanning_ = false;
  return true;
}


///////////////////////////////////////////////////////////////////////////////
// 点云回调函数
///////////////////////////////////////////////////////////////////////////////
void cw2::pointCloudCallback(const sensor_msgs::PointCloud2ConstPtr& msg)
{
  // 同时保存XYZRGB与XYZ以供后续使用
  pcl::fromROSMsg(*msg, *latest_cloud_rgb);
  pcl::fromROSMsg(*msg, *latest_cloud_xyz);
  cloud_received_ = true;

  if (is_scanning_) {
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed(new pcl::PointCloud<pcl::PointXYZRGB>);
    try {
      pcl_ros::transformPointCloud("world", *latest_cloud_rgb, *transformed, tf_buffer_);
      auto pt_filtered = filterPassThrough<pcl::PointXYZRGB>(
        transformed,
        "z",
        0.04f,
        0.4f
      );
      
      // 再加一层体素滤波
      pcl::VoxelGrid<pcl::PointXYZRGB> voxel_filter;
      voxel_filter.setInputCloud(pt_filtered);
      voxel_filter.setLeafSize(0.003f, 0.003f, 0.003f);  // 体素大小：根据你需要精度调整
      
      pcl::PointCloud<pcl::PointXYZRGB>::Ptr downsampled(new pcl::PointCloud<pcl::PointXYZRGB>);
      voxel_filter.filter(*downsampled);
      
      // 再叠加
      *accumulated_cloud_ += *downsampled;
    } catch (tf2::TransformException &ex) {
      ROS_WARN("TF transform failed: %s", ex.what());
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// move_arm: 机械臂移动到目标点正上方
///////////////////////////////////////////////////////////////////////////////
bool cw2::move_to_pose(const geometry_msgs::PointStamped& target, double z_offset, bool reset_orientation)
{

  geometry_msgs::Pose target_pose;
  target_pose.position.x = target.point.x;
  target_pose.position.y = target.point.y;
  target_pose.position.z = target.point.z + z_offset;

  if (reset_orientation) {
    // 🔒 使用硬编码“向下”朝向
    target_pose.orientation.x = 0.9238795;
    target_pose.orientation.y = -0.3826834;
    target_pose.orientation.z = 0.0;
    target_pose.orientation.w = 0.0;
  } else {
    // 🧭 使用当前姿态
    geometry_msgs::PoseStamped current_pose = arm_group_.getCurrentPose();
    target_pose.orientation = current_pose.pose.orientation;
  }

  // 🔍 打印三个轴方向（可选调试）
  tf2::Quaternion tf_q;
  tf2::fromMsg(target_pose.orientation, tf_q);
  tf2::Matrix3x3 tf_R(tf_q);
  tf2::Vector3 x_axis = tf_R.getColumn(0);
  tf2::Vector3 y_axis = tf_R.getColumn(1);
  tf2::Vector3 z_axis = tf_R.getColumn(2);

  arm_group_.setPoseTarget(target_pose);
  arm_group_.setMaxVelocityScalingFactor(0.7);
  arm_group_.setMaxAccelerationScalingFactor(0.7);
  arm_group_.setPlanningTime(5.0);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  bool success =
      (arm_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

  if (success) {
    arm_group_.move();
  } else {
    ROS_ERROR("In move_to_pose(): Arm planning failed!");
  }

  return success;
}



///////////////////////////////////////////////////////////////////////////////
// move_gripper: 控制夹爪张开/闭合
///////////////////////////////////////////////////////////////////////////////
bool cw2::move_gripper(float width)
{
  const float gripper_max = 0.10;
  const float gripper_min = 0.028;

  // clamp一下
  width = std::max(gripper_min, std::min(gripper_max, width));
  std::vector<double> joint_targets(2, width / 2.0);

  hand_group_.setJointValueTarget(joint_targets);
  hand_group_.setMaxVelocityScalingFactor(1.0);
  hand_group_.setMaxAccelerationScalingFactor(1.0);
  hand_group_.setPlanningTime(3.0);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  bool success = 
      (hand_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

  if (success) {
    hand_group_.move();
  } else {
    ROS_ERROR("Gripper planning failed!");
  }
  return success;
}

///////////////////////////////////////////////////////////////////////////////
// rotate_end_effector: 绕Z轴旋转到给定yaw
///////////////////////////////////////////////////////////////////////////////
bool cw2::rotate_end_effector(double target_yaw)
{
  geometry_msgs::PoseStamped current_pose_stamped = arm_group_.getCurrentPose();
  geometry_msgs::Pose current_pose = current_pose_stamped.pose;

  tf2::Quaternion current_q;
  tf2::fromMsg(current_pose.orientation, current_q);

  double roll, pitch, yaw_now;
  tf2::Matrix3x3(current_q).getRPY(roll, pitch, yaw_now);

  // 替换 yaw，保留 roll/pitch
  tf2::Quaternion new_q;
  new_q.setRPY(roll, pitch, target_yaw);
  new_q.normalize();

  geometry_msgs::Pose new_pose = current_pose;
  new_pose.orientation = tf2::toMsg(new_q);

  arm_group_.setPoseTarget(new_pose);
  arm_group_.setMaxVelocityScalingFactor(0.7);
  arm_group_.setMaxAccelerationScalingFactor(0.7);
  arm_group_.setPlanningTime(5.0);

  moveit::planning_interface::MoveGroupInterface::Plan plan;
  bool success =
      (arm_group_.plan(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);

  if (success) {
    arm_group_.move();
  } else {
    ROS_ERROR("rotate_end_effector_to planning failed!");
  }

  return success;
}


void cw2::publishOrientationArrow(
  const geometry_msgs::PointStamped& origin,
  double yaw,
  ros::Publisher& marker_pub,
  int id)
{
  visualization_msgs::Marker arrow;
  arrow.header.frame_id = origin.header.frame_id;
  arrow.header.stamp = ros::Time::now();
  arrow.ns = "grasp_orientation";
  arrow.id = id;
  arrow.type = visualization_msgs::Marker::ARROW;
  arrow.action = visualization_msgs::Marker::ADD;

  // 起点
  geometry_msgs::Point start = origin.point;

  // 终点（向前延伸一段距离）
  double len = 0.1;  // 箭头长度
  geometry_msgs::Point end;
  end.x = start.x + len * std::cos(yaw);
  end.y = start.y + len * std::sin(yaw);
  end.z = start.z;  // 同一平面

  arrow.points.push_back(start);
  arrow.points.push_back(end);

  arrow.scale.x = 0.01;  // 箭身直径
  arrow.scale.y = 0.02;  // 箭头宽度
  arrow.scale.z = 0.0;   // 不用管

  arrow.color.r = 1.0;
  arrow.color.g = 0.5;
  arrow.color.b = 0.0;
  arrow.color.a = 1.0;

  arrow.pose.orientation.w = 1.0;
  arrow.lifetime = ros::Duration(0);  // 永久显示

  marker_pub.publish(arrow);
}
