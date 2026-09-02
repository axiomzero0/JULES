#include "core/son/graph.h"

namespace jules {

const char* op_name(Op o) {
    switch (o) {
        case Op::Start:   return "Start";
        case Op::Region:  return "Region";
        case Op::If:      return "If";
        case Op::IfTrue:  return "IfTrue";
        case Op::IfFalse: return "IfFalse";
        case Op::Jump:    return "Jump";
        case Op::Return:  return "Return";
        case Op::Stop:    return "Stop";
        case Op::Const:   return "Const";
        case Op::Param:   return "Param";
        case Op::Phi:     return "Phi";
        case Op::Bin:     return "Bin";
        case Op::Cmp:     return "Cmp";
        case Op::Un:      return "Un";
        case Op::Cast:    return "Cast";
        case Op::Select:  return "Select";
        case Op::Load:    return "Load";
        case Op::Store:   return "Store";
        case Op::Alloc:   return "Alloc";
        case Op::Call:    return "Call";
        case Op::Dead:    return "Dead";
    }
    return "<op>";
}

const char* bin_name(BinOp b) {
    switch (b) {
        case BinOp::Add: return "add"; case BinOp::Sub: return "sub";
        case BinOp::Mul: return "mul"; case BinOp::Div: return "div";
        case BinOp::Mod: return "mod"; case BinOp::And: return "and";
        case BinOp::Or:  return "or";  case BinOp::Xor: return "xor";
        case BinOp::Shl: return "shl"; case BinOp::Shr: return "shr";
    }
    return "<bin>";
}

const char* cmp_name(CmpOp c) {
    switch (c) {
        case CmpOp::Eq: return "eq"; case CmpOp::Ne: return "ne";
        case CmpOp::Lt: return "lt"; case CmpOp::Le: return "le";
        case CmpOp::Gt: return "gt"; case CmpOp::Ge: return "ge";
    }
    return "<cmp>";
}

const char* un_name(UnOp u) {
    switch (u) {
        case UnOp::Neg: return "neg"; case UnOp::Not: return "not"; case UnOp::BNot: return "bnot";
    }
    return "<un>";
}

const char* cast_name(CastOp c) {
    switch (c) {
        case CastOp::ZExt: return "zext"; case CastOp::SExt: return "sext";
        case CastOp::Trunc: return "trunc"; case CastOp::SiToFp: return "sitofp";
        case CastOp::FpToSi: return "fptosi"; case CastOp::FpExt: return "fpext";
        case CastOp::FpTrunc: return "fptrunc"; case CastOp::Ptr: return "ptrcast";
    }
    return "<cast>";
}

NodeId Graph::make(Op op, TypeId ty, std::initializer_list<NodeId> ins, u8 sub, u32 aux) {
    Node n;
    n.op = op;
    n.ty = ty;
    n.sub = sub;
    n.aux = aux;
    u8 i = 0;
    for (NodeId v : ins) {
        if (i >= kMaxInputs) assert(false && "node input overflow");
        n.in[i++] = v;
    }
    n.n_in = i;
    nodes_.push_back(n);
    NodeId id = static_cast<NodeId>(nodes_.size() - 1);
    uses_dirty_ = true;
    return id;
}

NodeId Graph::make_arr(Op op, TypeId ty, const NodeId* ins, u8 n_in, u8 sub, u32 aux) {
    Node n;
    n.op = op;
    n.ty = ty;
    n.sub = sub;
    n.aux = aux;
    n.n_in = n_in < kMaxInputs ? n_in : kMaxInputs;
    for (u8 i = 0; i < n.n_in; ++i) n.in[i] = ins[i];
    nodes_.push_back(n);
    uses_dirty_ = true;
    return static_cast<NodeId>(nodes_.size() - 1);
}

void Graph::set_input(NodeId n, u8 idx, NodeId v) {
    Node& nd = node(n);
    assert(idx < kMaxInputs);
    if (nd.in[idx] == v) return;
    nd.in[idx] = v;
    if (idx >= nd.n_in) nd.n_in = idx + 1;
    uses_dirty_ = true;
}

void Graph::append_input(NodeId n, NodeId v) {
    Node& nd = node(n);
    assert(nd.n_in < kMaxInputs && "node input overflow (MVP arity limit)");
    nd.in[nd.n_in++] = v;
    uses_dirty_ = true;
}

u32 Graph::live_count() const {
    u32 c = 0;
    for (const Node& n : nodes_)
        if (n.op != Op::Dead) ++c;
    return c;
}

void Graph::rebuild_uses() {
    uses_.assign(nodes_.size(), {});
    for (NodeId u = 0; u < nodes_.size(); ++u) {
        const Node& n = node(u);
        for (u8 i = 0; i < n.n_in; ++i)
            if (n.in[i] != kNoNode) uses_[n.in[i]].push_back(u);
    }
    uses_dirty_ = false;
}

const SmallVec<NodeId, 4>& Graph::uses_of(NodeId n) {
    if (uses_dirty_) rebuild_uses();
    return uses_[n];
}

void Graph::replace_all_uses(NodeId from, NodeId to) {
    if (from == to) return;
    const SmallVec<NodeId, 4> users = uses_of(from); // copy: mutation-safe
    for (NodeId u : users) {
        Node& nd = node(u);
        for (u8 i = 0; i < nd.n_in; ++i)
            if (nd.in[i] == from) nd.in[i] = to;
    }
    uses_dirty_ = true;
    ++changes_;
}

void Graph::replace_uses_in_slot(NodeId from, NodeId to, u8 slot) {
    if (from == to) return;
    const SmallVec<NodeId, 4> users = uses_of(from);
    for (NodeId u : users) {
        Node& nd = node(u);
        if (slot < nd.n_in && nd.in[slot] == from) nd.in[slot] = to;
    }
    uses_dirty_ = true;
    ++changes_;
}

void Graph::replace_uses_as_memory(NodeId from, NodeId to) {
    if (from == to) return;
    const SmallVec<NodeId, 4> users = uses_of(from);
    for (NodeId u : users) {
        Node& nd = node(u);
        if (nd.op == Op::Phi) {
            for (u8 i = 1; i < nd.n_in; ++i)
                if (nd.in[i] == from) nd.in[i] = to;
        } else {
            if (nd.n_in > 1 && nd.in[1] == from) nd.in[1] = to;
        }
    }
    uses_dirty_ = true;
    ++changes_;
}

void Graph::kill(NodeId n) {
    Node& nd = node(n);
    if (nd.op == Op::Dead) return;
    nd.op = Op::Dead;
    // inputs are PRESERVED so repair passes can follow Dead -> in[1]; every
    // consumer checks op == Dead before interpreting a node.
    uses_dirty_ = true;
    ++changes_;
}

u64 Graph::pure_hash(NodeId n) const {
    const Node& nd = node(n);
    u64 h = fnv1a(&nd.op, sizeof nd.op);
    h = hash_mix(h, nd.sub);
    h = hash_mix(h, nd.ty);
    h = hash_mix(h, nd.aux);
    h = hash_mix(h, static_cast<u64>(nd.ival));
    h = hash_mix(h, nd.fval != 0 ? fnv1a(&nd.fval, sizeof nd.fval) : 0);
    for (u8 i = 1; i < nd.n_in; ++i)  // slot 0 = ctrl pin, excluded
        h = hash_mix(h, nd.in[i]);
    return h;
}

bool Graph::pure_equals(NodeId a, NodeId b) const {
    const Node& x = node(a);
    const Node& y = node(b);
    if (x.op != y.op || x.sub != y.sub || x.ty != y.ty) return false;
    if (x.op == Op::Const) {
        if (ty_is_float(x.ty)) return x.fval == y.fval;
        return x.ival == y.ival;
    }
    if (x.aux != y.aux) return false;
    if (x.n_in != y.n_in) return false;
    for (u8 i = 1; i < x.n_in; ++i)  // ctrl-insensitive
        if (x.in[i] != y.in[i]) return false;
    return true;
}

} // namespace jules
