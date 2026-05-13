// Copyright (c) 2026, Cortex contributors. Apache-2.0.
//
// Round-trip tests for the standard catalogue adapters added in PR6+PR7:
// Array (Float32/Float64 MultiArray), Image, PointCloud2, Pose, Transform,
// Tensor.
#include "cortex_ros2_bridge/adapters/arrays.hpp"
#include "cortex_ros2_bridge/adapters/image.hpp"
#include "cortex_ros2_bridge/adapters/pointcloud.hpp"
#include "cortex_ros2_bridge/adapters/pose.hpp"
#include "cortex_ros2_bridge/adapters/primitives.hpp"
#include "cortex_ros2_bridge/adapters/tensor.hpp"
#include "cortex_ros2_bridge/adapters/transform.hpp"
#include "cortex_ros2_bridge/binding_factory.hpp"
#include "cortex_ros2_bridge/registry.hpp"

#include <cortex_wire/metadata.hpp>
#include <cortex_wire/oob_buffer.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace cortex_ros2_bridge;
using namespace cortex_ros2_bridge::adapters;

namespace
{

// Build an inbound carrying a metadata frame plus a list of OOB byte buffers,
// each owned through a fresh zmq::message_t.
struct Inbound
{
  std::vector<std::uint8_t> metadata_bytes;
  std::vector<std::vector<std::uint8_t>> oob_owned;
  std::vector<cortex_wire::ZmqFramePtr> oob_frames;
  cortex_wire::DecodedMetadata metadata;
  cortex_wire::MessageHeader header{};
  BridgeEntry cfg;

  Inbound(std::vector<std::uint8_t> meta, std::vector<std::vector<std::uint8_t>> oob)
  : metadata_bytes(std::move(meta)),
    oob_owned(std::move(oob)),
    metadata(cortex_wire::DecodedMetadata::from_bytes(
        metadata_bytes.data(), metadata_bytes.size()))
  {
    oob_frames.reserve(oob_owned.size());
    for (auto & buf : oob_owned) {
      zmq::message_t m(buf.size());
      std::memcpy(m.data(), buf.data(), buf.size());
      oob_frames.push_back(cortex_wire::make_owned(std::move(m)));
    }
  }

  CortexInbound view() const {return CortexInbound{header, metadata, oob_frames, cfg};}
};

}  // namespace

// ---- ArrayFloat32 --------------------------------------------------------

TEST(ArrayFloat32Adapter, RoundTrip)
{
  std_msgs::msg::Float32MultiArray src;
  src.data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  std_msgs::msg::MultiArrayDimension d0; d0.size = 2; d0.stride = 6;
  std_msgs::msg::MultiArrayDimension d1; d1.size = 3; d1.stride = 3;
  src.layout.dim = {d0, d1};

  ArrayFloat32Adapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, 0, cfg);
  ASSERT_EQ(out.oob_buffers.size(), 1u);

  Inbound in(std::move(out.metadata), std::move(out.oob_buffers));
  auto round = adapter.to_ros2(in.view());
  EXPECT_EQ(round->data, src.data);
  ASSERT_EQ(round->layout.dim.size(), 2u);
  EXPECT_EQ(round->layout.dim[0].size, 2u);
  EXPECT_EQ(round->layout.dim[1].size, 3u);
}

TEST(ArrayFloat32Adapter, RejectsWrongDtype)
{
  // Build a metadata frame with <f8 instead of <f4 — adapter must reject.
  std::vector<double> data{1.0, 2.0};
  cortex_wire::MetadataBuilder b(3);
  b.pack_numpy_oob("<f8", {2}, data.data(), data.size() * sizeof(double));
  b.pack_str("");
  b.pack_str("");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  ArrayFloat32Adapter adapter;
  EXPECT_THROW(adapter.to_ros2(in.view()), cortex_wire::WireDecodeError);
}

TEST(ArrayFloat64Adapter, RoundTrip)
{
  std_msgs::msg::Float64MultiArray src;
  src.data = {0.5, 1.5, 2.5, 3.5};
  std_msgs::msg::MultiArrayDimension dim; dim.size = 4; dim.stride = 4;
  src.layout.dim = {dim};

  ArrayFloat64Adapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, 0, cfg);
  Inbound in(std::move(out.metadata), std::move(out.oob_buffers));
  auto round = adapter.to_ros2(in.view());
  EXPECT_EQ(round->data, src.data);
}

// ---- Image ----------------------------------------------------------------

TEST(ImageAdapter, RoundTripRgb8)
{
  // Synthesize a tiny 4×3×3 RGB image.
  const std::uint32_t W = 3, H = 4, C = 3;
  std::vector<std::uint8_t> pixels(W * H * C);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<std::uint8_t>(i & 0xff);
  }

  cortex_wire::MetadataBuilder b(4);
  b.pack_numpy_oob("<u1", {H, W, C}, pixels.data(), pixels.size());
  b.pack_str("rgb8");
  b.pack_uint(W);
  b.pack_uint(H);
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  ImageAdapter adapter;
  auto img = adapter.to_ros2(in.view());
  EXPECT_EQ(img->width, W);
  EXPECT_EQ(img->height, H);
  EXPECT_EQ(img->encoding, "rgb8");
  EXPECT_EQ(img->step, W * C);
  ASSERT_EQ(img->data.size(), pixels.size());
  EXPECT_EQ(std::memcmp(img->data.data(), pixels.data(), pixels.size()), 0);

  // Reverse: pack the same image back to Cortex.
  BridgeEntry cfg;
  auto back = adapter.to_cortex(*img, 0, cfg);
  ASSERT_EQ(back.oob_buffers.size(), 1u);
  EXPECT_EQ(back.oob_buffers[0].size(), pixels.size());
  EXPECT_EQ(
    std::memcmp(back.oob_buffers[0].data(), pixels.data(), pixels.size()), 0);
}

TEST(ImageAdapter, FrameIdYamlOverride)
{
  cortex_wire::MetadataBuilder b(4);
  std::vector<std::uint8_t> px(2 * 2 * 1, 0xff);
  b.pack_numpy_oob("<u1", {2, 2}, px.data(), px.size());
  b.pack_str("mono8");
  b.pack_uint(2);
  b.pack_uint(2);
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));
  in.cfg.ros2.frame_id = "cam_optical";

  ImageAdapter adapter;
  auto img = adapter.to_ros2(in.view());
  EXPECT_EQ(img->header.frame_id, "cam_optical");
}

// ---- PointCloud2 ---------------------------------------------------------

TEST(PointCloudAdapter, XyzOnlyRoundTrip)
{
  // Three points.
  const std::vector<float> pts = {
    0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 2.0f,
  };

  cortex_wire::MetadataBuilder b(5);
  b.pack_numpy_oob("<f4", {3, 3}, pts.data(), pts.size() * sizeof(float));
  b.pack_nil();   // colors
  b.pack_nil();   // intensity
  b.pack_nil();   // normals
  b.pack_str("lidar");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  PointCloudAdapter adapter;
  auto pc = adapter.to_ros2(in.view());
  EXPECT_EQ(pc->width, 3u);
  EXPECT_EQ(pc->height, 1u);
  EXPECT_EQ(pc->fields.size(), 3u);
  EXPECT_EQ(pc->point_step, 12u);
  ASSERT_EQ(pc->data.size(), 36u);
  // Spot-check x of point 1
  float x1;
  std::memcpy(&x1, pc->data.data() + 12 + 0, 4);
  EXPECT_FLOAT_EQ(x1, 1.0f);
  EXPECT_EQ(pc->header.frame_id, "lidar");

  // Reverse.
  BridgeEntry cfg;
  auto back = adapter.to_cortex(*pc, 0, cfg);
  // points + 3 nils for colors/intensity/normals → 1 OOB buffer total.
  EXPECT_EQ(back.oob_buffers.size(), 1u);
}

TEST(PointCloudAdapter, FullAttributesRoundTrip)
{
  const std::uint32_t N = 2;
  const std::vector<float> pts = {1, 2, 3, 4, 5, 6};
  const std::vector<std::uint8_t> col = {255, 0, 0, 0, 255, 0};
  const std::vector<float> inten = {10.0f, 20.0f};
  const std::vector<float> nrm = {0, 0, 1, 1, 0, 0};

  cortex_wire::MetadataBuilder b(5);
  b.pack_numpy_oob("<f4", {N, 3}, pts.data(), pts.size() * sizeof(float));
  b.pack_numpy_oob("<u1", {N, 3}, col.data(), col.size());
  b.pack_numpy_oob("<f4", {N}, inten.data(), inten.size() * sizeof(float));
  b.pack_numpy_oob("<f4", {N, 3}, nrm.data(), nrm.size() * sizeof(float));
  b.pack_str("");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  PointCloudAdapter adapter;
  auto pc = adapter.to_ros2(in.view());
  // 3 (xyz) + 1 (rgb packed) + 1 (intensity) + 3 (normal_x/y/z) = 8 fields.
  EXPECT_EQ(pc->fields.size(), 8u);
  EXPECT_EQ(pc->point_step, 12u + 4u + 4u + 12u);

  BridgeEntry cfg;
  auto back = adapter.to_cortex(*pc, 0, cfg);
  EXPECT_EQ(back.oob_buffers.size(), 4u);  // points, colors, intensity, normals
}

// ---- PoseStamped ---------------------------------------------------------

TEST(PoseAdapter, RoundTrip)
{
  std::array<double, 3> pos{1.0, 2.0, 3.0};
  std::array<double, 4> orient{0.0, 0.0, 0.0, 1.0};

  cortex_wire::MetadataBuilder b(4);
  b.pack_numpy_oob("<f8", {3}, pos.data(), pos.size() * sizeof(double));
  b.pack_numpy_oob("<f8", {4}, orient.data(), orient.size() * sizeof(double));
  b.pack_str("map");
  b.pack_str("base_link");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  PoseAdapter adapter;
  auto p = adapter.to_ros2(in.view());
  EXPECT_DOUBLE_EQ(p->pose.position.x, 1.0);
  EXPECT_DOUBLE_EQ(p->pose.position.y, 2.0);
  EXPECT_DOUBLE_EQ(p->pose.position.z, 3.0);
  EXPECT_DOUBLE_EQ(p->pose.orientation.w, 1.0);
  EXPECT_EQ(p->header.frame_id, "map");

  // Reverse — child_frame_id is dropped (PoseStamped has none).
  BridgeEntry cfg;
  auto back = adapter.to_cortex(*p, 0, cfg);
  EXPECT_EQ(back.oob_buffers.size(), 2u);
}

TEST(PoseAdapter, AcceptsFloat32Wire)
{
  std::array<float, 3> pos{0.5f, 1.5f, 2.5f};
  std::array<float, 4> orient{0.0f, 0.0f, 0.0f, 1.0f};
  cortex_wire::MetadataBuilder b(4);
  b.pack_numpy_oob("<f4", {3}, pos.data(), pos.size() * sizeof(float));
  b.pack_numpy_oob("<f4", {4}, orient.data(), orient.size() * sizeof(float));
  b.pack_str("");
  b.pack_str("");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  PoseAdapter adapter;
  auto p = adapter.to_ros2(in.view());
  EXPECT_FLOAT_EQ(p->pose.position.y, 1.5);
}

// ---- TransformStamped ----------------------------------------------------

TEST(TransformAdapter, IdentityRoundTrip)
{
  std::array<double, 16> identity{
    1, 0, 0, 5,
    0, 1, 0, 6,
    0, 0, 1, 7,
    0, 0, 0, 1,
  };

  cortex_wire::MetadataBuilder b(3);
  b.pack_numpy_oob("<f8", {4, 4}, identity.data(), identity.size() * sizeof(double));
  b.pack_str("world");
  b.pack_str("robot");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  TransformAdapter adapter;
  auto t = adapter.to_ros2(in.view());
  EXPECT_DOUBLE_EQ(t->transform.translation.x, 5);
  EXPECT_DOUBLE_EQ(t->transform.translation.y, 6);
  EXPECT_DOUBLE_EQ(t->transform.translation.z, 7);
  EXPECT_DOUBLE_EQ(t->transform.rotation.w, 1.0);
  EXPECT_DOUBLE_EQ(t->transform.rotation.x, 0.0);
  EXPECT_EQ(t->header.frame_id, "world");
  EXPECT_EQ(t->child_frame_id, "robot");

  // Reverse round-trips through quaternion → matrix.
  BridgeEntry cfg;
  auto back = adapter.to_cortex(*t, 0, cfg);
  Inbound rt(std::move(back.metadata), std::move(back.oob_buffers));
  auto t2 = adapter.to_ros2(rt.view());
  EXPECT_DOUBLE_EQ(t2->transform.translation.x, 5);
  EXPECT_DOUBLE_EQ(t2->transform.translation.y, 6);
  EXPECT_DOUBLE_EQ(t2->transform.translation.z, 7);
}

TEST(TransformAdapter, RotationQuaternionRoundTrip)
{
  // 90 degree rotation about Z, no translation.
  const double c = 0.0;
  const double s = 1.0;
  std::array<double, 16> R{
    c, -s, 0, 0,
    s,  c, 0, 0,
    0,  0, 1, 0,
    0,  0, 0, 1,
  };

  cortex_wire::MetadataBuilder b(3);
  b.pack_numpy_oob("<f8", {4, 4}, R.data(), R.size() * sizeof(double));
  b.pack_str("");
  b.pack_str("");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  TransformAdapter adapter;
  auto t = adapter.to_ros2(in.view());
  // q = (0,0, sin(45deg), cos(45deg)) for Z 90deg → ~(0,0,0.707,0.707)
  EXPECT_NEAR(t->transform.rotation.x, 0.0, 1e-9);
  EXPECT_NEAR(t->transform.rotation.y, 0.0, 1e-9);
  EXPECT_NEAR(t->transform.rotation.z, std::sqrt(0.5), 1e-9);
  EXPECT_NEAR(t->transform.rotation.w, std::sqrt(0.5), 1e-9);
}

// ---- Tensor --------------------------------------------------------------

TEST(TensorFloat32Adapter, RoundTrip)
{
  std::vector<float> data{1.0f, 2.0f, 3.0f, 4.0f};

  // Construct a torch OOB descriptor by hand.
  cortex_wire::MetadataBuilder b(2);
  b.pack_torch_oob(
    "<f4", {2, 2}, "cpu", false, data.data(), data.size() * sizeof(float));
  b.pack_str("weights");
  auto frames = std::move(b).finish();
  Inbound in(std::move(frames.metadata), std::move(frames.oob_buffers));

  TensorFloat32Adapter adapter;
  auto m = adapter.to_ros2(in.view());
  EXPECT_EQ(m->data, data);
  ASSERT_EQ(m->layout.dim.size(), 2u);
  EXPECT_EQ(m->layout.dim[0].size, 2u);
  EXPECT_EQ(m->layout.dim[1].size, 2u);

  BridgeEntry cfg;
  auto back = adapter.to_cortex(*m, 0, cfg);
  // Verify reverse emits a torch OOB descriptor by decoding the metadata.
  auto md = cortex_wire::DecodedMetadata::from_bytes(
    back.metadata.data(), back.metadata.size());
  ASSERT_EQ(md.field_count(), 2u);
  auto desc = cortex_wire::DecodedMetadata::as_oob(md.field(0));
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->kind, cortex_wire::OobKind::Torch);
  EXPECT_EQ(desc->device, "cpu");
  EXPECT_FALSE(desc->requires_grad);
}

// ---- Registration --------------------------------------------------------

TEST(RegisterStandard, AllAdaptersAndBindingsLandInRegistries)
{
  AdapterRegistry ar;
  BindingFactoryRegistry br;

  const auto a_pri = register_primitives(ar);
  const auto a_arr = register_array_adapters(ar);
  const auto a_img = register_image_adapters(ar);
  const auto a_pc  = register_pointcloud_adapters(ar);
  const auto a_pose = register_pose_adapters(ar);
  const auto a_tf  = register_transform_adapters(ar);
  const auto a_ten = register_tensor_adapters(ar);

  // Adapters: 12 primitive + 4 array (2 × 2 dirs) + 2 image + 2 pointcloud +
  // 2 pose + 2 transform + 2 tensor.
  EXPECT_EQ(a_pri + a_arr + a_img + a_pc + a_pose + a_tf + a_ten, 12u + 4u + 2u + 2u + 2u + 2u + 2u);

  const auto b_pri = register_primitive_bindings(br);
  const auto b_std = register_standard_bindings(br);
  EXPECT_EQ(b_pri + b_std, 12u + 12u);

  EXPECT_TRUE(br.get_cortex_to_ros2("sensor_msgs/msg/Image"));
  EXPECT_TRUE(br.get_ros2_to_cortex("sensor_msgs/msg/PointCloud2"));
  EXPECT_TRUE(br.get_cortex_to_ros2("geometry_msgs/msg/PoseStamped"));
}
