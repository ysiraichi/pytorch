#include <torch/csrc/jit/tensorexpr/tensor.h>

#include <c10/util/Logging.h>
#include <torch/csrc/jit/tensorexpr/dim_arg.h>
#include <torch/csrc/jit/tensorexpr/reduction.h>

namespace torch {
namespace jit {
namespace tensorexpr {

Stmt* Tensor::lowerToStmt() const {
  Stmt* s = ElementStmt();

  // If this Tensor has no functional body, it already has its axes expanded.
  if (nullptr == body()) {
    return s;
  }

  if (ndim() == 0 && reduce_ndim() == 0) {
    return s;
  }

  const Expr* init_expr = buf()->initializer();

  std::vector<const Expr*> indices(args().begin(), args().end());

  if (reduce_ndim() > 0) {
    for (size_t i = 0; i < reduce_ndim(); i++) {
      // Going in reverse order: from innermost loop to the outermost
      size_t dim_index = reduce_ndim() - i - 1;
      s = new For(
          reduce_arg(dim_index), new IntImm(0), reduce_dim(dim_index), s);
    }
    if (init_expr) {
      Store* init = new Store(buf(), indices, init_expr, new IntImm(1));
      s = new Block({init, s});
    }
  }

  for (size_t i = 0; i < ndim(); i++) {
    // Going in reverse order: from innermost loop to the outermost
    size_t dim_index = ndim() - i - 1;
    s = new For(arg(dim_index), new IntImm(0), dim(dim_index), s);
  }
  return s;
}

Tensor* Compute(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const std::function<ExprHandle(const std::vector<VarHandle>&)>& body_func) {
  std::vector<const Expr*> dims;
  std::vector<const Var*> args;
  unpack_dim_args(dim_args, &dims, &args);
  const Expr* body = body_func(VarVectorToVarHandleVector(args)).node();
  return new Tensor(func_name, dims, args, body);
}

Tensor* Compute(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const std::function<ExprHandle(const VarHandle&)>& body_func) {
  if (dim_args.size() != 1) {
    throw malformed_input("mismatch between body and arg size (1)");
  }

  std::vector<const Expr*> dims;
  std::vector<const Var*> args;
  unpack_dim_args(dim_args, &dims, &args);
  const Expr* body = body_func(VarHandle(args[0])).node();
  return new Tensor(func_name, dims, args, body);
}

Tensor* Compute(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const std::function<ExprHandle(const VarHandle&, const VarHandle&)>&
        body_func) {
  if (dim_args.size() != 2) {
    throw malformed_input("mismatch between body and arg size (2)");
  }
  std::vector<const Expr*> dims;
  std::vector<const Var*> args;
  unpack_dim_args(dim_args, &dims, &args);
  const Expr* body = body_func(VarHandle(args[0]), VarHandle(args[1])).node();
  return new Tensor(func_name, dims, args, body);
}

Tensor* Compute(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const std::function<
        ExprHandle(const VarHandle&, const VarHandle&, const VarHandle&)>&
        body_func) {
  if (dim_args.size() != 3) {
    throw malformed_input("mismatch between body and arg size (3)");
  }
  std::vector<const Expr*> dims;
  std::vector<const Var*> args;
  unpack_dim_args(dim_args, &dims, &args);
  const Expr* body =
      body_func(VarHandle(args[0]), VarHandle(args[1]), VarHandle(args[2]))
          .node();
  return new Tensor(func_name, dims, args, body);
}

Tensor* Compute(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const std::function<ExprHandle(
        const VarHandle&,
        const VarHandle&,
        const VarHandle&,
        const VarHandle&)>& body_func) {
  if (dim_args.size() != 4) {
    throw malformed_input("mismatch between body and arg size (4)");
  }
  std::vector<const Expr*> dims;
  std::vector<const Var*> args_nodes;
  unpack_dim_args(dim_args, &dims, &args_nodes);
  auto args = VarVectorToVarHandleVector(args_nodes);
  const Expr* body = body_func(args[0], args[1], args[2], args[3]).node();
  return new Tensor(func_name, dims, args_nodes, body);
}

Stmt* Tensor::ElementStmt() const {
  std::vector<const Expr*> indices;
  for (size_t i = 0; i < buf_->ndim(); i++) {
    indices.push_back(args_[i]);
  }

  const Expr* mask = new IntImm(1);
  Stmt* update_stmt = new Store(buf_, indices, body_, mask);
  return update_stmt;
}

Tensor* Reduce(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const Reducer& reducer,
    const Placeholder& buffer,
    const std::vector<DimArg>& reduce_args) {
  return Reduce(
      func_name,
      dim_args,
      reducer,
      [&](ParameterList& p) { return buffer.load(p); },
      reduce_args);
}

Tensor* Reduce(
    const std::string& func_name,
    const std::vector<DimArg>& dim_args,
    const Reducer& reducer,
    Tensor* tensor,
    const std::vector<DimArg>& reduce_args) {
  return Reduce(
      func_name,
      dim_args,
      reducer,
      [&](ParameterList& p) { return tensor->call(p); },
      reduce_args);
}

} // namespace tensorexpr
} // namespace jit
} // namespace torch
