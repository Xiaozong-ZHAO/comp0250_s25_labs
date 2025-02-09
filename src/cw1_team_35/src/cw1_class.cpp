/* feel free to change any part of this file, or delete this file. In general,
you can do whatever you want with this template code, including deleting it all
and starting from scratch. The only requirment is to make sure your entire 
solution is contained within the cw1_team_<your_team_number> package */

#include <cw1_class.h>

///////////////////////////////////////////////////////////////////////////////

cw1::cw1(ros::NodeHandle nh)
{
  /* class constructor */

  nh_ = nh;

  // advertise solutions for coursework tasks
  t1_service_  = nh_.advertiseService("/task1_start", 
    &cw1::t1_callback, this);
  t2_service_  = nh_.advertiseService("/task2_start", 
    &cw1::t2_callback, this);
  t3_service_  = nh_.advertiseService("/task3_start",
    &cw1::t3_callback, this);

  ROS_INFO("cw1 class initialised");
}

///////////////////////////////////////////////////////////////////////////////

bool
cw1::t1_callback(cw1_world_spawner::Task1Service::Request &request,
  cw1_world_spawner::Task1Service::Response &response) 
{
  // print the request message with ROS_INFO
  ROS_INFO("The request message for task 1: Object Location -> [x: %.2f, y: %.2f, z: %.2f]",
         request.object_loc.pose.position.x,
         request.object_loc.pose.position.y,
         request.object_loc.pose.position.z);
  /* function which should solve task 1 */
  // 1. Get the block position
  // 2. Move arm to that position
  // 3. Change gripper distance to grab the item
  // 4. Get the basket position
  // 5. Move arm above the basket

  ROS_INFO("The coursework solving callback for task 1 has been triggered");

  return true;
}

///////////////////////////////////////////////////////////////////////////////

bool
cw1::t2_callback(cw1_world_spawner::Task2Service::Request &request,
  cw1_world_spawner::Task2Service::Response &response)
{
  /* function which should solve task 2 */

  ROS_INFO("The coursework solving callback for task 2 has been triggered");

  return true;
}

///////////////////////////////////////////////////////////////////////////////

bool
cw1::t3_callback(cw1_world_spawner::Task3Service::Request &request,
  cw1_world_spawner::Task3Service::Response &response)
{
  /* function which should solve task 3 */

  ROS_INFO("The coursework solving callback for task 3 has been triggered");

  return true;
}