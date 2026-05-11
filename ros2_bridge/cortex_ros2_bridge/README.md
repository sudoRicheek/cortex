# cortex_ros2_bridge

ROS 2 package that bridges Cortex pub/sub topics to ROS 2 topics, in both directions.

This is **PR1 of [the implementation plan](../PLAN.md)** — package skeleton and YAML config loader only. No bridging is wired up yet; subsequent PRs add the Cortex wire decoder, adapter registry, and the two composable nodes.

## Build

```bash
# from a colcon workspace whose src/ contains this package
colcon build --packages-select cortex_ros2_bridge
colcon test  --packages-select cortex_ros2_bridge
```

## Configuration

A YAML file describes every bridged topic. See [config/example_minimal.yaml](config/example_minimal.yaml) and [config/example_full.yaml](config/example_full.yaml). The schema is documented in [../PLAN.md](../PLAN.md) §5.

The C++ loader lives in [include/cortex_ros2_bridge/config.hpp](include/cortex_ros2_bridge/config.hpp); use `cortex_ros2_bridge::load_config(path)` from any node.
