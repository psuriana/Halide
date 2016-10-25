#include <iostream>
#include <limits>

#include "CodeGen_Coli.h"
#include "IROperator.h"
#include "IRMutator.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::ostream;
using std::set;
using std::string;
using std::vector;
using std::ostringstream;
using std::map;

namespace {
const string headers =
    "#include <isl/set.h>\n"
    "#include <isl/union_map.h>\n"
    "#include <isl/union_set.h>\n"
    "#include <isl/ast_build.h>\n"
    "#include <isl/schedule.h>\n"
    "#include <isl/schedule_node.h>\n\n"
    "#include <coli/debug.h>\n"
    "#include <coli/core.h>\n\n"
    "#include <string.h>\n"
    "#include <Halide.h>\n"
    "#include \"halide_image_io.h\"\n";

const int tab_size = 4;
}

template<typename T>
std::string to_string(const std::vector<T>& v) {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        ss << v[i];
        if (i != v.size() - 1) {
            ss << ", ";
        }
    }
    ss << "]";
    return ss.str();
}

void CodeGen_Coli::print(Expr e) {
    internal_assert(e.defined()) << "CodeGen_Coli can't convert undefined expr.\n";
    // For now, substitute in all lets to make life easier (does not substitute in lets in stmt though)
    e = substitute_in_all_lets(e);
    e.accept(this);
}

void CodeGen_Coli::print(Stmt s) {
    internal_assert(s.defined()) << "CodeGen_Coli can't convert undefined stmt.\n";
    // For now, substitute in all lets to make life easier (does not substitute in lets in stmt though)
    s = substitute_in_all_lets(s);
    s.accept(this);
}

namespace {
string print_name(const string &name) {
    ostringstream oss;

    // Prefix an underscore to avoid reserved words (e.g. a variable named "while")
    if (isalpha(name[0])) {
        oss << '_';
    }

    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] == '.') {
            oss << '_';
        } else if (name[i] == '$') {
            oss << "__";
        } else if (name[i] != '_' && !isalnum(name[i])) {
            oss << "___";
        }
        else oss << name[i];
    }
    return oss.str();
}

string halide_type_to_coli_type_str(Type type)
{
    if (type.is_uint()) {
        if (type.bits() == 8) {
            return "coli::p_uint8";
        } else if (type.bits() == 16) {
            return "coli::p_uint16";
        } else if (type.bits() == 32) {
            return "coli::p_uint32";
        } else {
            return "coli::p_uint64";
        }
    } else if (type.is_int()) {
        if (type.bits() == 8) {
            return "coli::p_int8";
        } else if (type.bits() == 16) {
            return "coli::p_int16";
        } else if (type.bits() == 32) {
            return "coli::p_int32";
        } else {
            return "coli::p_int64";
        }
    } else if (type.is_float()) {
        if (type.bits() == 32) {
            return "coli::p_float32";
        } else if (type.bits() == 64) {
            return "coli::p_float64";
        } else {
            user_error << "Floats other than 32 and 64 bits are not suppored in Coli.\n";
        }
    } else if (type.is_bool()) {
        return "coli::p_boolean";
    } else {
        user_error << "Halide type cannot be translated to Coli type.\n";
    }
    return "coli::p_none";
}

class NormalizeVariableName : public IRMutator {
    using IRMutator::visit;

    void visit(const For *op) {
        string name = print_name(op->name);
        Expr min = mutate(op->min);
        Expr extent = mutate(op->extent);
        Stmt body = mutate(op->body);
        if ((name == op->name) &&
            min.same_as(op->min) &&
            extent.same_as(op->extent) &&
            body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = For::make(name, min, extent, op->for_type, op->device_api, body);
        }
    }

    void visit(const Let *op) {
        string name = print_name(op->name);
        Expr value = mutate(op->value);
        Expr body = mutate(op->body);
        if ((name == op->name) &&
            value.same_as(op->value) &&
            body.same_as(op->body)) {
            expr = op;
        } else {
            expr = Let::make(name, value, body);
        }
    }

    void visit(const LetStmt *op) {
        string name = print_name(op->name);
        Expr value = mutate(op->value);
        Stmt body = mutate(op->body);
        if ((name == op->name) &&
            value.same_as(op->value) &&
            body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = LetStmt::make(name, value, body);
        }
    }

    void visit(const Variable *op) {
        string name = print_name(op->name);
        if (name != op->name) {
            expr = Variable::make(op->type, name, op->image, op->param, op->reduction_domain);
        } else {
            expr = op;
        }
    }
};
}

CodeGen_Coli::CodeGen_Coli(ostream &dest, const string &pipeline_name,
                           const vector<Function> &outputs,
                           const vector<vector<int32_t>> &output_buffer_extents,
                           const vector<Type> &output_buffer_types,
                           const vector<string> &inputs,
                           const vector<vector<int32_t>> &input_buffer_extents,
                           const vector<Type> &input_buffer_types)
        : IRPrinter(dest), func(pipeline_name) {

    internal_assert(outputs.size() == output_buffer_extents.size());
    internal_assert(output_buffer_extents.size() == output_buffer_types.size());
    internal_assert(inputs.size() == input_buffer_extents.size());
    internal_assert(input_buffer_extents.size() == input_buffer_types.size());

    stream << headers << "\n\n";
    stream << "using namespace coli;\n\n";
    stream << "int main(int argc, char **argv)\n";
    stream << "{\n";

    indent += tab_size;

    do_indent();
    stream << "// Set default coli options.\n";
    do_indent();
    stream << "global::set_default_coli_options();\n\n";
    do_indent();
    stream << "coli::function " << func << "(\"" << func << "\")" << ";\n";

    // Allocate the output buffers
    for (size_t k = 0; k < outputs.size(); ++k) {
        const Function &f = outputs[k];
        const vector<int32_t> &buffer_extents = output_buffer_extents[k];
        const Type type = output_buffer_types[k];

        internal_assert(buffer_extents.size() == f.args().size());

        ostringstream sizes;
        sizes << "{";
        for (size_t i = 0; i < buffer_extents.size(); ++i) {
            sizes << "coli::expr(" << buffer_extents[i] << ")";
            string min_str = print_name(f.name() + ".min." + std::to_string(i));
            string extent_str = print_name(f.name() + ".extent." + std::to_string(i));
            scope.push(min_str, make_const(Int(32), 0));
            scope.push(extent_str, make_const(Int(32), buffer_extents[i]));
            if (i != buffer_extents.size() - 1) {
                sizes << ", ";
            }
        }
        sizes << "}";

        string buffer_name = "buff_" + f.name();
        do_indent();
        stream << "coli::buffer " << buffer_name << "(\"" << buffer_name << "\", "
               << f.args().size() << ", " << sizes.str() << ", "
               << halide_type_to_coli_type_str(type) << ", NULL, coli::a_output, "
               << "&" << func << ");\n";
        output_buffers.insert(buffer_name);
    }

    // Bind to the input buffers
    for (size_t k = 0; k < inputs.size(); ++k) {
        const string &input_name = inputs[k];
        const vector<int32_t> &buffer_extents = input_buffer_extents[k];
        const Type type = input_buffer_types[k];

        vector<string> dummy_dims(buffer_extents.size());

        ostringstream sizes;
        sizes << "{";
        for (size_t i = 0; i < buffer_extents.size(); ++i) {
            dummy_dims[i] = "i" + std::to_string(i);
            push_loop_dim(dummy_dims[i], make_const(Int(32), 0), buffer_extents[i]);

            sizes << "coli::expr(" << buffer_extents[i] << ")";
            if (i != buffer_extents.size() - 1) {
                sizes << ", ";
            }
        }
        sizes << "}";

        string buffer_name = "buff_" + input_name;
        do_indent();
        stream << "coli::buffer " << buffer_name << "(\"" << buffer_name << "\", "
               << buffer_extents.size() << ", " << sizes.str() << ", "
               << halide_type_to_coli_type_str(type) << ", NULL, coli::a_input, "
               << "&" << func << ");\n";
        input_buffers.insert(buffer_name);

        // Bind the input buffer to a computation
        string dims_str = to_string(dummy_dims);

        string symbolic_str = get_loop_bound_vars();
        string iter_space_str;
        if (!symbolic_str.empty()) {
            iter_space_str = get_loop_bound_vars() + "->{" + input_name + dims_str + ": " + get_loop_bounds() + "}";
        } else {
            iter_space_str = "{" + input_name + dims_str + ": " + get_loop_bounds() + "}";
        }

        do_indent();
        stream << "coli::computation " << input_name << "(\"" << iter_space_str << "\", "
               << "expr(), false, " << halide_type_to_coli_type_str(type)
               << ", &" << func << ");\n";

        // 1-to-1 mapping to buffer
        string access_str = "{" + input_name + dims_str + "->" + "buff_" + input_name + dims_str + "}";
        do_indent();
        stream << input_name << ".set_access(\"" << access_str << "\");\n";
        stream << "\n";

        computation_list.insert(input_name);

        for (size_t i = 0; i < buffer_extents.size(); ++i) {
            pop_loop_dim();
        }
    }
}

CodeGen_Coli::~CodeGen_Coli() {
    ostringstream buffers_stream;
    buffers_stream << "{";
    int count = 0;
    // Allocate the output buffers
    for (auto it = output_buffers.begin(); it != output_buffers.end(); ++it) {
        count += 1;
        buffers_stream << "&" << (*it);
        if (it != (--output_buffers.end())) {
            buffers_stream << ", ";
        }
    }
    bool start = true;
    // Allocate the input buffers
    for (auto it = input_buffers.begin(); it != input_buffers.end(); ++it) {
        if ((count > 0) && start) {
            start = false;
            buffers_stream << ", ";
        }
        buffers_stream << "&" << (*it);
        if (it != (--input_buffers.end())) {
            buffers_stream << ", ";
        }
    }
    buffers_stream << "}";

    stream << "\n";
    do_indent();
    stream << func << ".set_arguments(" << buffers_stream.str() << ");\n";
    do_indent();
    stream << func << ".gen_isl_ast();\n";
    do_indent();
    stream << func << ".gen_halide_stmt();\n";
    do_indent();
    stream << func << ".dump_halide_stmt();\n";
    do_indent();
    stream << func << ".gen_halide_obj(\"build/generated_" << func << "_test.o\");\n";

    indent -= tab_size;

    do_indent();
    stream << "}\n\n";
}

void CodeGen_Coli::push_loop_dim(const string &name, Expr min, Expr extent) {
    loop_dims.push_back({name, min, extent});
}

void CodeGen_Coli::pop_loop_dim() {
    loop_dims.pop_back();
}

string CodeGen_Coli::get_loop_bound_vars() const {
    vector<Expr> relevant_exprs;
    for (size_t i = 0; i < loop_dims.size(); ++i) {
        if (!is_const(loop_dims[i].min)) {
            relevant_exprs.push_back(loop_dims[i].min);
        }
        if (!is_const(loop_dims[i].extent)) {
            relevant_exprs.push_back(loop_dims[i].extent);
        }
    }
    if (relevant_exprs.empty()) {
        return "";
    }
    return to_string(relevant_exprs);
}

string CodeGen_Coli::get_loop_bounds() const {
    ostringstream ss;
    ss << "(";
    for (size_t i = 0; i < loop_dims.size(); ++i) {
        ss << loop_dims[i].to_string();
        if (i != loop_dims.size() - 1) {
            ss << ") and (";
        }
    }
    ss << ")";
    return ss.str();
}

void CodeGen_Coli::visit(const StringImm *op) {
    user_error << "Conversion of StringImm to COLi is not supported.\n";
}

void CodeGen_Coli::visit(const AssertStmt *op) {
    // Do nothing
    debug(5) << "Conversion of AssertStmt to COLi is not supported.\n";
}

void CodeGen_Coli::visit(const Ramp *op) {
    user_error << "Conversion of Ramp to COLi is not supported.\n";
}

void CodeGen_Coli::visit(const Broadcast *op) {
    user_error << "Conversion of Broadcast to COLi is not supported.\n";
}

void CodeGen_Coli::visit(const IfThenElse *op) {
    debug(5) << "Conversion of IfThenElse to COLi is not supported.\n";
    // Just use the "then" case for now
    print(op->then_case);
}

void CodeGen_Coli::visit(const Free *op) {
    user_error << "Conversion of Free to COLi is not supported.\n";
}

void CodeGen_Coli::visit(const Store *op) {
    user_error << "Should pass the unflatten version of Store to COLi\n.\n";
}

void CodeGen_Coli::visit(const Allocate *op) {
    user_error << "Should pass the unflatten version of Allocate to COLi\n.\n";
}

void CodeGen_Coli::visit(const IntImm *op) {
    stream << "coli::expr(";
    if (op->type.bits() == 8) {
        stream << "(int8_t)";
    } else if (op->type.bits() == 16) {
        stream << "(int16_t)";
    } else if (op->type.bits() == 32) {
        stream << "(int32_t)";
    }
    stream << op->value << ")";
}

void CodeGen_Coli::visit(const UIntImm *op) {
    stream << "coli::expr(";
    if (op->type.bits() == 8) {
        stream << "(uint8_t)";
    } else if (op->type.bits() == 16) {
        stream << "(uint16_t)";
    } else if (op->type.bits() == 32) {
        stream << "(uint32_t)";
    }
    stream << op->value << ")";
}

void CodeGen_Coli::visit(const FloatImm *op) {
    if (op->type.bits() == 32) {
        stream << "coli::expr((float)op->value);";
    } else if (op->type.bits() == 64) {
        stream << "coli::expr(op->value);";
    } else {
        // Only support 32- and 64-bit integer
        user_error << "Conversion of float " << op->type.bits() << "_t to COLi is not currently supported.\n";
    }
}

void CodeGen_Coli::visit(const Cast *op) {
    user_error << "Conversion of Cast to COLi is not currently supported.\n";
}

void CodeGen_Coli::visit(const Variable *op) {
    user_assert(!op->param.defined() && !op->image.defined())
        << "Can only handle conversion of simple variable to COLi for now.\n";

    const auto &iter = constant_list.find(op->name);
    if (iter != constant_list.end()) {
        // It is a reference to variable defined in Let/LetStmt
        //TODO(psuriana): when do we actually generate constant???
        stream << (*iter) << "(0)";
    } else {
        // It is presumably a reference to loop variable
        stream << "coli::idx(\"" << op->name << "\")";
    }
}

void CodeGen_Coli::visit(const Add *op) {
    stream << '(';
    print(op->a);
    stream << " + ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Sub *op) {
    stream << '(';
    print(op->a);
    stream << " - ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Mul *op) {
    stream << '(';
    print(op->a);
    stream << "*";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Div *op) {
    stream << '(';
    print(op->a);
    stream << "/";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Mod *op) {
    stream << '(';
    print(op->a);
    stream << " % ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Min *op) {
    stream << "coli::expr(coli::o_min, ";
    print(op->a);
    stream << ", ";
    print(op->b);
    stream << ")";
}

void CodeGen_Coli::visit(const Max *op) {
    stream << "coli::expr(coli::o_max, ";
    print(op->a);
    stream << ", ";
    print(op->b);
    stream << ")";
}

void CodeGen_Coli::visit(const EQ *op) {
    stream << '(';
    print(op->a);
    stream << " == ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const NE *op) {
    stream << '(';
    print(op->a);
    stream << " != ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const LT *op) {
    stream << '(';
    print(op->a);
    stream << " < ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const LE *op) {
    stream << '(';
    print(op->a);
    stream << " <= ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const GT *op) {
    stream << '(';
    print(op->a);
    stream << " > ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const GE *op) {
    stream << '(';
    print(op->a);
    stream << " >= ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const And *op) {
    stream << '(';
    print(op->a);
    stream << " && ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Or *op) {
    stream << '(';
    print(op->a);
    stream << " || ";
    print(op->b);
    stream << ')';
}

void CodeGen_Coli::visit(const Not *op) {
    stream << '!';
    print(op->a);
}

void CodeGen_Coli::visit(const Select *op) {
    do_indent();
    stream << "coli::expr(coli::o_cond, ";
    print(op->condition);
    stream << ", ";
    print(op->true_value);
    stream << ", ";
    print(op->false_value);
    stream << ")";
}

void CodeGen_Coli::visit(const Let *op) {
    user_error << "Should not have encountered Let expr since we've called substitute_in_all_lets.\n";
}

void CodeGen_Coli::visit(const LetStmt *op) {
    scope.push(op->name, op->value);
    print(op->body);
    scope.pop(op->name);
}

void CodeGen_Coli::visit(const ProducerConsumer *op) {
    user_assert(op->body.as<Block>() == NULL) << "Does not currently handle update.\n";
    internal_assert(!op->is_producer || (computation_list.find(op->name) == computation_list.end()))
        << "Found another computation with the same name.\n";

    vector<Loop> old_loop_dims = loop_dims;
    print(op->body);
    loop_dims = old_loop_dims;
}

void CodeGen_Coli::define_constant(const string &name, Expr val) {
    internal_assert(constant_list.find(name) == constant_list.end())
        << "Redefinition of lets is not supported right now.\n";

    val = simplify(val);

    do_indent();
    stream << "coli::constant " << name << "(\"" << name << "\", ";
    print(val);
    stream << ", " << halide_type_to_coli_type_str(val.type())
           << ", true, NULL, 0, &" << func << ");\n";

    constant_list.insert(name);
}

void CodeGen_Coli::visit(const For *op) {
    push_loop_dim(op->name, op->min, op->extent);

    const Variable *min = op->min.as<Variable>();
    internal_assert(min != NULL) << "Min value of a loop should have been a variable.\n";
    const Variable *extent = op->extent.as<Variable>();
    internal_assert(extent != NULL) << "Extent of a loop should have been a variable.\n";

    Expr min_val = scope.get(min->name);
    Expr extent_val = scope.get(extent->name);

    // Substitute it in all references to some other variables in the min/extent val
    map<string, Expr> replacements;
    typename Scope<Expr>::const_iterator iter;
    for (iter = scope.cbegin(); iter != scope.cend(); ++iter) {
        if ((iter.name() != min->name) || (iter.name() != extent->name)) {
            replacements.emplace(iter.name(), iter.value());
        }
    }

    // Do it twice, to make sure we substitute in all variables properly
    min_val = substitute(replacements, min_val);
    min_val = substitute(replacements, min_val);

    extent_val = substitute(replacements, extent_val);
    extent_val = substitute(replacements, extent_val);

    do_indent();
    stream << "// Define loop bounds for dimension \"" << op->name << "\".\n";
    define_constant(min->name, min_val);
    define_constant(extent->name, extent_val);
    stream << "\n";

    print(op->body);
    pop_loop_dim();
}

void CodeGen_Coli::visit(const Evaluate *op) {
    //TODO(psuriana): do nothing for now
}

void CodeGen_Coli::visit(const Load *op) {
    user_error << "Conversion of Load to COLi is not currently supported.\n";
}

void CodeGen_Coli::visit(const Provide *op) {
    internal_assert(computation_list.find(op->name) == computation_list.end())
        << "Duplicate computation is not currently supported.\n";
    internal_assert(temporary_buffers.count("buff_" + op->name) || output_buffers.count("buff_" + op->name))
        << "The buffer should have been allocated previously.\n";

    for (size_t i = 0; i < op->args.size(); ++i) {
        user_assert(op->args[i].as<Variable>() != NULL)
            << "Expect args of provide to be loop dims for now (doesn't currently handle update).\n";
    }
    user_assert(op->values.size() == 1) << "Expect 1D store (no tuple) in the Provide node for now.\n";

    do_indent();
    stream << "coli::computation " << op->name << "(\"";
    indent += 5*tab_size;

    string dims_str = to_string(op->args);
    string symbolic_str = get_loop_bound_vars();
    if (!symbolic_str.empty()) {
        stream << get_loop_bound_vars() + "->{" << op->name + dims_str << ": \"\n";
    } else {
        stream << "{" << op->name << dims_str + ": \"\n";
    }

    do_indent();
    stream << "\"" << get_loop_bounds() << "}\", \n";
    do_indent();
    print(op->values[0]);
    stream << ", true, " << halide_type_to_coli_type_str(op->values[0].type())
           << ", &" << func << ");\n";
    indent -= 5*tab_size;

    // 1-to-1 mapping to buffer
    string access_str = "{" + op->name + dims_str + "->" + "buff_" + op->name + dims_str + "}";
    do_indent();
    stream << op->name << ".set_access(\"" << access_str << "\");\n";

    computation_list.insert(op->name);
}

void CodeGen_Coli::visit(const Realize *op) {
    // We will ignore the condition on the Realize node for now.

    user_assert(temporary_buffers.find("buff_" + op->name) == temporary_buffers.end())
        << "Duplicate allocation (i.e. duplicate compute) is not currently supported.\n";

    // Assert that the types of all buffer dimensions are the same for now.
    for (size_t i = 1; i < op->types.size(); ++i) {
        user_assert(op->types[i-1] == op->types[i])
            << "Realize node should have the same types for all dimensions for now.\n";
    }

    // Assert that the bounds on the dimensions start from 0 for now.
    for (size_t i = 0; i < op->bounds.size(); ++i) {
        user_assert(is_zero(op->bounds[i].min))
            << "Bound of realize node should start from 0 for now.\n";
    }

    do_indent();

    // Create a temporary buffer

    string buffer_name = "buff_" + op->name;
    do_indent();
    stream << "coli::buffer " << buffer_name << "(\"" << buffer_name << "\", "
           << op->bounds.size() << ", ";

    stream << "{";
    for (size_t i = 0; i < op->bounds.size(); ++i) {
        print(op->bounds[i].extent);
        if (i != op->bounds.size() - 1) {
            stream << ", ";
        }
    }
    stream << "}, ";

    stream << halide_type_to_coli_type_str(op->types[0]) << ", NULL, coli::a_temporary, "
           << "&" << func << ");\n";

    temporary_buffers.insert(buffer_name);

    print(op->body);
}

void CodeGen_Coli::visit(const Call *op) {
    user_assert((op->call_type == Call::CallType::Halide) || (op->call_type == Call::CallType::Image))
        << "Only handle call to halide func or image for now.\n"
        << Expr(op) << "\n"
        << "is pure? " << op->is_pure() << "\n";

    const auto iter = computation_list.find(op->name);
    internal_assert(iter != computation_list.end()) << "Call to computation that does not exist.\n";

    stream << (*iter) << "(";
    for (size_t i = 0; i < op->args.size(); i++) {
        print(op->args[i]);
        if (i < op->args.size() - 1) {
            stream << ", ";
        }
    }
    stream << ")";
}

void CodeGen_Coli::visit(const Block *op) {
    print(op->first);
    if (op->rest.defined()) print(op->rest);
}

void CodeGen_Coli::test() {

    std::cout << "CodeGen_Coli test passed\n";
}

void print_to_coli(Stmt s, ostream &dest, const string &pipeline_name,
                   const vector<Function> &outputs,
                   const vector<vector<int32_t>> &output_buffer_extents,
                   const vector<Type> &output_buffer_types,
                   const vector<string> &inputs,
                   const vector<vector<int32_t>> &input_buffer_extents,
                   const vector<Type> &input_buffer_types) {

    NormalizeVariableName normalize;
    s = normalize.mutate(s);
    debug(0) << "After normalization:\n" << s << "\n\n";

    CodeGen_Coli cg(dest, pipeline_name, outputs, output_buffer_extents,
                    output_buffer_types, inputs, input_buffer_extents,
                    input_buffer_types);
    cg.print(s);
}

}
}