# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Launch file for the alp_perception node + standard remappings."""

from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        Node(
            package='alp_perception',
            executable='perception_node',
            name='alp_perception',
            output='screen',
            parameters=[],
            remappings=[
                # Customer launch files override these to merge the
                # ALP topics into a wider robot graph.
                ('/alp/imu',        '/robot/imu'),
                ('/alp/rail_3v3',   '/robot/rail_3v3'),
                ('/alp/image',      '/robot/camera/image_raw'),
                ('/alp/detections', '/robot/perception/detections'),
                ('/alp/cmd_vel',    '/robot/cmd_vel'),
            ],
        ),
    ])
