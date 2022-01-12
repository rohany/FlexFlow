#include "flexflow/ops/element_binary.h"
#include "legion/legion_utilities.h"
#include "flexflow/utils/hash_utils.h"

namespace FlexFlow {
  
// declare Legion names
using Legion::Context;
using Legion::Runtime;
using Legion::Domain;
using Legion::Task;
using Legion::Rect;
using Legion::PhysicalRegion;
using Legion::coord_t;
using Legion::TaskLauncher;
using Legion::IndexLauncher;
using Legion::FutureMap;
using Legion::ArgumentMap;
using Legion::TaskArgument;
using Legion::RegionRequirement;
using Legion::Predicate;

Tensor FFModel::binary(OperatorType op,
                       const Tensor in1,
                       const Tensor in2,
                       bool inplace_a,
                       char const *name)
{
  Layer* ele = nullptr;
  DataType dtype;
  assert(in1->num_dims == in2->num_dims);
  for (int i = 0; i < in1->num_dims; i++) {
    assert(in1->dims[i] == in2->dims[i]);
  }
  if (in1->data_type < in2->data_type) {
    dtype = in2->data_type;
    std::string str(name);
    Tensor new_in1 = cast(in1, dtype, (str+"input1_pre_cast").c_str());
    ele = new Layer(this, op, name, 2/*inputs*/, 0/*weights*/, 1/*outputs*/, new_in1, in2);
  } else if (in1->data_type > in2->data_type) {
    dtype = in1->data_type;
    std::string str(name);
    Tensor new_in2 = cast(in2, dtype, (str+"input2_pre_cast").c_str());
    ele = new Layer(this, op, name, 2/*inputs*/, 0/*weights*/, 1/*outputs*/, in1, new_in2);
  } else {
    dtype = in1->data_type;
    ele = new Layer(this, op, name, 2/*inputs*/, 0/*weights*/, 1/*outputs*/, in1, in2);
  }
  ele->outputs[0] = create_tensor_legion_ordering(
      in1->num_dims, in1->dims, dtype, ele, 0, true/*create_grad*/);
  ele->add_int_property("inplace_a", inplace_a);
  layers.push_back(ele);
  return ele->outputs[0];
}

Op* ElementBinary::create_operator_from_layer(
    FFModel& model,
    const Layer* layer,
    const std::vector<ParallelTensor>& inputs) {
  long long value;
  layer->get_int_property("inplace_a", value);
  bool inplace_a = (bool) value;
  return new ElementBinary(model, layer->op_type, inputs[0], inputs[1],
      inplace_a, layer->name);
}

Tensor FFModel::add(const Tensor in1,
                    const Tensor in2,
                    bool inplace_a,
                    char const *name)
{
  return this->binary(OP_EW_ADD, in1, in2, inplace_a, name);
}

Tensor FFModel::subtract(const Tensor in1,
                         const Tensor in2,
                         bool inplace_a,
                         char const *name)
{
  return this->binary(OP_EW_SUB, in1, in2, inplace_a, name);
}

Tensor FFModel::multiply(const Tensor in1,
                         const Tensor in2,
                         bool inplace_a,
                         char const *name)
{
  return this->binary(OP_EW_MUL, in1, in2, inplace_a, name);
}

Tensor FFModel::divide(const Tensor in1,
                       const Tensor in2,
                       bool inplace_a,
                       char const *name)
{
  return this->binary(OP_EW_DIV, in1, in2, inplace_a, name);
}

ElementBinary::ElementBinary(FFModel& model,
                             OperatorType _op_type,
                             const ParallelTensor in1,
                             const ParallelTensor in2,
                             bool _inplace_a,
                             const char* name)
: Op(
    model,
    _op_type,
    name,
    2/*inputs*/,
    0/*weights*/,
    1/*outputs*/,
    in1,
    in2
  ),
  inplace_a(_inplace_a)
{
  numOutputs = 1;
  numWeights = 0;
  assert(in1->data_type == in2->data_type);
  int numdim = std::max(in1->num_dims, in2->num_dims);
  ParallelDim dims[MAX_TENSOR_DIM];
  for (int i = 0; i < numdim; i++) {
    assert(in1->dims[i] == in2->dims[i]);
    dims[i] = in1->dims[i];
  }
  outputs[0] = model.create_parallel_tensor_legion_ordering(numdim, dims, in1->data_type, this);
}

bool ElementBinary::can_inplace_output(void)
{
  if (op_type == OP_EW_ADD || op_type == OP_EW_MUL) {
    // TODO: Currently assume that we always inplace_a
    if (outputs[0]->num_dims != inputs[0]->num_dims)
      return false;
    for (int i = 0; i < inputs[0]->num_dims; i++) {
      if (inputs[0]->dims[i] != outputs[0]->dims[i])
        return false;
    }
    return true;
  }
  return false;
}

bool ElementBinary::has_inplace_output(void)
{
  return inplace_a;
}

void ElementBinary::do_inplace_output(void)
{
  inplace_a = true;
}

void ElementBinary::init(const FFModel& ff)
{
  assert(check_output_input_weight_same_parallel_is());
  parallel_is = outputs[0]->parallel_is;
  ArgumentMap argmap;
  Context ctx = ff.config.lg_ctx;
  Runtime* runtime = ff.config.lg_hlr;
  set_argumentmap_for_init(ff, argmap);
  IndexLauncher launcher(ELEMENTBINARY_INIT_TASK_ID, parallel_is,
                         TaskArgument(this, sizeof(ElementBinary)), argmap,
                         Predicate::TRUE_PRED, false/*must*/, 0/*mapper_id*/,
                         outputs[0]->machine_view.hash());
  int rid = 0;
  launcher.add_region_requirement(
    RegionRequirement(inputs[0]->part, 0/*projection id*/,
      READ_WRITE, EXCLUSIVE, inputs[0]->region));
  launcher.add_field(rid++, FID_DATA);
  if (!has_same_operands) {
    launcher.add_region_requirement(
      RegionRequirement(inputs[1]->part, 0/*projection id*/,
        READ_WRITE, EXCLUSIVE, inputs[1]->region));
    launcher.add_field(rid++, FID_DATA);
  }
  if (!inplace_a) {
    launcher.add_region_requirement(
      RegionRequirement(outputs[0]->part, 0/*projection id*/,
        WRITE_ONLY, EXCLUSIVE, outputs[0]->region));
    launcher.add_field(rid++, FID_DATA);
  } else {
    assert(outputs[0]->part == inputs[0]->part);
    assert(outputs[0]->region == inputs[0]->region);
  }
  //launcher.add_region_requirement(
  //  RegionRequirement(input_grad_lps[0], 0/*projection id*/,
  //    WRITE_ONLY, EXCLUSIVE, inputs[0]->region_grad));
  //launcher.add_field(3, FID_DATA);
  //if (inputs[0]->region_grad != inputs[1]->region_grad) {
    // regions[4](I/O): input1_grad
  //  launcher.add_region_requirement(
  //    RegionRequirement(input_grad_lps[1], 0/*projection id*/,
  //                      WRITE_ONLY, EXCLUSIVE, inputs[1]->region_grad));
  //  launcher.add_field(4, FID_DATA);
  //}
  FutureMap fm = runtime->execute_index_space(ctx, launcher);
  fm.wait_all_results();
  set_opmeta_from_futuremap(ff, fm);
}

OpMeta* ElementBinary::init_task(const Task* task,
                                 const std::vector<PhysicalRegion> &regions,
                                 Context ctx, Runtime* runtime)
{
  ElementBinary* eb = (ElementBinary*) task->args;
  FFHandler handle = *((FFHandler*) task->local_args);
  ElementBinaryMeta* m = new ElementBinaryMeta(handle);
  m->op_type = eb->op_type;
  m->profiling = eb->profiling;
  m->inplace_a = eb->inplace_a;
  m->has_same_operands = eb->has_same_operands;
  Domain input1_domain = runtime->get_index_space_domain(
    ctx, task->regions[0].region.get_index_space());
  Domain input2_domain, output_domain;
  size_t num_regions = 1;
  if (!m->has_same_operands) {
    input2_domain = runtime->get_index_space_domain(
        ctx, task->regions[num_regions].region.get_index_space());
    num_regions ++;
  } else {
    input2_domain = input1_domain;
  }
  if (!m->inplace_a) {
    output_domain = runtime->get_index_space_domain(
        ctx, task->regions[num_regions].region.get_index_space());
    num_regions ++;
    // check that input can broadcast to output
    for (int i = 0; i < output_domain.dim; i++) {
      int output_dim_size = output_domain.hi()[i] - output_domain.lo()[i] + 1;
      if (i < input1_domain.dim) {
        int input1_dim_size = input1_domain.hi()[i] - input1_domain.lo()[i] + 1;
        assert(input1_dim_size == output_dim_size || input1_dim_size == 1);
      }
      if (i < input2_domain.dim) {
        int input2_dim_size = input2_domain.hi()[i] - input2_domain.lo()[i] + 1;
        assert(input2_dim_size == output_dim_size || input2_dim_size == 1);
      }
    }
  } else {
    output_domain = input1_domain;
  }
  assert(task->regions.size() == regions.size());
  assert(regions.size() == num_regions);
  ElementBinary::init_kernel(m, input1_domain, input2_domain, output_domain);
  return m;
}

void ElementBinary::forward(const FFModel& ff)
{
  ArgumentMap argmap;
  Context ctx = ff.config.lg_ctx;
  Runtime* runtime = ff.config.lg_hlr;
  set_argumentmap_for_forward(ff, argmap);
  IndexLauncher launcher(ELEMENTBINARY_FWD_TASK_ID, parallel_is,
      TaskArgument(NULL, 0), argmap,
      Predicate::TRUE_PRED, false/*must*/, 0/*mapper_id*/,
      outputs[0]->machine_view.hash());
  if (inplace_a) {
    assert(outputs[0]->part == inputs[0]->part);
    assert(outputs[0]->region == inputs[0]->region);
    launcher.add_region_requirement(
      RegionRequirement(inputs[0]->part, 0/*projection id*/,
        READ_WRITE, EXCLUSIVE, inputs[0]->region));
    launcher.add_field(0, FID_DATA);
    if (has_same_operands) {
      // do nothing else
    } else {
      launcher.add_region_requirement(
        RegionRequirement(inputs[1]->part, 0/*projection id*/,
          READ_ONLY, EXCLUSIVE, inputs[1]->region));
      launcher.add_field(1, FID_DATA);
    }
  } else {
    launcher.add_region_requirement(
      RegionRequirement(inputs[0]->part, 0/*projection id*/,
        READ_ONLY, EXCLUSIVE, inputs[0]->region));
    launcher.add_field(0, FID_DATA);
    if (has_same_operands) {
      launcher.add_region_requirement(
        RegionRequirement(outputs[0]->part, 0/*projection id*/,
          WRITE_ONLY, EXCLUSIVE, outputs[0]->region));
      launcher.add_field(1, FID_DATA);
    } else {
      launcher.add_region_requirement(
        RegionRequirement(inputs[1]->part, 0/*projection id*/,
          READ_ONLY, EXCLUSIVE, inputs[1]->region));
      launcher.add_field(1, FID_DATA);
      launcher.add_region_requirement(
        RegionRequirement(outputs[0]->part, 0/*projection id*/,
          WRITE_ONLY, EXCLUSIVE, outputs[0]->region));
      launcher.add_field(2, FID_DATA);
    }
  }
  runtime->execute_index_space(ctx, launcher);
}

/*
  regions[0](I): in1
  regions[1](I): in2
  regions[2](O): output
*/
__host__
void ElementBinary::forward_task(const Task* task,
                                 const std::vector<PhysicalRegion> &regions,
                                 Context ctx, Runtime* runtime)
{
  //const ElementBinary* ele = (const ElementBinary*) task->args;
  const ElementBinaryMeta* m = *((ElementBinaryMeta**) task->local_args);
  Domain in1_domain = runtime->get_index_space_domain(
    ctx, task->regions[0].region.get_index_space());
  if (!m->has_same_operands) {
    Domain in2_domain = runtime->get_index_space_domain(
      ctx, task->regions[1].region.get_index_space());
    // Currently only support broadcast for add and sub
    if (in1_domain != in2_domain) {
      assert(m->op_type == OP_EW_SUB || m->op_type == OP_EW_ADD);
    }
  }
  const float* in1_ptr = NULL, *in2_ptr = NULL;
  float *out_ptr = NULL;
  if (m->inplace_a) {
    if (m->has_same_operands) {
      assert(regions.size() == 1);
      assert(task->regions.size() == 1);
      out_ptr = helperGetTensorPointerRW<float>(
          regions[0], task->regions[0], FID_DATA, ctx, runtime);
      in2_ptr = out_ptr;
      in1_ptr = out_ptr;
    } else {
      assert(regions.size() == 2);
      assert(task->regions.size() == 2);
      out_ptr = helperGetTensorPointerRW<float>(
          regions[0], task->regions[0], FID_DATA, ctx, runtime);
      in2_ptr = helperGetTensorPointerRO<float>(
          regions[1], task->regions[1], FID_DATA, ctx, runtime);
      in1_ptr = out_ptr;
    }
  } else {
    if (m->has_same_operands) {
      assert(regions.size() == 2);
      assert(task->regions.size() == 2);
      Domain out_domain = runtime->get_index_space_domain(
          ctx, task->regions[1].region.get_index_space());
      assert(out_domain == in1_domain);
      in1_ptr = helperGetTensorPointerRO<float>(
          regions[0], task->regions[0], FID_DATA, ctx, runtime);
      in2_ptr = in1_ptr;
      out_ptr = helperGetTensorPointerWO<float>(
          regions[1], task->regions[1], FID_DATA, ctx, runtime);
    } else {
      assert(regions.size() == 3);
      assert(task->regions.size() == 3);
      Domain out_domain = runtime->get_index_space_domain(
          ctx, task->regions[2].region.get_index_space());
      assert(out_domain == in1_domain);
      in1_ptr = helperGetTensorPointerRO<float>(
          regions[0], task->regions[0], FID_DATA, ctx, runtime);
      in2_ptr = helperGetTensorPointerRO<float>(
          regions[1], task->regions[1], FID_DATA, ctx, runtime);
      out_ptr = helperGetTensorPointerWO<float>(
          regions[2], task->regions[2], FID_DATA, ctx, runtime);
    }
  }

  ElementBinary::forward_kernel_wrapper(m, in1_ptr, in2_ptr, out_ptr);
}

void ElementBinary::backward(const FFModel& ff)
{
  ArgumentMap argmap;
  Context ctx = ff.config.lg_ctx;
  Runtime* runtime = ff.config.lg_hlr;
  set_argumentmap_for_backward(ff, argmap);
  IndexLauncher launcher(ELEMENTBINARY_BWD_TASK_ID, parallel_is,
      TaskArgument(NULL, 0), argmap,
      Predicate::TRUE_PRED, false/*must*/, 0/*mapper_id*/,
      outputs[0]->machine_view.hash());
  if (inplace_a) {
    // regions[0](I/O): output_grad
    launcher.add_region_requirement(
      RegionRequirement(outputs[0]->part_grad, 0/*projection id*/,
                        READ_WRITE, EXCLUSIVE, outputs[0]->region_grad));
    launcher.add_field(0, FID_DATA);
    // regions[1](I): input0
    launcher.add_region_requirement(
      RegionRequirement(inputs[0]->part, 0/*projection id*/,
                        READ_ONLY, EXCLUSIVE, inputs[0]->region));
    launcher.add_field(1, FID_DATA);
    if (inputs[0]->region == inputs[1]->region) {
      // regions[3](I): input1
      launcher.add_region_requirement(
        RegionRequirement(inputs[1]->part, 0/*projection id*/,
                          READ_ONLY, EXCLUSIVE, inputs[1]->region));
      launcher.add_field(2, FID_DATA);
      // regions[4](I/O): input1_grad
      launcher.add_region_requirement(
        RegionRequirement(inputs[1]->part_grad, 0/*projection id*/,
                          READ_WRITE, EXCLUSIVE, inputs[1]->region_grad));
      launcher.add_field(3, FID_DATA);
    }
  } else {
    // regions[0](I): output_grad
    launcher.add_region_requirement(
      RegionRequirement(outputs[0]->part_grad, 0/*projection id*/,
                        READ_ONLY, EXCLUSIVE, outputs[0]->region_grad));
    launcher.add_field(0, FID_DATA);
    // regions[1](I): input0
    launcher.add_region_requirement(
      RegionRequirement(inputs[0]->part, 0/*projection id*/,
                        READ_ONLY, EXCLUSIVE, inputs[0]->region));
    launcher.add_field(1, FID_DATA);
    // regions[2](I/O): input0_grad
    launcher.add_region_requirement(
      RegionRequirement(inputs[0]->part_grad, 0/*projection id*/,
                        READ_WRITE, EXCLUSIVE, inputs[0]->region_grad));
    launcher.add_field(2, FID_DATA);
    if (inputs[0]->region == inputs[1]->region) {
      // regions[3](I): input1
      launcher.add_region_requirement(
        RegionRequirement(inputs[1]->part, 0/*projection id*/,
                          READ_ONLY, EXCLUSIVE, inputs[1]->region));
      launcher.add_field(3, FID_DATA);
      // regions[4](I/O): input1_grad
      launcher.add_region_requirement(
        RegionRequirement(inputs[1]->part_grad, 0/*projection id*/,
                          READ_WRITE, EXCLUSIVE, inputs[1]->region_grad));
      launcher.add_field(4, FID_DATA);
    }
  }
  runtime->execute_index_space(ctx, launcher);
}

/*
  regions[0](I or I/O): out_grad (I/O if inplace_a)
  regions[1](I): in0
  regions[2](I/O): in0_grad (Missing if in0_grad = out_grad)
  regions[3](I): in1 (Missing if in0 = in1)
  regions[4](I/O): in1_grad (Missing if in0=in1)
*/
void ElementBinary::backward_task(const Task *task,
                            const std::vector<PhysicalRegion> &regions,
                            Context ctx, Runtime* runtime)
{
  //const ElementBinary* ele = (const ElementBinary*) task->args;
  const ElementBinaryMeta* m = *((ElementBinaryMeta**) task->local_args);
  const float *in0_ptr = NULL, *in1_ptr = NULL, *out_grad_ptr = NULL;
  float *in0_grad_ptr = NULL, *in1_grad_ptr = NULL;
  Domain out_grad_domain = runtime->get_index_space_domain(
    ctx, task->regions[0].region.get_index_space());
  if (m->inplace_a) {
    in0_grad_ptr = helperGetTensorPointerRW<float>(
      regions[0], task->regions[0], FID_DATA, ctx, runtime);
    assert(regions.size() == 2 || regions.size() == 4);
    assert(task->regions.size() == regions.size());
    if (regions.size() == 2) {
      Domain in0_domain = runtime->get_index_space_domain(
        ctx, task->regions[1].region.get_index_space());
      assert(in0_domain == out_grad_domain);
      in0_ptr = helperGetTensorPointerRO<float>(
        regions[1], task->regions[1], FID_DATA, ctx, runtime);
      in1_ptr = in0_ptr;
      in1_grad_ptr = in0_grad_ptr;
      out_grad_ptr = in0_grad_ptr;
    } else {
      Domain in0_domain = runtime->get_index_space_domain(
        ctx, task->regions[1].region.get_index_space());
      Domain in1_domain = runtime->get_index_space_domain(
        ctx, task->regions[2].region.get_index_space());
      assert(in0_domain == out_grad_domain);
      //assert(in1_domain == out_grad_domain);
      in0_ptr = helperGetTensorPointerRO<float>(
        regions[1], task->regions[1], FID_DATA, ctx, runtime);
      in1_ptr = helperGetTensorPointerRO<float>(
        regions[2], task->regions[2], FID_DATA, ctx, runtime);
      in1_grad_ptr = helperGetTensorPointerRW<float>(
        regions[3], task->regions[3], FID_DATA, ctx, runtime);
      out_grad_ptr = in0_grad_ptr;
    }
  } else {
    assert(regions.size() == 3 || regions.size() == 5);
    assert(task->regions.size() == regions.size());
    out_grad_ptr = helperGetTensorPointerRO<float>(
      regions[0], task->regions[0], FID_DATA, ctx, runtime);
    Domain in0_domain = runtime->get_index_space_domain(
      ctx, task->regions[1].region.get_index_space());
    Domain in0_grad_domain = runtime->get_index_space_domain(
      ctx, task->regions[2].region.get_index_space());
    assert(out_grad_domain == in0_grad_domain);
    assert(out_grad_domain == in0_domain);
    in0_ptr = helperGetTensorPointerRO<float>(
      regions[1], task->regions[1], FID_DATA, ctx, runtime);
    in0_grad_ptr = helperGetTensorPointerRW<float>(
      regions[2], task->regions[2], FID_DATA, ctx, runtime);
    if (regions.size() == 3) {
      // in0 == in1
      in1_ptr = in0_ptr;
      in1_grad_ptr = in0_grad_ptr;
    } else {
      Domain in1_domain = runtime->get_index_space_domain(
        ctx, task->regions[3].region.get_index_space());
      Domain in1_grad_domain = runtime->get_index_space_domain(
        ctx, task->regions[4].region.get_index_space());
      //assert(out_grad_domain == in1_domain);
      assert(in1_domain == in1_grad_domain);
      in1_ptr = helperGetTensorPointerRO<float>(
        regions[3], task->regions[3], FID_DATA, ctx, runtime);
      in1_grad_ptr = helperGetTensorPointerRW<float>(
        regions[4], task->regions[4], FID_DATA, ctx, runtime);
    }
  }

  ElementBinary::backward_kernel_wrapper(m, out_grad_ptr, in0_ptr, in1_ptr, in0_grad_ptr, in1_grad_ptr);
}

size_t ElementBinary::get_params_hash() const {
  size_t hash = this->inputs[0]->get_owner_independent_hash();
  hash_combine(hash, this->inputs[1]->get_owner_independent_hash());
  hash_combine(hash, this->op_type);

  return hash;
}

bool ElementBinary::measure_operator_cost(Simulator* sim,
                                          const ParallelConfig& pc,
                                          CostMetrics& cost_metrics) const
{
  ParallelTensorBase sub_output, sub_input1, sub_input2;
  if (!outputs[0]->get_output_sub_tensor(pc, sub_output, op_type))
    return false;
  if (!inputs[0]->get_input_sub_tensor(pc, sub_input1, op_type))
    return false;
  if (!inputs[1]->get_input_sub_tensor(pc, sub_input2, op_type))
    return false;
  ElementBinaryMeta* m = sim->ele_binary_meta;
  m->op_type = op_type;
  Domain input1_domain = sub_input1.get_domain();
  Domain input2_domain = sub_input2.get_domain();
  Domain output_domain = sub_output.get_domain();

  init_kernel(m, input1_domain, input2_domain, output_domain);

  sim->free_all();
  float* input1_ptr = (float*)sim->allocate(sub_input1.get_volume(), DT_FLOAT);
  assert(input1_ptr != NULL);
  float* input2_ptr = (float*)sim->allocate(sub_input2.get_volume(), DT_FLOAT);
  assert(input2_ptr != NULL);
  float* output_ptr = NULL;
  if (inplace_a) {
    output_ptr = input1_ptr;
  } else {
    output_ptr = (float*)sim->allocate(sub_output.get_volume(), DT_FLOAT);
  }
  assert(output_ptr != NULL);
  
  assert(m->profiling == false);

  std::function<void()> forward, backward;
  forward = [&] {
    forward_kernel_wrapper(m, input1_ptr, input2_ptr, output_ptr);
  };
  if (sim->computationMode == COMP_MODE_TRAINING) {
    float* input1_grad_ptr = (float*)sim->allocate(sub_input1.get_volume(), DT_FLOAT);
    assert(input1_grad_ptr != NULL);
    float* input2_grad_ptr = (float*)sim->allocate(sub_input2.get_volume(), DT_FLOAT);
    assert(input2_grad_ptr != NULL);
    float* output_grad_ptr = NULL;
    if (inplace_a) {
      output_grad_ptr = input1_grad_ptr;
    } else {
      output_grad_ptr = (float*)sim->allocate(sub_output.get_volume(), DT_FLOAT);
    }
    assert(output_grad_ptr != NULL);
    backward = [&] {
      backward_kernel_wrapper(m, output_grad_ptr, input1_ptr, input2_ptr, input1_grad_ptr, input2_grad_ptr);
    };
  }

  inner_measure_operator_cost(sim, forward, backward, cost_metrics);

  if (sim->computationMode == COMP_MODE_TRAINING) {
    log_measure.debug("[Measure Elewise Binary] name(%s) num_elements(%zu) forward_time(%.4lf) backward_time(%.4lf)\n",
        name, sub_output.get_volume(),
        cost_metrics.forward_time,
        cost_metrics.backward_time);
  } else {
    log_measure.debug("[Measure Elewise Binary] name(%s) num_elements(%zu) forward_time(%.4lf)\n",
        name, sub_output.get_volume(),
        cost_metrics.forward_time);
  }

  return true;
}

using PCG::Node;
Node FFModel::get_or_create_element_binary_node(const ParallelTensor input1,
                                                const ParallelTensor input2,
                                                OperatorType op_type)
{
  size_t hash = input1->get_owner_independent_hash();
  hash = hash * 31 + input2->get_owner_independent_hash();
  hash = hash * 31 + std::hash<int>()(op_type);
  const auto& it = cached_element_binary_ops.find(hash);
  ElementBinary* eb = NULL;
  if (it != cached_element_binary_ops.end()) {
    eb = it->second;
  } else {
    eb = new ElementBinary(*this, op_type, input1, input2, false/*inplace*/, NULL);
    cached_element_binary_ops[hash] = eb;
  }
  Node ret;
  ret.guid = node_global_guid ++;
  ret.ptr = eb;
  return ret;
}

}; // namespace FlexFlow
