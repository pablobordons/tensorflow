/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// XLA TensorList operators.
// Tensor lists are represented as tuple consisting of a pre-allocated list
// consisting of the tensors (and where dim 0 is the list index), along with a
// scalar telling us the current number of elements.

#include <limits>
#include <vector>

#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_helpers.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/core/framework/bounds_check.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/concat_lib.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/types.h"

namespace tensorflow {
namespace {

Status GetTensorListShape(xla::XlaBuilder* builder, xla::XlaOp op,
                          TensorShape* tensor_list_shape) {
  auto shape_or_status = builder->GetShape(op);
  if (!shape_or_status.ok()) {
    return shape_or_status.status();
  }
  xla::Shape shape = shape_or_status.ValueOrDie();
  TF_RET_CHECK(shape.IsTuple());
  return XLAShapeToTensorShape(xla::ShapeUtil::GetTupleElementShape(shape, 0),
                               tensor_list_shape);
}

class TensorListLengthOp : public XlaOpKernel {
 public:
  explicit TensorListLengthOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {}

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaOp tl = ctx->Input(0);
    xla::XlaOp index = xla::GetTupleElement(tl, 1);
    ctx->SetOutput(0, index);
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(TensorListLengthOp);
};

REGISTER_XLA_OP(Name("TensorListLength"), TensorListLengthOp);

class TensorListReserveOp : public XlaOpKernel {
 public:
  explicit TensorListReserveOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    TensorShape element_shape;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsShape(0, &element_shape));
    int64 num_elements;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsIntScalar(1, &num_elements));

    TensorShape tensor_shape;
    tensor_shape.AddDim(num_elements);
    tensor_shape.AppendShape(element_shape);

    xla::XlaBuilder* b = ctx->builder();
    ctx->SetTensorListOutput(
        0, xla::Tuple(b, {xla::Broadcast(XlaHelpers::Zero(b, dtype_),
                                         tensor_shape.dim_sizes()),
                          xla::ConstantR0<int32>(b, num_elements)}));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListReserveOp);
};

REGISTER_XLA_OP(Name("TensorListReserve")
                    .CompileTimeConstantInput("element_shape")
                    .CompileTimeConstantInput("num_elements"),
                TensorListReserveOp);

class EmptyTensorListOp : public XlaOpKernel {
 public:
  explicit EmptyTensorListOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    TensorShape element_shape;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsShape(0, &element_shape));
    int64 max_num_elements;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsIntScalar(1, &max_num_elements));
    OP_REQUIRES(
        ctx, max_num_elements >= 0,
        errors::InvalidArgument("XLA compilation requires a fixed tensor list "
                                "size. Set the max number of elements."));

    TensorShape tensor_shape;
    tensor_shape.AddDim(max_num_elements);
    tensor_shape.AppendShape(element_shape);

    xla::XlaBuilder* b = ctx->builder();
    ctx->SetTensorListOutput(
        0, xla::Tuple(b, {xla::Broadcast(XlaHelpers::Zero(b, dtype_),
                                         tensor_shape.dim_sizes()),
                          xla::ConstantR0<int32>(b, 0)}));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(EmptyTensorListOp);
};

REGISTER_XLA_OP(Name("EmptyTensorList")
                    .CompileTimeConstantInput("element_shape")
                    .CompileTimeConstantInput("max_num_elements"),
                EmptyTensorListOp);

class TensorListElementShapeOp : public XlaOpKernel {
 public:
  explicit TensorListElementShapeOp(OpKernelConstruction* ctx)
      : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("shape_type", &shape_type_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* b = ctx->builder();
    TensorShape shape;
    OP_REQUIRES_OK(ctx, GetTensorListShape(b, ctx->Input(0), &shape));
    shape.RemoveDim(0);

    switch (shape_type_) {
      case DT_INT64:
        ctx->SetOutput(0, xla::ConstantR1<int64>(b, shape.dim_sizes()));
        break;
      case DT_INT32: {
        std::vector<int32> size;
        for (int64 s : shape.dim_sizes()) {
          size.push_back(s);
        }
        ctx->SetOutput(0, xla::ConstantR1<int32>(b, size));
        break;
      }
      default:
        ctx->CtxFailure(
            errors::InvalidArgument("Unsupported shape type requested"));
        return;
    }
  }

 private:
  DataType shape_type_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListElementShapeOp);
};

REGISTER_XLA_OP(Name("TensorListElementShape"), TensorListElementShapeOp);

class TensorListGetItemOp : public XlaOpKernel {
 public:
  explicit TensorListGetItemOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* b = ctx->builder();
    xla::XlaOp state = ctx->Input(0);

    TensorShape shape;
    OP_REQUIRES_OK(ctx, GetTensorListShape(b, state, &shape));

    xla::XlaOp ta = xla::GetTupleElement(state, 0);
    xla::XlaOp index = ctx->Input(1);

    // start_indices of the DynamicSlice are [index, 0, 0, ..., 0].
    std::vector<xla::XlaOp> start_indices(shape.dims(),
                                          xla::ConstantR0<int32>(b, 0));
    start_indices[0] = index;
    auto slice_shape = shape.dim_sizes();
    slice_shape[0] = 1LL;

    xla::XlaOp read = xla::DynamicSlice(ta, start_indices, slice_shape);
    // Remove the leading '1' dimension.
    std::vector<int64> value_shape(slice_shape.begin() + 1, slice_shape.end());

    ctx->SetOutput(0, xla::Reshape(read, value_shape));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListGetItemOp);
};

REGISTER_XLA_OP(Name("TensorListGetItem"), TensorListGetItemOp);

class TensorListStackOp : public XlaOpKernel {
 public:
  explicit TensorListStackOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaOp state = ctx->Input(0);
    xla::XlaOp ta = xla::GetTupleElement(state, 0);
    ctx->SetOutput(0, ta);
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListStackOp);
};

REGISTER_XLA_OP(Name("TensorListStack"), TensorListStackOp);

class TensorListFromTensorOp : public XlaOpKernel {
 public:
  explicit TensorListFromTensorOp(OpKernelConstruction* ctx)
      : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    TensorShape element_shape;
    OP_REQUIRES_OK(ctx, ctx->ConstantInputAsShape(1, &element_shape));

    const TensorShape tensor_shape = ctx->InputShape(0);
    OP_REQUIRES(ctx, tensor_shape.dims() > 0,
                errors::InvalidArgument("Input value must be at least a "
                                        "vector but received shape: ",
                                        tensor_shape.DebugString()));
    const int num_elements = tensor_shape.dim_size(0);

    xla::XlaBuilder* b = ctx->builder();
    const xla::XlaOp tensor = ctx->Input(0);

    ctx->SetTensorListOutput(
        0, xla::Tuple(b, {tensor, xla::ConstantR0<int32>(b, num_elements)}));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListFromTensorOp);
};

REGISTER_XLA_OP(
    Name("TensorListFromTensor").CompileTimeConstantInput("element_shape"),
    TensorListFromTensorOp);

class TensorListSetItemOp : public XlaOpKernel {
 public:
  explicit TensorListSetItemOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* b = ctx->builder();
    xla::XlaOp tl = ctx->Input(0);
    TensorShape elem_shape = ctx->InputShape(2);

    xla::XlaOp ta = xla::GetTupleElement(tl, 0);
    xla::XlaOp index = ctx->Input(1);
    xla::XlaOp value = ctx->Input(2);

    // start_indices of the DynamicUpdateSlice are [index, 0, 0, ..., 0].
    std::vector<xla::XlaOp> start_indices(elem_shape.dims() + 1,
                                          xla::ConstantR0<int32>(b, 0));
    start_indices[0] = index;

    TensorShape slice_shape = elem_shape;
    slice_shape.InsertDim(0, 1LL);
    auto update = xla::Reshape(value, slice_shape.dim_sizes());

    ctx->SetTensorListOutput(
        0, xla::Tuple(b, {xla::DynamicUpdateSlice(ta, update, start_indices),
                          index + xla::ConstantR0<int32>(b, 1)}));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListSetItemOp);
};

REGISTER_XLA_OP(Name("TensorListSetItem"), TensorListSetItemOp);

class TensorListPushBackOp : public XlaOpKernel {
 public:
  explicit TensorListPushBackOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* b = ctx->builder();
    xla::XlaOp tl = ctx->Input(0);
    TensorShape elem_shape = ctx->InputShape(1);

    xla::XlaOp ta = xla::GetTupleElement(tl, 0);
    xla::XlaOp index = xla::GetTupleElement(tl, 1);
    xla::XlaOp value = ctx->Input(1);

    // start_indices of the DynamicUpdateSlice are [index, 0, 0, ..., 0].
    std::vector<xla::XlaOp> start_indices(elem_shape.dims() + 1,
                                          xla::ConstantR0<int32>(b, 0));
    start_indices[0] = index;

    TensorShape slice_shape = elem_shape;
    slice_shape.InsertDim(0, 1LL);
    auto update = xla::Reshape(value, slice_shape.dim_sizes());

    ctx->SetTensorListOutput(
        0, xla::Tuple(b, {xla::DynamicUpdateSlice(ta, update, start_indices),
                          index + xla::ConstantR0<int32>(b, 1)}));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListPushBackOp);
};

REGISTER_XLA_OP(Name("TensorListPushBack"), TensorListPushBackOp);

class TensorListPopBackOp : public XlaOpKernel {
 public:
  explicit TensorListPopBackOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("element_dtype", &dtype_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    xla::XlaBuilder* b = ctx->builder();
    xla::XlaOp state = ctx->Input(0);

    TensorShape shape;
    OP_REQUIRES_OK(ctx, GetTensorListShape(b, state, &shape));

    xla::XlaOp ta = xla::GetTupleElement(state, 0);
    xla::XlaOp index = xla::GetTupleElement(state, 1);

    index = index - xla::ConstantR0<int32>(b, 1);

    // start_indices of the DynamicSlice are [index, 0, 0, ..., 0].
    std::vector<xla::XlaOp> start_indices(shape.dims(),
                                          xla::ConstantR0<int32>(b, 0));
    start_indices[0] = index;
    auto slice_shape = shape.dim_sizes();
    slice_shape[0] = 1LL;

    xla::XlaOp read = xla::DynamicSlice(ta, start_indices, slice_shape);
    // Remove the leading '1' dimension.
    std::vector<int64> value_shape(slice_shape.begin() + 1, slice_shape.end());

    ctx->SetTensorListOutput(0, xla::Tuple(b, {ta, index}));
    ctx->SetOutput(1, xla::Reshape(read, value_shape));
  }

 private:
  DataType dtype_;

  TF_DISALLOW_COPY_AND_ASSIGN(TensorListPopBackOp);
};

REGISTER_XLA_OP(Name("TensorListPopBack"), TensorListPopBackOp);

}  // anonymous namespace
}  // namespace tensorflow
