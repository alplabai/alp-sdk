# SPDX-License-Identifier: Apache-2.0
#
# Build config for the ROS 2 Humble subset the Alp images ship (the alp-ros
# feature group: rclcpp + the message/transport stack + alp-perception).
# Required by conf/distro/alp.conf so every DISTRO=alp build gets it -- this is
# recipe-intent, NOT a CI scratch local.conf.
#
# Two things meta-ros's full recipe set needs that we deliberately do without:
#
#  1. cmake_qt5 (meta-qt5) -- pulled by the GUI/sim/visualization recipes
#     (gazebo, rviz, rqt, moveit, mapviz, ...). meta-qt5 is not vendored and
#     none of those recipes are in any alp image, so mask them; otherwise a
#     plain .bbappend that inherits the missing class is a hard parse error.
#  2. rmw-connextdds -> rti-connext-dds (proprietary, not in the OSS pkg).
#     rmw-implementation hard-lists every RMW backend; drop Connext so it
#     builds against the open backends (Fast-DDS stays the default RMW).
#
# Copyright (C) 2026 Alp Lab AB

# --- 1. mask the cmake_qt5-inheriting recipes (relative-path regexes) -------
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/cartographer-ros/cartographer-rviz_2.0.9002-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/clearpath-common/clearpath-generator-common_1.3.9-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/find-object-2d/find-object-2d_0.7.0-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/grid-map/grid-map-rviz-plugin_2.0.1-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/imu-tools/rviz-imu-plugin_2.1.5-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/mapviz/mapviz_2.6.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/mapviz/mapviz-plugins_2.6.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/mapviz/multires-image_2.6.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/mapviz/tile-map_2.6.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/moveit/moveit-ros-visualization_2.5.9-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/moveit/moveit-setup-framework_2.5.9-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/moveit/moveit-setup-srdf-plugins_2.5.9-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/moveit-visual-tools/moveit-visual-tools_4.1.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/navigation2/nav2-rviz-plugins_1.1.20-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/octomap-rviz-plugins/octomap-rviz-plugins_2.1.1-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/plotjuggler/plotjuggler_3.15.0-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/plotjuggler-ros/plotjuggler-ros_2.3.1-2.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/polygon-ros/polygon-rviz-plugins_1.2.0-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/qt-gui-core/qt-gui-cpp_2.2.4-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rmf-task/rmf-task_2.1.8-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rmf-traffic-editor/rmf-traffic-editor_1.6.3-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rmf-visualization/rmf-visualization-rviz2-plugins_2.0.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/ros-gz/ros-gz-sim_0.244.23-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/ros-gz/ros-ign-gazebo_0.244.23-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/ros-image-to-qimage/ros-image-to-qimage_0.2.1-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rqt-image-overlay/rqt-image-overlay_0.1.3-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rqt-image-view/rqt-image-view_1.2.0-2.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rqt/rqt-gui-cpp_1.1.9-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz-2d-overlay-plugins/rviz-2d-overlay-plugins_1.4.0-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz/rviz2_11.2.26-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz/rviz-common_11.2.26-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz/rviz-default-plugins_11.2.26-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz/rviz-rendering_11.2.26-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz-satellite/rviz-satellite_4.0.0-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/rviz-visual-tools/rviz-visual-tools_4.1.4-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/slam-toolbox/slam-toolbox_2.6.10-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/snowbot-operating-system/snowbot-operating-system_0.1.2-3.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/swri-console/swri-console_2.1.2-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/turtlesim/turtlesim_%.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/vision-msgs-layers/vision-msgs-layers_0.1.0-1.bbappend"
BBMASK += "meta-ros/meta-ros2-humble/recipes-bbappends/vision-msgs/vision-msgs-rviz-plugins_4.1.1-1.bbappend"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo-classic/gazebo11_11.15.1.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo-classic/sdformat9_9.10.1.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-gui8_8.3.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-gui8_8.4.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-gui9_9.0.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-gui9_9.0.1.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-gui9_9.0.2.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-launch7_7.1.1.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-launch8_8.0.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-launch8_8.0.1.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim8_8.10.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim8_8.11.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim8_8.7.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim8_8.9.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim9_9.0.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim9_9.1.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim9_9.4.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/gz-sim9_9.5.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/ignition-gazebo6_6.17.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/ignition-gui6_6.8.0.bb"
BBMASK += "meta-ros/meta-ros-common/recipes-devtools/gazebo/ignition-launch5_5.3.0.bb"

# --- 2. drop the proprietary Connext RMW backend ---------------------------
DEPENDS:remove:pn-rmw-implementation = "rmw-connextdds"
