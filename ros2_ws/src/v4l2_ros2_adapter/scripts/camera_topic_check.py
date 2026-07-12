#!/usr/bin/env python3

import argparse
import sys
import time
from typing import Dict, Set, Tuple

import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CameraInfo, Image

Stamp = Tuple[int, int]


class CameraTopicChecker(Node):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__("v4l2_camera_topic_checker")
        self.args = args
        self.image_stamps: Set[Stamp] = set()
        self.info_stamps: Set[Stamp] = set()
        self.failure = ""
        self.latest_diagnostics: Dict[str, str] = {}

        self.image_subscription = self.create_subscription(
            Image,
            args.image_topic,
            self.on_image,
            qos_profile_sensor_data,
        )
        self.info_subscription = self.create_subscription(
            CameraInfo,
            args.camera_info_topic,
            self.on_camera_info,
            qos_profile_sensor_data,
        )
        self.diagnostics_subscription = self.create_subscription(
            DiagnosticArray,
            args.diagnostic_topic,
            self.on_diagnostics,
            10,
        )

    @staticmethod
    def stamp_key(message) -> Stamp:
        return (
            int(message.header.stamp.sec),
            int(message.header.stamp.nanosec),
        )

    def fail(self, message: str) -> None:
        if not self.failure:
            self.failure = message

    def on_image(self, message: Image) -> None:
        if message.width != self.args.width or message.height != self.args.height:
            self.fail(f"Image size mismatch: {message.width}x{message.height}")
            return
        if message.encoding != self.args.encoding:
            self.fail(f"Image encoding mismatch: {message.encoding}")
            return

        expected_step = self.args.width * (
            3 if self.args.encoding == "rgb8"
            else 1 if self.args.encoding == "mono8"
            else 2
        )
        expected_size = expected_step * self.args.height
        if message.step != expected_step:
            self.fail(f"Image step mismatch: {message.step}, expected {expected_step}")
            return
        if len(message.data) != expected_size:
            self.fail(
                f"Image payload mismatch: {len(message.data)}, expected {expected_size}"
            )
            return
        if message.header.frame_id != self.args.frame_id:
            self.fail(f"Image frame_id mismatch: {message.header.frame_id}")
            return

        stamp = self.stamp_key(message)
        if stamp == (0, 0):
            self.fail("Image timestamp is zero")
            return
        self.image_stamps.add(stamp)

    def on_camera_info(self, message: CameraInfo) -> None:
        if message.width != self.args.width or message.height != self.args.height:
            self.fail(f"CameraInfo size mismatch: {message.width}x{message.height}")
            return
        if message.header.frame_id != self.args.frame_id:
            self.fail(f"CameraInfo frame_id mismatch: {message.header.frame_id}")
            return
        if len(message.k) != 9:
            self.fail(f"CameraInfo K size mismatch: {len(message.k)}")
            return

        stamp = self.stamp_key(message)
        if stamp == (0, 0):
            self.fail("CameraInfo timestamp is zero")
            return
        self.info_stamps.add(stamp)

    def on_diagnostics(self, message: DiagnosticArray) -> None:
        for status in message.status:
            if status.name != "v4l2_capture/camera_link":
                continue
            self.latest_diagnostics = {item.key: item.value for item in status.values}

    def matched_count(self) -> int:
        return len(self.image_stamps.intersection(self.info_stamps))

    def diagnostics_clean(self) -> bool:
        if not self.latest_diagnostics:
            return False
        for key in (
            "header_errors",
            "crc_errors",
            "rejected_frames",
            "image_publish_errors",
            "latency_clock_errors",
        ):
            if self.latest_diagnostics.get(key) != "0":
                return False
        try:
            published_images = int(
                self.latest_diagnostics.get("published_images", "0")
            )
            received_frames = int(
                self.latest_diagnostics.get("received_frames", "0")
            )
        except ValueError:
            return False
        return (
            published_images >= self.args.min_matched
            and received_frames >= self.args.min_matched
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image-topic", default="/camera/image_raw")
    parser.add_argument("--camera-info-topic", default="/camera/camera_info")
    parser.add_argument("--diagnostic-topic", default="/camera_link/diagnostics")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument(
        "--encoding",
        default="rgb8",
        choices=["rgb8", "mono8", "yuv422_yuy2"],
    )
    parser.add_argument("--frame-id", default="camera_optical_frame")
    parser.add_argument("--min-matched", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=float, default=20.0)
    args = parser.parse_args()
    if args.width <= 0 or args.height <= 0:
        parser.error("width and height must be positive")
    if args.min_matched <= 0:
        parser.error("min-matched must be positive")
    return args


def main() -> int:
    args = parse_args()
    rclpy.init()
    node = CameraTopicChecker(args)
    deadline = time.monotonic() + args.timeout_seconds

    try:
        while rclpy.ok() and time.monotonic() < deadline and not node.failure:
            rclpy.spin_once(node, timeout_sec=0.2)
            if node.matched_count() >= args.min_matched and node.diagnostics_clean():
                break

        if node.failure:
            print(f"[FAIL] {node.failure}")
            return 1
        if node.matched_count() < args.min_matched:
            print("[FAIL] insufficient matching Image and CameraInfo timestamps")
            print(f"image messages      : {len(node.image_stamps)}")
            print(f"camera_info messages: {len(node.info_stamps)}")
            print(f"matching timestamps : {node.matched_count()}")
            return 1
        if not node.diagnostics_clean():
            print("[FAIL] diagnostics were missing or reported errors")
            print(node.latest_diagnostics)
            return 1

        bytes_per_pixel = (
        3 if args.encoding == "rgb8"
        else 1 if args.encoding == "mono8"
        else 2
    )
        print("[PASS] ROS2 camera topics and diagnostics are valid")
        print(f"matching timestamps : {node.matched_count()}")
        print(f"image width         : {args.width}")
        print(f"image height        : {args.height}")
        print(f"image encoding      : {args.encoding}")
        print(f"image step          : {args.width * bytes_per_pixel}")
        print(
            f"image payload       : "
            f"{args.width * args.height * bytes_per_pixel} bytes"
        )
        print(f"camera frame_id     : {args.frame_id}")
        print(
            "published_images    : "
            f"{node.latest_diagnostics.get('published_images', 'unknown')}"
        )
        print(
            "RMW implementation  : "
            f"{node.latest_diagnostics.get('rmw_implementation', 'not-exported')}"
        )
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    sys.exit(main())
