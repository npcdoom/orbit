# Copyright (c) 2021 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

load("//:orbit.bzl", "orbit_cc_library", "orbit_cc_test")

package(default_visibility = ["//:__subpackages__"])

licenses(["notice"])

orbit_cc_library(
    name = "FramePointerValidatorService",
    deps = [
        "//src/FramePointerValidator",
        "//src/GrpcProtos:code_block_cc_proto",
        "//src/GrpcProtos:services_cc_grpc_proto",
        "//src/GrpcProtos:services_cc_proto",
        "//src/ObjectUtils",
        "//src/OrbitBase",
        "@com_github_grpc_grpc//:grpc",
        "@com_github_grpc_grpc//:grpc++",
        "@com_google_absl//absl/strings:str_format",
    ],
)

orbit_cc_test(
    name = "FramePointerValidatorServiceTests",
    deps = [
        ":FramePointerValidatorService",
    ],
)
