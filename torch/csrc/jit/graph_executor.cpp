#include "Python.h"
#include "torch/csrc/jit/graph_executor.h"
#include "torch/csrc/jit/ir.h"
#include "torch/csrc/jit/argument_spec.h"
#include "torch/csrc/jit/autodiff.h"
#include "torch/csrc/jit/interpreter.h"
#include "torch/csrc/autograd/grad_mode.h"
#include "torch/csrc/jit/passes/create_autodiff_subgraphs.h"
#include "torch/csrc/jit/passes/shape_analysis.h"
#include "torch/csrc/jit/passes/dead_code_elimination.h"
#include "torch/csrc/jit/passes/common_subexpression_elimination.h"
#include "torch/csrc/jit/passes/peephole.h"
#include "torch/csrc/jit/passes/graph_fuser.h"
#include "torch/csrc/jit/passes/inplace_check.h"
#include "torch/csrc/jit/passes/batch_mm.h"

#include "torch/csrc/autograd/function.h"

#include <unordered_map>

namespace torch { namespace jit {

namespace {

using tensor_list = std::vector<at::Tensor>;
using Variable = autograd::Variable;
using autograd::variable_list;

// this type is in ExecutionPlan to run its Gradient if it is
// specified. It has a list of inputs captured by ExecutionPlan that
// it concats with inputs to form the full set of inputs to graph.
// see struct Gradient for a description of how the derivative graph
// is constructed and what variables are captured.
struct ExecutionPlanAutogradFunction : public autograd::Function {
  ExecutionPlanAutogradFunction(GraphExecutor graph, size_t capture_size)
  : graph(std::move(graph)), captures(capture_size) {}
  virtual variable_list apply(const variable_list& inputs) override {
    // TODO: expensive copies here to convert to/from tensor_list
    // TODO: because inputs is passed by const reference there is no
    // way to release tensors incrementally as this runs
    variable_tensor_list all_inputs;
    all_inputs.reserve(captures.size() + inputs.size());
    for(auto & sv : captures) {
      all_inputs.push_back(sv.unpack(this->shared_from_this()));
    }
    all_inputs.insert(all_inputs.end(), inputs.begin(), inputs.end());
    auto tensors = graph.run(std::move(all_inputs));
    // TODO: another copy that needs to be removed
    return autograd::variable_list(tensors.begin(), tensors.end());
  }
private:
  friend struct ExecutionPlan;
  GraphExecutor graph;
  std::vector<autograd::SavedVariable> captures;
};


// an optimized way of executing the subgraph computed directly on
// tensors rather than Variables.
// This will unwrap Variables, run the plan, and re-wrap them.
// It can optionally also have a gradient which is hooked up
// to the output Variables if present.
struct ExecutionPlan {
  ExecutionPlan(std::shared_ptr<Graph> & graph)
  : f(graph) {}
  ExecutionPlan(std::shared_ptr<Graph> & graph, Gradient grad)
  : f(graph), grad(std::move(grad)), grad_executor(this->grad.df) {}

  variable_tensor_list run(variable_tensor_list inputs) const {
    if(grad) {
      return runWithGrad(std::move(inputs));
    }
    // TODO: interpreter needs to accept moved inputs
    // and delete incrementally
    tensor_list outputs;
    InterpreterState(f).runOneStage(unwrapVariables(std::move(inputs)), outputs);
    return wrapTensors(std::move(outputs));
  }
private:
  // inplace to avoid allocations
  tensor_list unwrapVariables(variable_tensor_list && list) const {
    for(auto & v : list) {
      v = v.defined() ? static_cast<Variable&>(v).data() : at::Tensor();
    }
    return std::move(list);
  }
  // inplace to avoid allocations
  variable_tensor_list wrapTensors(tensor_list && list) const {
    for(auto & v : list) {
      v = autograd::make_variable(v);
    }
    return variable_tensor_list(std::move(list));
  }
  // TODO: both captureInputs/captureOutputs have to iterate over the entire
  // capture list because the list is not sorted with inputs before outputs.
  // We cannot capture both, because we actually destroy our input lists (to release references)
  // before we get the outputs list.
  // Capture (save) inputs that would be required to subsequently run backwards
  void captureInputs(ExecutionPlanAutogradFunction & grad_fn, variable_tensor_list & inputs) const {
    auto & capture_desc = grad.df_input_captures;
    size_t N = capture_desc.size();
    for(size_t i = 0; i < N; ++i) {
      if(capture_desc[i].kind == Capture::Kind::Input) {
        size_t offset = capture_desc[i].offset;
        grad_fn.captures[i] = autograd::SavedVariable(static_cast<Variable&>(inputs[offset]), false);
      }
    }
  }
  void captureOutputs(ExecutionPlanAutogradFunction & grad_fn, variable_tensor_list & outputs) const {
    auto & capture_desc = grad.df_input_captures;
    size_t N = capture_desc.size();
    for(size_t i = 0; i < N; ++i) {
      if(capture_desc[i].kind == Capture::Kind::Output) {
        size_t offset = capture_desc[i].offset;
        grad_fn.captures[i] = autograd::SavedVariable(static_cast<Variable&>(outputs[offset]), true);
      }
    }
  }

  variable_tensor_list runWithGrad(variable_tensor_list&& inputs) const {
    auto grad_fn = std::make_shared<ExecutionPlanAutogradFunction>(grad_executor, grad.df_input_captures.size());
    // hook up the outputs of df to the gradient functions of the inputs that require
    // gradients
    for(auto idx : grad.df_output_vjps) {
      auto & v = static_cast<Variable&>(inputs[idx]);
      // TODO: this kinda stuff is _way_ to low level to the public API of variable.
      // Why do I have to care here whether v has a grad_fn or grad accumulator?
      // Why do I have to care here about output_nr? I just want to say
      // grad_fn->setOutputTo(i, v.input_port());
      grad_fn->next_functions.emplace_back(v.grad_fn() ? v.grad_fn() : v.grad_accumulator(), v.output_nr());
    }
    captureInputs(*grad_fn, inputs);

    tensor_list outputs_;
    InterpreterState(f).runOneStage(unwrapVariables(std::move(inputs)), outputs_);
    variable_tensor_list outputs = wrapTensors(std::move(outputs_));

    // hookup the gradients for the output tensors that require gradients
    // to the inputs to our gradient function df
    // TODO - XXX - if any output is the same tensor multiple times, views have to be
    // setup here. We need to refactor autograd until it is safe for
    // tensors to be constructed without all the viewing infrastructure.
    // this is currently intentionally not done here so we can get an idea of our
    // perf before introducing overhead for correctness
    for(auto idx : grad.df_input_vjps) {
      auto & o = static_cast<Variable&>(outputs[idx]);
      auto impl = o.get();
      // Note: we have to set this up in place, or we have to
      // throw away and reallocate variables that were already created in
      // wrapTensors. We should add an API for this, and more generally
      // we need to clean up the fields of Variable.
      impl->_grad_fn = grad_fn;
      impl->output_nr = grad_fn->num_inputs++;
      impl->_requires_grad = true;
    }
    captureOutputs(*grad_fn, outputs);
    // drop the temporary outputs so that we return the same number of
    // outputs as if we were not also calculating gradient
    outputs.erase(outputs.begin() + grad.f_real_outputs, outputs.end());
    return outputs;
  }
  Code f;
  // description of gradient as a graph
  Gradient grad; // if(grad) is false when this is unused
  // executor for df, including code caches
  GraphExecutor grad_executor;
};

} // anonymous namespace

// a Graph can be created via tracing, or via a language-based frontend
// GraphExecutor runs it. It can run the same graph on many different sizes
// and different requires_grad states, and handles specializations for each situation.
// GraphExecutor is completely unaware of tracing or module parameters to keep the
// tracing concerns separated.
struct GraphExecutorImpl {

  GraphExecutorImpl(std::shared_ptr<Graph> graph, bool optimize, bool symbolically_differentiable)
  : graph(std::move(graph))
  , optimize(optimize)
  , symbolically_differentiable(symbolically_differentiable) {}
  GraphExecutorImpl(std::shared_ptr<Graph> graph, bool optimize)
  : graph(std::move(graph))
  , optimize(optimize)
  , symbolically_differentiable(isDifferentiable(*this->graph)) {}

  // entry point where execution begins
  variable_tensor_list run(variable_tensor_list inputs) {
    // this is the fallback pathway, when we cannot differentiate
    if(!optimize || (!symbolically_differentiable && needsGradient(inputs))) {
      auto & fb = getOrCreateAutogradFallback();
      InterpreterState state(fb);
      tensor_list outputs;
      state.runOneStage(std::move(inputs), outputs);
      // note: we never unwrapped inputs, because we want autograd to record the trace
      return variable_tensor_list(std::move(outputs));
    }

    // either we can symbolically differentiate, or we do not need a gradient.
    // go down the route where we treat the inputs as tensors
    // and fully optimize
    auto & implementation = getOrCompile(inputs);
    return implementation.run(std::move(inputs));
  }

private:

  static bool needsGradient(const variable_tensor_list & inputs) {
    if (!autograd::GradMode::is_enabled()) {
      return false;
    }
    for (const auto & tensor : inputs) {
      if(tensor.defined() && static_cast<const Variable&>(tensor).requires_grad())
        return true;
    }
    return false;
  }
  static bool isDifferentiable(Graph & g) {
    for(auto n : g.nodes()) {
      if(!jit::isDifferentiable(n))
        return false;
    }
    return true;
  }

  void runOptimization(std::shared_ptr<Graph> & graph, bool graphMustSupportVariables) {

    // these optimizations must run in the presence of variables
    // and when shape information is not statically known.

    EliminateDeadCode(graph);
    CheckInplace(graph);
    EliminateCommonSubexpression(graph);

    if (!graphMustSupportVariables) {
      // These optimizations can introduce operators like FusionGroup that
      // do not work on variables

      // They also may assume that concrete sizes/strides are availiable

      //TODO: create peephole optimizations that are safe to run
      // when we are using variables, and when we do not know sizes.
      PeepholeOptimize(graph);
      // TODO: remove mandatory size checking in BatchMM, otherwise
      // it works fine on variables.
      BatchMM(graph);
      FuseGraph(graph);
    }
  }
  const Code & getOrCreateAutogradFallback() {
    std::lock_guard<std::mutex> lock(compile_mutex);
    if(autograd_fallback) {
      return autograd_fallback;
    }
    auto graph_ = graph->copy();
    if(optimize) {
      CreateAutodiffSubgraphs(*graph_);
      runOptimization(graph_, /*graphMustSupportVariables=*/true);
    }
    autograd_fallback = Code(graph_);
    return autograd_fallback;
  }
  const ExecutionPlan & getOrCompile(const variable_tensor_list & inputs) {
    // outside lock guard, to minimize the time holding the lock on the fast path
    // ArgumentSpec even computes its hashCode here.
    ArgumentSpec spec(autograd::GradMode::is_enabled(), inputs);
    {
      std::lock_guard<std::mutex> lock(compile_mutex);
      auto it = plan_cache.find(spec);
      if(it != plan_cache.end())
        return it->second;
      auto plan = compileSpec(spec);
      auto r = plan_cache.emplace(std::move(spec), std::move(plan));
      return r.first->second;
    }
  }
  bool needsGradient(const ArgumentSpec & spec) {
    for(size_t i = 0; i < spec.size(); ++i) {
      if(spec.tensorInfo(i).requires_grad())
        return true;
    }
    return false;
  }


  // remove ReplaceIfUndef(v, replacement) nodes that consume inputs with 'v' if
  // the input is defined, and 'replacement' if it is not.
  // Note: this is a very limited pass. It looks at undefined inputs,
  // and cleans up ReplaceIfUndef nodes inserted by autodiff.
  void specializeUndef(Graph & g, const ArgumentSpec & spec) {
    for(size_t i = 0; i < spec.size(); i++) {
      std::vector<Value*> to_replace;
      // do not edit in place, since it invalidates uses iterator
      for(auto u : g.inputs()[i]->uses()) {
        if(u.user->kind() == kReplaceIfUndef) {
          to_replace.push_back(u.user->output());
        }
      }
      for(auto v : to_replace) {
        // if it is defined, then we replace with 'v' if not,
        // we replace with 'replacement' which is normally just a zero tensor
        int idx = spec.tensorInfo(i).defined() ? 0 : 1;
        v->replaceAllUsesWith(v->node()->inputs()[idx]);
        v->node()->destroy();
      }
    }
  }
  // a + 0 -> a
  // 0 + a -> a
  void propagateZeros(Graph & g) {
    for(auto it = g.nodes().begin(); it != g.nodes().end(); ++it) {
      if(it->kind() == kadd && at::Scalar(it->t(kalpha)).toDouble() == 1.0) {
        if(isZero(it->inputs()[0])) {
          it->output()->replaceAllUsesWith(it->inputs()[1]);
          it.destroyCurrent();
        } else if(isZero(it->inputs()[1])) {
          it->output()->replaceAllUsesWith(it->inputs()[0]);
          it.destroyCurrent();
        }
      }
    }
  }
  void specializeToSpec(std::shared_ptr<Graph> g, const ArgumentSpec & spec) {

    // The following passes are specialized to clean up after autograd
    // decisions to insert/remove undefs nodes and to work before
    // we propagate input shapes.

    // clean up replaceIfUndef nodes
    specializeUndef(*g, spec);
    // clean up additions resulting from nodes that were in fact undefined
    propagateZeros(*g);
    // clean up dead constants from specialization
    EliminateDeadCode(g);
    // calculate all input shapes
    PropagateInputShapes(*g, spec);
  }
  ExecutionPlan compileSpec(const ArgumentSpec & spec) {
    auto graph_ = graph->copy();
    specializeToSpec(graph_, spec);
    if(!needsGradient(spec)) {
      runOptimization(graph_, /*graphMustSupportVariables=*/false);
      return ExecutionPlan(graph_);
    }
    JIT_ASSERT(symbolically_differentiable);

    std::vector<bool> requires_grads;
    requires_grads.reserve(spec.size());
    for(size_t i = 0; i < spec.size(); i++)
      requires_grads.push_back(spec.tensorInfo(i).requires_grad());

    Gradient gradient = differentiate(graph_, requires_grads);
    graph_ = gradient.f;
    runOptimization(graph_, /*graphMustSupportVariables=*/false);
    return ExecutionPlan(graph_, std::move(gradient));
  }
  // the unoptimized starting graph
  // this is never mutated
  std::shared_ptr<Graph> graph;

  // true - do everything we can to make this graph run fast
  // false - do not modifiy the graph at all and just use the interpreter
  // to run the graph. Useful for debugging correctness issues in the implementation
  bool optimize;

  // GraphExecutor optimizes more aggresively when we _know_ the graph will be
  // symbolically differentiable.
  bool symbolically_differentiable;

  // when this graph has some parts that are not symbolically_differentable,
  // but some input does require a derivative, we create and use autograd_fallback,
  // which wraps up the fully differentiable subgraphs, and then runs the outer
  // graph through autograd.
  // Since we can't optimize black box functions anyway, there is only one fallback path,
  // and it must work on all sizes (so no optimizations that inspect sizes can run on it)
  Code autograd_fallback;

  // optimizable code paths, used when we can differentiate or when no derivative is needed
  // Spec describes input conditions, Plan describes how to execute them.
  std::unordered_map<ArgumentSpec, ExecutionPlan> plan_cache;

  // GraphExecutor can be accessed from  multiple thread so
  // anytime we are checking or updating the autograd_fallback or
  // plan_cache, we must hold the compile mutex.
  // along the fast path (no compilation) code should
  // hold this for as little time as possible.
  std::mutex compile_mutex;
};

GraphExecutor::GraphExecutor(std::shared_ptr<Graph> graph, bool optimize)
: pImpl(new GraphExecutorImpl(std::move(graph), optimize)) {}

GraphExecutor::GraphExecutor(std::shared_ptr<Graph> graph, bool optimize, bool symbolically_differentiable)
: pImpl(new GraphExecutorImpl(std::move(graph), optimize, symbolically_differentiable)) {}

variable_tensor_list GraphExecutor::run(variable_tensor_list && inputs) {
  return pImpl->run(std::move(inputs));
}

}}
