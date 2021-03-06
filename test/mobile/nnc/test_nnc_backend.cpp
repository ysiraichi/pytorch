#include <gtest/gtest.h>
#include <torch/csrc/jit/backends/backend.h>
#include <torch/csrc/jit/backends/backend_detail.h>
#include <torch/csrc/jit/backends/backend_preprocess.h>
#include <torch/csrc/jit/frontend/resolver.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/mobile/nnc/context.h>
#include <torch/csrc/jit/mobile/nnc/registry.h>
#include <torch/custom_class.h>
#include <torch/script.h>
#include <ATen/Functions.h>

namespace torch {
namespace jit {
namespace mobile {
namespace nnc {

namespace {

c10::Dict<c10::IValue, c10::IValue> create_compile_spec(
    const std::string& method_name,
    const std::string& nnc_kernel_id,
    const std::vector<std::vector<int64_t>>& input_shapes,
    const std::vector<std::vector<int64_t>>& output_shapes,
    const c10::impl::GenericList& parameters,
    const std::vector<int64_t>& buffer_sizes) {
  c10::Dict<c10::IValue, c10::IValue> method_spec(
      c10::StringType::get(), c10::AnyType::get());
  method_spec.insert("nnc_kernel_id", nnc_kernel_id);
  method_spec.insert("input_sizes", input_shapes);
  method_spec.insert("output_sizes", output_shapes);

  // For testing purpose we don't call the real NNC so pass in these directly.
  method_spec.insert("parameters", parameters);
  method_spec.insert("buffer_sizes", buffer_sizes);

  c10::Dict<c10::IValue, c10::IValue> compile_spec(
      c10::StringType::get(), c10::AnyType::get());
  compile_spec.insert(method_name, method_spec);
  return compile_spec;
}

std::vector<mobile::nnc::InputSpec> get_input_specs(
    const c10::Dict<c10::IValue, c10::IValue>& method_compile_spec) {
  auto input_shapes = method_compile_spec.at("input_sizes").toList();

  std::vector<mobile::nnc::InputSpec> specs;
  for (const auto& input_shape : input_shapes) {
    mobile::nnc::InputSpec spec;
    spec.sizes_ = ((c10::IValue) input_shape).toIntVector();
    spec.dtype_ = c10::ScalarType::Float;
    specs.emplace_back(std::move(spec));
  }
  return specs;
}

std::vector<mobile::nnc::OutputSpec> get_output_specs(
    const c10::Dict<c10::IValue, c10::IValue>& method_compile_spec) {
  auto output_shapes = method_compile_spec.at("output_sizes").toList();

  std::vector<mobile::nnc::OutputSpec> specs;
  for (const auto& output_shape : output_shapes) {
    mobile::nnc::OutputSpec spec;
    spec.sizes_ = ((c10::IValue) output_shape).toIntVector();
    spec.dtype_ = c10::ScalarType::Float;
    specs.emplace_back(std::move(spec));
  }
  return specs;
}

// A fake NNC preprocess method, which only produces the compiled model but
// does not produce the assembly with the NNC compiler.
c10::IValue preprocess(
    const torch::jit::Module& /* mod */,
    const c10::Dict<c10::IValue, c10::IValue>& method_compile_spec,
    const torch::jit::BackendDebugHandleGenerator&) {
  torch::jit::mobile::nnc::CompilationUnit cu;
  for (const auto& entry : method_compile_spec) {
    const std::string& method_name = entry.key().toStringRef();
    auto compile_spec = entry.value().toGenericDict();

    auto func = std::make_unique<mobile::nnc::Function>();
    func->set_name(method_name);
    func->set_nnc_kernel_id(compile_spec.at("nnc_kernel_id").toStringRef());
    func->set_input_specs(get_input_specs(compile_spec));
    func->set_output_specs(get_output_specs(compile_spec));

    func->set_parameters(compile_spec.at("parameters").toList());

    mobile::nnc::MemoryPlan plan;
    plan.buffer_sizes_ = compile_spec.at("buffer_sizes").toIntVector();
    func->set_memory_plan(plan);

    cu.register_function(std::move(func));
  }
  return cu.serialize();
}

static auto reg = torch::jit::backend_preprocess_register("nnc", preprocess);

struct FakeTensor : torch::CustomClassHolder {
  explicit FakeTensor(std::vector<int64_t> data) : data_(std::move(data)) {}
  int64_t get() {
    return data_[0];
  }
  std::vector<int64_t> data_;
};

TORCH_LIBRARY(_TorchScriptTesting, m) {
  m.class_<FakeTensor>("_MobileNNCFakeTensor")
      .def(torch::init<std::vector<int64_t>>())
      .def("get", &FakeTensor::get)
      .def_pickle(
          [](c10::intrusive_ptr<FakeTensor> self) { // __getstate__
            return self->data_;
          },
          [](std::vector<int64_t> state) { // __setstate__
            return c10::make_intrusive<FakeTensor>(std::move(state));
          });
}

} // namespace

extern "C" {

// The test kernels are supposed to be generated by the NNC compiler ahead-of-
// time. For integration test purpose we manually wrote instead.
int add_kernel(void** args) {
  // out = input + param
  at::Tensor input = at::from_blob(args[0], {4, 4}, at::kFloat);
  at::Tensor out = at::from_blob(args[1], {4, 4}, at::kFloat);
  at::Tensor param = at::from_blob(args[2], {1}, at::kFloat);
  out.copy_(at::add(input, param));
  return 0;
}

int fake_tensor_add_kernel(void** args) {
  // out = input + param.get()
  at::Tensor input = at::from_blob(args[0], {4, 4}, at::kFloat);
  at::Tensor out = at::from_blob(args[1], {4, 4}, at::kFloat);
  FakeTensor* param = reinterpret_cast<FakeTensor*>(args[2]);
  out.copy_(at::add(input, param->get()));
  return 0;
}

} // extern "C"

REGISTER_NNC_KERNEL("_add_kernel", add_kernel)
REGISTER_NNC_KERNEL("_fake_tensor_add_kernel", fake_tensor_add_kernel)

TEST(NNCBackendTest, AOTCompileThenExecute) {
  torch::jit::Module m("m");
  auto param = torch::ones({});
  m.register_parameter("param", param, false);
  m.define(R"(
    def forward(self, input):
        return input + self.param
  )");

  // Run the TorchScript module to get reference result.
  std::vector<IValue> inputs;
  inputs.emplace_back(2.0 * torch::ones({4, 4}));
  auto reference = m.forward(inputs);

  // Compile the model with NNC.
  auto compile_spec = create_compile_spec(
      "forward",
      "_add_kernel",
      {{4, 4}},
      {{4, 4}},
      c10::impl::toList(c10::List<at::Tensor>({param})),
      {});
  auto any_dict_ty =
      c10::DictType::create(c10::StringType::get(), c10::AnyType::get());
  auto compiled_module = torch::jit::detail::codegen_backend_module(
      "nnc", m, compile_spec, any_dict_ty);

  // Save the compiled model.
  std::stringstream ss;
  compiled_module._save_for_mobile(ss);

  // Load and run the saved model.
  auto loaded_module = _load_for_mobile(ss);
  auto result = loaded_module.forward(inputs);
  EXPECT_TRUE(result.toTensor().equal(3.0 * torch::ones({4, 4})));
  EXPECT_TRUE(result.toTensor().equal(reference.toTensor()));
}

TEST(NNCBackendTest, FakeTensor) {
  script::Module m("m");
  auto param_cls = getCustomClass(
      "__torch__.torch.classes._TorchScriptTesting._MobileNNCFakeTensor");
  auto param_value = c10::make_intrusive<FakeTensor>(std::vector<int64_t>({3}));
  m.register_attribute("param", param_cls, param_value, false);
  m.define(
      R"(
        def forward(self, input):
            return input + self.param.get()
      )");

  // Run the TorchScript module to get reference result.
  std::vector<IValue> inputs;
  inputs.emplace_back(2.0 * torch::ones({4, 4}));
  auto reference = m.forward(inputs);

  // Compile the model with NNC.
  auto params = c10::impl::GenericList(c10::AnyType::get());
  params.emplace_back(param_value);
  auto compile_spec = create_compile_spec(
      "forward",
      "_fake_tensor_add_kernel",
      {{4, 4}},
      {{4, 4}},
      params,
      {});
  auto any_dict_ty =
      c10::DictType::create(c10::StringType::get(), c10::AnyType::get());
  auto compiled_module = torch::jit::detail::codegen_backend_module(
      "nnc", m, compile_spec, any_dict_ty);

  // Save the compiled model.
  std::stringstream ss;
  compiled_module._save_for_mobile(ss);

  // Load and run the saved model.
  auto loaded_module = _load_for_mobile(ss);
  auto result = loaded_module.forward(inputs);
  EXPECT_TRUE(result.toTensor().equal(5.0 * torch::ones({4, 4})));
  EXPECT_TRUE(result.toTensor().equal(reference.toTensor()));
}

} // namespace nnc
} // namespace mobile
} // namespace jit
} // namespace torch
