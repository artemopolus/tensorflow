/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/pjrt/utils.h"

#include "absl/container/flat_hash_set.h"
#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/service/hlo.pb.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_sharding.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"

namespace xla {

namespace {
StatusOr<Shape> GetShardedShape(const Shape& shape,
                                const OpSharding& sharding) {
  if (sharding.type() == OpSharding::TUPLE) {
    if (!shape.IsTuple()) {
      return InvalidArgument(
          "Got tuple OpSharding (%s) for non-tuple shape (%s)",
          sharding.DebugString(), shape.ToString());
    }
    if (sharding.tuple_shardings_size() != shape.tuple_shapes_size()) {
      return InvalidArgument(
          "Got mismatched OpSharding tuple size (%d) and shape tuple size (%d)."
          " (OpSharding: %s, shape: %s)",
          sharding.tuple_shardings_size(), shape.tuple_shapes_size(),
          sharding.DebugString(), shape.ToString());
    }
    std::vector<Shape> sharded_subshapes;
    for (int i = 0; i < shape.tuple_shapes_size(); ++i) {
      TF_ASSIGN_OR_RETURN(
          Shape sharded_subshape,
          GetShardedShape(shape.tuple_shapes(i), sharding.tuple_shardings(i)));
      sharded_subshapes.emplace_back(std::move(sharded_subshape));
    }
    return ShapeUtil::MakeTupleShape(sharded_subshapes);
  }
  TF_ASSIGN_OR_RETURN(HloSharding hlo_sharding,
                      HloSharding::FromProto(sharding));
  return hlo_sharding.TileShape(shape);
}

StatusOr<Shape> GetShardedShape(const HloInstructionProto& instr) {
  const Shape unsharded_shape(instr.shape());
  Shape sharded_shape;
  if (instr.has_sharding()) {
    TF_ASSIGN_OR_RETURN(sharded_shape,
                        GetShardedShape(unsharded_shape, instr.sharding()));
  } else {
    sharded_shape = unsharded_shape;
  }
  LayoutUtil::ClearLayout(&sharded_shape);
  return sharded_shape;
}

}  // namespace

// Returns sharded (argument shapes, result shape) without layouts.
StatusOr<std::pair<std::vector<Shape>, Shape>> GetShardedProgramShapes(
    const XlaComputation& computation) {
  TF_ASSIGN_OR_RETURN(ProgramShape program_shape,
                      computation.GetProgramShape());
  std::vector<Shape> arg_shapes;
  arg_shapes.resize(program_shape.parameters_size());
  Shape result_shape;
  for (const HloComputationProto& comp : computation.proto().computations()) {
    if (comp.id() != computation.proto().entry_computation_id()) {
      continue;
    }
    for (const HloInstructionProto& instr : comp.instructions()) {
      if (instr.opcode() == HloOpcodeString(HloOpcode::kParameter)) {
        if (instr.parameter_number() >= program_shape.parameters_size()) {
          return InvalidArgument(
              "Got invalid parameter number %d, expected %d parameters",
              instr.parameter_number(), program_shape.parameters_size());
        }
        TF_ASSIGN_OR_RETURN(arg_shapes[instr.parameter_number()],
                            GetShardedShape(instr));
      }
      if (instr.id() == comp.root_id()) {
        if (result_shape.element_type() != PRIMITIVE_TYPE_INVALID) {
          return InvalidArgument("Found multiple root instructions");
        }
        TF_ASSIGN_OR_RETURN(result_shape, GetShardedShape(instr));
      }
    }
  }
  for (int i = 0; i < arg_shapes.size(); ++i) {
    if (arg_shapes[i].element_type() == PRIMITIVE_TYPE_INVALID) {
      return InvalidArgument("Couldn't find parameter %d", i);
    }
  }
  if (result_shape.element_type() == PRIMITIVE_TYPE_INVALID) {
    return InvalidArgument("Couldn't find root instruction");
  }
  return std::make_pair(arg_shapes, result_shape);
}

StatusOr<absl::flat_hash_set<int>> GetParametersThatMustBeDonated(
    const HloModule& module, bool tuple_inputs) {
  HloComputation* computation = module.entry_computation();
  int number_of_parameters = [&]() -> int {
    if (tuple_inputs) {
      CHECK_EQ(computation->num_parameters(), 1);
      const Shape& input_tuple_shape =
          computation->parameter_instruction(0)->shape();
      CHECK(input_tuple_shape.IsTuple());
      return input_tuple_shape.tuple_shapes_size();
    } else {
      return computation->num_parameters();
    }
  }();
  // If any buffer in a parameter is aliased we will donate the entire input
  // parameter.
  absl::flat_hash_set<int> parameters_to_donate;
  const HloInputOutputAliasConfig& config = module.input_output_alias_config();
  TF_RETURN_IF_ERROR(config.ForEachAliasWithStatus(
      [&](const ShapeIndex& output_index,
          const HloInputOutputAliasConfig::Alias& alias) {
        if (tuple_inputs) {
          if (alias.parameter_number != 0) {
            return InvalidArgument(
                "Unexpected parameter number %d in alias config with tupled "
                "inputs",
                alias.parameter_number);
          }
          const ShapeIndex& index = alias.parameter_index;
          if (!index.empty()) {
            int this_parameter = index.data()[0];
            if (this_parameter >= number_of_parameters) {
              return InvalidArgument(
                  "Unexpected parameter index %s in alias config with tupled "
                  "inputs and %d parameters",
                  index.ToString(), number_of_parameters);
            }
            parameters_to_donate.insert(this_parameter);
          }
        } else {
          int this_parameter = alias.parameter_number;
          if (this_parameter >= number_of_parameters) {
            return InvalidArgument(
                "Unexpected parameter number %d in alias config without tupled "
                "inputs and %d parameters",
                this_parameter, number_of_parameters);
          }
          parameters_to_donate.insert(this_parameter);
        }
        return Status::OK();
      }));
  return parameters_to_donate;
}

}  // namespace xla
