#pragma once

// Console rectangle lists on Vulkan, which has no such primitive.
//
// A rectangle is three corners (v1, v2, v3) of a screen-aligned rectangle and the hardware makes the fourth, for the
// position and for every interpolant, after the vertex stage. A list topology of rectangles (D3D_PRIMITIVE_TOPOLOGY
// 17) and a pipeline whose topology type is 19 are console-only. The fact comes from xvalkyrie (GPL-3.0, commit
// 53af7df77c10, notes in its backlog); no code is taken from it. The corner between the other two (the right angle)
// is not always v1: the title's own full-screen blit (VS_Blit_6e5520d1) gives (-1, 1) (-1, -1) (1, -1), the corner
// at v2, and xcl's notes say the same of a retail title's full-screen pass and list v1, v2 and v3 as possible
// (learned from its docs and tests, GPL-2.0-only, no code taken). Taking v1 there made the parallelogram
// v2 + v3 - v1 = (1, -3), of which only the lower-left half of the screen is on screen.
//
// The fourth vertex depends on what the vertex stage wrote, not on what it read, so the expansion has to sit after
// the vertex stage: a pre-pass over the vertex buffers is wrong for any shader that is not linear in its inputs (and
// the translated vertex shaders fetch by V# themselves, so there is no vertex buffer to expand). The pipeline gets a
// small geometry stage instead, built here from the vertex shader's own SPIR-V: it takes the triangle (v1, v2, v3)
// the list draws, finds the corner between the other two as the one opposite the longest side (from the positions'
// x and y; a degenerate triangle takes v1), makes the fourth corner v4 = the other two's sum minus that corner, and
// emits the rectangle as two triangles with the winding of (v1, v2, v3) that both start at v1, so a flat input
// (taken from a triangle's first vertex) is v1's over the whole rectangle:
//   corner v1: (v1, v2, v4) (v1, v4, v3);  corner v2: (v1, v2, v3) (v1, v3, v4);  corner v3: (v1, v2, v3) (v1, v4, v2). Every Location output of the vertex stage is a vec4 float in the translator's output (rdna2spirv.py),
// and so is the stage's Position; the generated stage reads and writes exactly those.
//
// Not covered, and refused with a reason: a vertex stage that writes ClipDistance, CullDistance, Layer or
// ViewportIndex, or a Location output that is not a vec4 float.
//
// Plain word editing, standard library only.

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace leonida {

// D3D_PRIMITIVE_TOPOLOGY values and the PSO topology type of the console's rectangle and quad lists.
constexpr std::uint32_t topology_rect_list = 17;
constexpr std::uint32_t topology_quad_list = 19;
constexpr std::uint32_t topology_type_rect_list = 19;
constexpr std::array<std::uint32_t, 6> quad_triangle_indices{0, 1, 2, 0, 2, 3};

// The geometry stage for `vertex_words`' outputs; empty, with `why` set, when the vertex stage cannot be expanded.
inline std::vector<std::uint32_t> rect_list_geometry(const std::vector<std::uint32_t>& vertex_words, std::string& why) {
    enum : std::uint32_t {
        op_entry_point = 15, op_type_void = 19, op_type_int = 21, op_type_float = 22,
        op_type_vector = 23, op_type_array = 28, op_type_pointer = 32, op_type_function = 33, op_constant = 43,
        op_type_bool = 20, op_composite_construct = 80, op_composite_extract = 81, op_fmul = 133,
        op_logical_and = 167, op_logical_not = 168, op_select = 169, op_ford_greater_than_equal = 190,
        op_function = 54, op_function_end = 56, op_variable = 59, op_load = 61, op_store = 62, op_access_chain = 65,
        op_decorate = 71, op_fadd = 129, op_fsub = 131, op_emit_vertex = 218, op_end_primitive = 219, op_label = 248, op_return = 253,
        op_capability = 17, op_memory_model = 14, op_execution_mode = 16,
        deco_builtin = 11, deco_location = 30, builtin_position = 0, builtin_point_size = 1,
        storage_input = 1, storage_output = 3
    };
    why.clear();
    if (vertex_words.size() < 6 || vertex_words[0] != 0x07230203u) {
        why = "the vertex shader is not a SPIR-V module";
        return {};
    }
    // What the vertex stage declares: types, Output variables, and the decorations that name them.
    struct Type { std::uint32_t op = 0, component = 0, count = 0, width = 0; };
    std::map<std::uint32_t, Type> types;
    std::map<std::uint32_t, std::uint32_t> variable_type;  // Output variable -> its pointer type
    std::map<std::uint32_t, std::uint32_t> location, builtin;       // variable id -> value
    std::set<std::uint32_t> outputs;
    for (std::size_t i = 5; i < vertex_words.size();) {
        const std::uint32_t op = vertex_words[i] & 0xffffu, n = vertex_words[i] >> 16;
        if (!n || i + n > vertex_words.size()) {
            why = "the vertex shader's SPIR-V is cut short";
            return {};
        }
        const std::uint32_t* w = &vertex_words[i];
        if (op == op_type_float && n >= 3) types[w[1]] = {op, 0, 0, w[2]};
        else if (op == op_type_vector && n >= 4) types[w[1]] = {op, w[2], w[3], 0};
        else if (op == op_type_pointer && n >= 4) types[w[1]] = {op, w[3], w[2], 0};
        else if (op == op_variable && n >= 4 && w[3] == storage_output) outputs.insert(w[2]), variable_type[w[2]] = w[1];
        else if (op == op_decorate && n >= 4 && w[2] == deco_location) location[w[1]] = w[3];
        else if (op == op_decorate && n >= 4 && w[2] == deco_builtin) builtin[w[1]] = w[3];
        i += n;
    }
    std::uint32_t position = 0;
    std::vector<std::uint32_t> params;  // Location values, ascending
    for (std::uint32_t id : outputs) {
        const auto b = builtin.find(id);
        if (b != builtin.end()) {
            if (b->second == builtin_position) position = id;
            else if (b->second != builtin_point_size) {
                why = "the vertex shader writes ClipDistance, CullDistance, Layer or ViewportIndex";
                return {};
            }
            continue;
        }
        const auto l = location.find(id);
        if (l == location.end()) continue;
        const Type& ptr = types[variable_type[id]];
        const Type& vec = types[ptr.component];
        const Type& scalar = types[vec.component];
        // Known limit: every vec4 float output gets the plain a + b - p corner. The translator can export an attribute
        // as the register's raw bits with two packed f16 halves (only the pixel shader knows, through per_vertex in
        // tools/rdna2spirv.py), and nothing on the vertex side tells those apart, so such an output would get a wrong
        // fourth corner. Fixing it needs the pixel shader's words here and a per-channel marker from the translator.
        if (vec.op != op_type_vector || vec.count != 4 || scalar.op != op_type_float || scalar.width != 32) {
            why = "a vertex output is not a vec4 float";
            return {};
        }
        params.push_back(l->second);
    }
    if (!position) {
        why = "the vertex shader writes no Position";
        return {};
    }
    std::sort(params.begin(), params.end());
    params.erase(std::unique(params.begin(), params.end()), params.end());

    // The module: a fixed prelude, then per variable ids, then the body. Ids count up from 1.
    std::uint32_t next = 1;
    const auto id = [&] { return next++; };
    std::vector<std::uint32_t> head, annotations, globals, body;
    const auto emit = [](std::vector<std::uint32_t>& to, std::uint32_t op, std::initializer_list<std::uint32_t> args) {
        to.push_back((std::uint32_t(args.size()) + 1) << 16 | op);
        to.insert(to.end(), args.begin(), args.end());
    };
    const std::uint32_t t_void = id(), t_fn = id(), t_uint = id(), t_float = id(), t_v4 = id(),
                        c3 = id(), t_arr = id(), t_pin_arr = id(), t_pin_v4 = id(),
                        t_pout_v4 = id(), c0 = id(), c1 = id(), c2 = id();
    emit(globals, op_type_void, {t_void});
    emit(globals, op_type_function, {t_fn, t_void});
    emit(globals, op_type_int, {t_uint, 32, 0});
    emit(globals, op_type_float, {t_float, 32});
    emit(globals, op_type_vector, {t_v4, t_float, 4});
    emit(globals, op_constant, {t_uint, c3, 3});
    emit(globals, op_constant, {t_uint, c0, 0});
    emit(globals, op_constant, {t_uint, c1, 1});
    emit(globals, op_constant, {t_uint, c2, 2});
    emit(globals, op_type_array, {t_arr, t_v4, c3});
    emit(globals, op_type_pointer, {t_pin_arr, storage_input, t_arr});
    emit(globals, op_type_pointer, {t_pin_v4, storage_input, t_v4});
    emit(globals, op_type_pointer, {t_pout_v4, storage_output, t_v4});

    struct Var { std::uint32_t in, out; bool is_position; };
    std::vector<Var> vars;
    vars.push_back({id(), id(), true});
    for (std::size_t i = 0; i < params.size(); ++i) vars.push_back({id(), id(), false});
    std::vector<std::uint32_t> io_vars;
    emit(annotations, op_decorate, {vars[0].in, deco_builtin, builtin_position});
    emit(annotations, op_decorate, {vars[0].out, deco_builtin, builtin_position});
    for (std::size_t i = 0; i < params.size(); ++i) {
        emit(annotations, op_decorate, {vars[i + 1].in, deco_location, params[i]});
        emit(annotations, op_decorate, {vars[i + 1].out, deco_location, params[i]});
    }
    for (const Var& v : vars) {
        emit(globals, op_variable, {t_pin_arr, v.in, storage_input});
        emit(globals, op_variable, {t_pout_v4, v.out, storage_output});
        io_vars.push_back(v.in);
        io_vars.push_back(v.out);
    }

    const std::uint32_t main_fn = id(), entry_label = id();
    emit(body, op_function, {t_void, main_fn, 0, t_fn});
    emit(body, op_label, {entry_label});
    // Per variable: the three corners.
    struct Corners { std::uint32_t v[3]; };
    std::vector<Corners> corners;
    const std::uint32_t cs[3] = {c0, c1, c2};
    for (const Var& v : vars) {
        Corners c{};
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t ptr = id();
            c.v[k] = id();
            emit(body, op_access_chain, {t_pin_v4, ptr, v.in, cs[k]});
            emit(body, op_load, {t_v4, c.v[k], ptr});
        }
        corners.push_back(c);
    }
    // The corner between the other two, opposite the longest side: squared lengths from the positions' x and y.
    const std::uint32_t t_bool = id(), t_bv4 = id();
    emit(globals, op_type_bool, {t_bool});
    emit(globals, op_type_vector, {t_bv4, t_bool, 4});
    const auto extract = [&](std::uint32_t vector, std::uint32_t component) {
        const std::uint32_t r = id();
        emit(body, op_composite_extract, {t_float, r, vector, component});
        return r;
    };
    const auto length2 = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t d = id(), xx = id(), yy = id(), sum = id();
        emit(body, op_fsub, {t_v4, d, a, b});
        const std::uint32_t x = extract(d, 0), y = extract(d, 1);
        emit(body, op_fmul, {t_float, xx, x, x});
        emit(body, op_fmul, {t_float, yy, y, y});
        emit(body, op_fadd, {t_float, sum, xx, yy});
        return sum;
    };
    const auto at_least = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t r = id();
        emit(body, op_ford_greater_than_equal, {t_bool, r, a, b});
        return r;
    };
    const auto both = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t r = id();
        emit(body, op_logical_and, {t_bool, r, a, b});
        return r;
    };
    const auto splat = [&](std::uint32_t b) {  // OpSelect on vectors takes a vector condition before SPIR-V 1.4
        const std::uint32_t r = id();
        emit(body, op_composite_construct, {t_bv4, r, b, b, b, b});
        return r;
    };
    const std::uint32_t* p = corners[0].v;  // vars[0] is the Position
    const std::uint32_t l12 = length2(p[0], p[1]), l23 = length2(p[1], p[2]), l31 = length2(p[2], p[0]);
    const std::uint32_t at_v1 = both(at_least(l23, l12), at_least(l23, l31)), not_v1 = id();
    emit(body, op_logical_not, {t_bool, not_v1, at_v1});
    const std::uint32_t at_v2 = both(not_v1, at_least(l31, l12));
    const std::uint32_t c_v1 = splat(at_v1), c_v2 = splat(at_v2);
    const auto pick = [&](std::uint32_t condition, std::uint32_t yes, std::uint32_t no) {
        const std::uint32_t r = id();
        emit(body, op_select, {t_v4, r, condition, yes, no});
        return r;
    };
    // Per variable: the fourth corner, v1 + v2 + v3 - 2 * corner, and the six vertices of the two triangles.
    std::vector<std::array<std::uint32_t, 6>> emitted;
    for (const Corners& c : corners) {
        const std::uint32_t corner = pick(c_v1, c.v[0], pick(c_v2, c.v[1], c.v[2]));
        const std::uint32_t s12 = id(), s123 = id(), less = id(), fourth = id();
        emit(body, op_fadd, {t_v4, s12, c.v[0], c.v[1]});
        emit(body, op_fadd, {t_v4, s123, s12, c.v[2]});
        emit(body, op_fsub, {t_v4, less, s123, corner});
        emit(body, op_fsub, {t_v4, fourth, less, corner});
        emitted.push_back({c.v[0], c.v[1], pick(c_v1, fourth, c.v[2]),
                           c.v[0], pick(c_v2, c.v[2], fourth), pick(c_v1, c.v[2], pick(c_v2, fourth, c.v[1]))});
    }
    for (int triangle = 0; triangle < 2; ++triangle) {
        for (int k = 0; k < 3; ++k) {
            for (std::size_t i = 0; i < vars.size(); ++i) emit(body, op_store, {vars[i].out, emitted[i][triangle * 3 + k]});
            emit(body, op_emit_vertex, {});
        }
        emit(body, op_end_primitive, {});
    }
    emit(body, op_return, {});
    emit(body, op_function_end, {});

    std::vector<std::uint32_t> out{0x07230203u, 0x00010000u, 0, next, 0};
    emit(out, op_capability, {2});  // Geometry
    emit(out, op_memory_model, {0, 1});  // Logical, GLSL450
    std::vector<std::uint32_t> entry{3, main_fn, 0x6e69616du, 0};  // Geometry, %main, "main"
    entry.insert(entry.end(), io_vars.begin(), io_vars.end());
    out.push_back((std::uint32_t(entry.size()) + 1) << 16 | op_entry_point);
    out.insert(out.end(), entry.begin(), entry.end());
    emit(out, op_execution_mode, {main_fn, 22});         // Triangles
    emit(out, op_execution_mode, {main_fn, 0, 1});       // Invocations 1
    emit(out, op_execution_mode, {main_fn, 29});         // OutputTriangleStrip
    emit(out, op_execution_mode, {main_fn, 26, 6});      // OutputVertices 6
    out.insert(out.end(), annotations.begin(), annotations.end());
    out.insert(out.end(), globals.begin(), globals.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Geometry-shader-free rectangle list expansion for Vulkan devices without geometryShader (e.g. Apple Silicon /
// MoltenVK). Rewrites a translated vertex shader in place so each DrawInstanced(3 * R) can be issued as
// vkCmdDraw(6 * R) with VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: the original entry point becomes an internal helper
// %orig_main(vertex_index, base_vertex) writing Private shadow copies of the outputs, and a new entry point calls it
// three times for the rectangle's three input vertices (base + 3*r + 0..2), computes the same fourth corner and
// winding as rect_list_geometry(), and outputs the corner selected by (VertexIndex - BaseVertex) % 6.
//
// Requires a vertex shader with at most 10 per-vertex Location inputs (3x instanced hardware fetch fits inside
// MoltenVK's 31-attribute limit) and the same output restrictions as rect_list_geometry(). When the vertex shader
// declares Location inputs, `input_locations` (if non-null) receives the sorted original Location indices that were
// remapped to `3 * j + {0, 1, 2}` for corners 0..2, and the draw must be issued as `vkCmdDraw(6, rects, first, 0)`
// with each vertex buffer bound 3x at `VK_VERTEX_INPUT_RATE_INSTANCE` with stride `3 * orig_stride` and offsets
// `base + (first + {0, 1, 2}) * orig_stride`. Plain word editing, standard library only.
inline std::vector<std::uint32_t> rect_list_vertex(const std::vector<std::uint32_t>& vertex_words, std::string& why,
                                                   std::vector<std::uint32_t>* input_locations = nullptr) {
    enum : std::uint32_t {
        op_memory_model = 14, op_entry_point = 15, op_execution_mode = 16, op_capability = 17,
        op_type_void = 19, op_type_bool = 20, op_type_int = 21, op_type_float = 22, op_type_vector = 23,
        op_type_pointer = 32, op_type_function = 33, op_constant = 43, op_constant_null = 46,
        op_function = 54, op_function_parameter = 55, op_function_end = 56, op_function_call = 57,
        op_variable = 59, op_load = 61, op_store = 62,
        op_decorate = 71, op_member_decorate = 72, op_decoration_group = 73,
        op_group_decorate = 74, op_group_member_decorate = 75,
        op_composite_construct = 80, op_composite_extract = 81, op_copy_object = 83,
        op_bitcast = 124, op_iadd = 128, op_fadd = 129, op_isub = 130, op_fsub = 131,
        op_imul = 132, op_fmul = 133, op_udiv = 134, op_umod = 137,
        op_logical_and = 167, op_logical_not = 168, op_select = 169, op_iequal = 170,
        op_ford_greater_than_equal = 190, op_label = 248, op_return = 253,
        op_execution_mode_id = 331, op_decorate_id = 332,
        op_decorate_string = 5632, op_member_decorate_string = 5633,
        op_string = 7, op_source_continued = 2, op_source = 3, op_source_extension = 4,
        op_name = 5, op_member_name = 6, op_module_processed = 330,
        op_extension = 10, op_ext_inst_import = 11,
        cap_draw_parameters = 4427,
        deco_builtin = 11, deco_location = 30,
        builtin_position = 0, builtin_point_size = 1, builtin_vertex_index = 42,
        builtin_instance_index = 43, builtin_base_vertex = 4424, builtin_base_instance = 4425,
        storage_input = 1, storage_output = 3, storage_private = 6
    };
    why.clear();
    if (input_locations) input_locations->clear();
    if (vertex_words.size() < 6 || vertex_words[0] != 0x07230203u) {
        why = "the vertex shader is not a SPIR-V module";
        return {};
    }
    struct TypeInfo { std::uint32_t op = 0, a = 0, b = 0, c = 0; };
    std::map<std::uint32_t, TypeInfo> types;
    std::map<std::uint32_t, std::uint32_t> variable_type;
    std::map<std::uint32_t, std::uint32_t> location, builtin;
    std::vector<std::uint32_t> input_vars, output_vars;

    std::uint32_t entry_fn = 0;
    bool has_draw_params = false;
    std::size_t after_caps = 5;
    std::size_t after_decos = 5;
    std::size_t first_func = vertex_words.size();

    std::uint32_t t_void = 0, t_bool = 0, t_int = 0, t_uint = 0, t_float = 0;
    std::uint32_t t_v4 = 0, t_bv4 = 0, t_fn_void = 0, t_pin_int = 0, t_pfn_v4 = 0;

    const auto is_pre_types_op = [](std::uint32_t op) {
        switch (op) {
            case op_capability: case op_extension: case op_ext_inst_import: case op_memory_model:
            case op_entry_point: case op_execution_mode: case op_execution_mode_id:
            case op_string: case op_source_continued: case op_source: case op_source_extension:
            case op_name: case op_member_name: case op_module_processed:
            case op_decorate: case op_member_decorate: case op_decoration_group:
            case op_group_decorate: case op_group_member_decorate: case op_decorate_id:
            case op_decorate_string: case op_member_decorate_string:
                return true;
            default:
                return false;
        }
    };

    bool seen_types_section = false;
    for (std::size_t i = 5; i < vertex_words.size();) {
        const std::uint32_t op = vertex_words[i] & 0xffffu, n = vertex_words[i] >> 16;
        if (!n || i + n > vertex_words.size()) {
            why = "the vertex shader's SPIR-V is cut short";
            return {};
        }
        const std::uint32_t* w = &vertex_words[i];
        if (op == op_capability) {
            if (n >= 2 && w[1] == cap_draw_parameters) has_draw_params = true;
            after_caps = i + n;
        }
        if (op == op_entry_point && n >= 4 && w[1] == 0) {  // Vertex
            if (entry_fn) {
                why = "the vertex shader has more than one Vertex entry point";
                return {};
            }
            entry_fn = w[2];
        }
        if (op == op_function && first_func == vertex_words.size()) first_func = i;
        if (!seen_types_section) {
            if (is_pre_types_op(op)) {
                after_decos = i + n;
            } else {
                seen_types_section = true;
            }
        }
        if (first_func == vertex_words.size()) {
            if (op == op_type_void && n == 2) { t_void = w[1]; types[w[1]] = {op, 0, 0, 0}; }
            else if (op == op_type_bool && n == 2) { t_bool = w[1]; types[w[1]] = {op, 0, 0, 0}; }
            else if (op == op_type_int && n == 4) {
                types[w[1]] = {op, w[2], w[3], 0};
                if (w[2] == 32 && w[3] == 1) t_int = w[1];
                if (w[2] == 32 && w[3] == 0) t_uint = w[1];
            } else if (op == op_type_float && n >= 3) {
                types[w[1]] = {op, w[2], 0, 0};
                if (w[2] == 32) t_float = w[1];
            } else if (op == op_type_vector && n == 4) {
                types[w[1]] = {op, w[2], w[3], 0};
            } else if (op == op_type_pointer && n == 4) {
                types[w[1]] = {op, w[2], w[3], 0};
            } else if (op == op_type_function && n >= 3) {
                types[w[1]] = {op, w[2], n - 3, n >= 5 ? w[3] : 0};
                if (n == 3 && w[2] == t_void) t_fn_void = w[1];
            } else if (op == op_variable && n >= 4) {
                variable_type[w[2]] = w[1];
                if (w[3] == storage_input) input_vars.push_back(w[2]);
                else if (w[3] == storage_output) output_vars.push_back(w[2]);
            } else if (op == op_decorate && n >= 4) {
                if (w[2] == deco_location) location[w[1]] = w[3];
                else if (w[2] == deco_builtin) builtin[w[1]] = w[3];
            }
        }
        i += n;
    }
    if (!entry_fn || first_func >= vertex_words.size()) {
        why = "the vertex shader has no Vertex entry point";
        return {};
    }
    // Resolve composite types that depend on base types parsed above.
    constexpr std::uint32_t storage_function = 7;
    for (const auto& [tid, ti] : types) {
        if (ti.op == op_type_vector && ti.a == t_float && ti.b == 4) t_v4 = tid;
        if (ti.op == op_type_vector && ti.a == t_bool && ti.b == 4) t_bv4 = tid;
        if (ti.op == op_type_pointer && ti.a == storage_input && ti.b == t_int) t_pin_int = tid;
        if (ti.op == op_type_pointer && ti.a == storage_function && ti.b == t_v4) t_pfn_v4 = tid;
    }
    std::uint32_t var_vid = 0, var_base = 0, var_iid = 0, var_binst = 0;
    struct LocInput { std::uint32_t orig_loc, var_id, ptr_type, elem_type; };
    std::vector<LocInput> loc_inputs;
    for (std::uint32_t id : input_vars) {
        if (const auto l = location.find(id); l != location.end()) {
            const std::uint32_t pt = variable_type[id];
            const auto pt_it = types.find(pt);
            if (pt_it == types.end() || pt_it->second.op != op_type_pointer || pt_it->second.a != storage_input) {
                why = "unexpected pointer type on vertex Location input";
                return {};
            }
            loc_inputs.push_back({l->second, id, pt, pt_it->second.b});
            continue;
        }
        const auto b = builtin.find(id);
        if (b != builtin.end()) {
            if (b->second == builtin_vertex_index) var_vid = id;
            else if (b->second == builtin_base_vertex) var_base = id;
            else if (b->second == builtin_instance_index) var_iid = id;
            else if (b->second == builtin_base_instance) var_binst = id;
        }
    }
    if (loc_inputs.size() > 10) {
        why = "too many per-vertex Location inputs for 3-corner expansion";
        return {};
    }
    std::sort(loc_inputs.begin(), loc_inputs.end(),
              [](const LocInput& a, const LocInput& b) { return a.orig_loc < b.orig_loc; });
    std::map<std::uint32_t, std::size_t> loc_in_index;
    for (std::size_t j = 0; j < loc_inputs.size(); ++j) loc_in_index[loc_inputs[j].var_id] = j;
    const bool has_loc_inputs = !loc_inputs.empty();
    std::uint32_t position_var = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loc_outputs;
    for (std::uint32_t id : output_vars) {
        const auto b = builtin.find(id);
        if (b != builtin.end()) {
            if (b->second == builtin_position) position_var = id;
            else if (b->second != builtin_point_size) {
                why = "the vertex shader writes ClipDistance, CullDistance, Layer or ViewportIndex";
                return {};
            }
            continue;
        }
        const auto l = location.find(id);
        if (l == location.end()) continue;
        const TypeInfo& ptr = types[variable_type[id]];
        const TypeInfo& vec = types[ptr.b];
        const TypeInfo& scalar = types[vec.a];
        if (vec.op != op_type_vector || vec.b != 4 || scalar.op != op_type_float || scalar.a != 32) {
            why = "a vertex output is not a vec4 float";
            return {};
        }
        loc_outputs.push_back({l->second, id});
    }
    if (!position_var) {
        why = "the vertex shader writes no Position";
        return {};
    }
    std::sort(loc_outputs.begin(), loc_outputs.end());
    // Ordered output variables: [0] is Position, [1..] are Location outputs in ascending order.
    std::vector<std::uint32_t> out_vars;
    out_vars.push_back(position_var);
    for (const auto& [loc, vid] : loc_outputs) out_vars.push_back(vid);
    std::map<std::uint32_t, std::size_t> out_index;
    for (std::size_t idx = 0; idx < out_vars.size(); ++idx) out_index[out_vars[idx]] = idx;

    // Validate how entry_fn (and any helper functions) use var_vid, var_base, and out_vars.
    // Check only ID operands of instructions that can take a pointer variable (never literal operands such as
    // OpExtInst instruction numbers, OpCompositeExtract indices, or memory-access masks).
    enum : std::uint32_t {
        op_copy_memory = 63, op_copy_memory_sized = 64, op_access_chain = 65,
        op_in_bounds_access_chain = 66, op_ptr_access_chain = 67, op_phi = 245
    };
    const auto is_tracked = [&](std::uint32_t ref) -> const char* {
        if (var_vid && ref == var_vid) return "unsupported use of VertexIndex";
        if (var_base && ref == var_base) return "unsupported use of BaseVertex";
        if (has_loc_inputs && var_iid && ref == var_iid) return "unsupported use of InstanceIndex";
        if (has_loc_inputs && var_binst && ref == var_binst) return "unsupported use of BaseInstance";
        if (loc_in_index.count(ref)) return "unsupported use of vertex Location input";
        if (out_index.count(ref)) return "unsupported use of vertex output variable";
        return nullptr;
    };
    bool in_entry = false, found_entry_def = false;
    for (std::size_t i = first_func; i < vertex_words.size();) {
        const std::uint32_t op = vertex_words[i] & 0xffffu, n = vertex_words[i] >> 16;
        const std::uint32_t* w = &vertex_words[i];
        if (op == op_function && n >= 5) {
            in_entry = (w[2] == entry_fn);
            if (in_entry) found_entry_def = true;
        } else if (op == op_function_end) {
            in_entry = false;
        } else if (op == op_function_parameter && in_entry) {
            why = "the vertex entry point has parameters";
            return {};
        } else if (op == op_load && n >= 4) {
            const std::uint32_t ptr = w[3];
            if ((var_vid && ptr == var_vid) || (var_base && ptr == var_base) ||
                (has_loc_inputs && ((var_iid && ptr == var_iid) || (var_binst && ptr == var_binst)))) {
                if (!(in_entry && (w[1] == t_int || w[1] == t_uint))) {
                    why = "unsupported use of vertex BuiltIn input";
                    return {};
                }
            } else if (const auto it = loc_in_index.find(ptr); it != loc_in_index.end()) {
                if (!(in_entry && w[1] == loc_inputs[it->second].elem_type)) {
                    why = "unsupported use of vertex Location input";
                    return {};
                }
            } else if (out_index.count(ptr) && !in_entry) {
                why = "unsupported use of vertex output variable";
                return {};
            }
        } else if (op == op_store && n >= 3) {
            if ((var_vid && w[1] == var_vid) || (var_base && w[1] == var_base) ||
                (has_loc_inputs && ((var_iid && w[1] == var_iid) || (var_binst && w[1] == var_binst))) ||
                loc_in_index.count(w[1])) {
                why = "unsupported store to vertex input";
                return {};
            }
            if (out_index.count(w[1]) && !in_entry) {
                why = "unsupported use of vertex output variable";
                return {};
            }
        } else if ((op == op_access_chain || op == op_in_bounds_access_chain || op == op_ptr_access_chain) && n >= 4) {
            if (const char* err = is_tracked(w[3])) { why = err; return {}; }
        } else if ((op == op_copy_memory || op == op_copy_memory_sized) && n >= 3) {
            if (const char* err = is_tracked(w[1])) { why = err; return {}; }
            if (const char* err = is_tracked(w[2])) { why = err; return {}; }
        } else if (op == op_copy_object && n >= 4) {
            if (const char* err = is_tracked(w[3])) { why = err; return {}; }
        } else if (op == op_select && n >= 6) {
            if (const char* err = is_tracked(w[4])) { why = err; return {}; }
            if (const char* err = is_tracked(w[5])) { why = err; return {}; }
        } else if (op == op_function_call && n >= 4) {
            for (std::uint32_t k = 4; k < n; ++k) {
                if (const char* err = is_tracked(w[k])) { why = err; return {}; }
            }
        } else if (op == op_phi && n >= 5) {
            for (std::uint32_t k = 3; k < n; k += 2) {
                if (const char* err = is_tracked(w[k])) { why = err; return {}; }
            }
        }
        i += n;
    }
    if (!found_entry_def) {
        why = "the vertex entry point has no function body";
        return {};
    }

    std::uint32_t next = vertex_words[3];
    const auto id = [&] { return next++; };
    const auto emit = [](std::vector<std::uint32_t>& to, std::uint32_t op, std::initializer_list<std::uint32_t> args) {
        to.push_back((std::uint32_t(args.size()) + 1) << 16 | op);
        to.insert(to.end(), args.begin(), args.end());
    };

    std::vector<std::uint32_t> extra_decos, extra_types, extra_vars;
    if (!t_void) { t_void = id(); emit(extra_types, op_type_void, {t_void}); }
    if (!t_bool) { t_bool = id(); emit(extra_types, op_type_bool, {t_bool}); }
    if (!t_int) { t_int = id(); emit(extra_types, op_type_int, {t_int, 32, 1}); }
    if (!t_uint) { t_uint = id(); emit(extra_types, op_type_int, {t_uint, 32, 0}); }
    if (!t_float) { t_float = id(); emit(extra_types, op_type_float, {t_float, 32}); }
    if (!t_v4) { t_v4 = id(); emit(extra_types, op_type_vector, {t_v4, t_float, 4}); }
    if (!t_bv4) { t_bv4 = id(); emit(extra_types, op_type_vector, {t_bv4, t_bool, 4}); }
    if (!t_fn_void) { t_fn_void = id(); emit(extra_types, op_type_function, {t_fn_void, t_void}); }
    if (!t_pin_int) { t_pin_int = id(); emit(extra_types, op_type_pointer, {t_pin_int, storage_input, t_int}); }
    if (!t_pfn_v4) { t_pfn_v4 = id(); emit(extra_types, op_type_pointer, {t_pfn_v4, storage_function, t_v4}); }

    // Function type for the original entry point:
    //   without Location inputs: void(int vid, int base, Function* v4 out0, ...)
    //   with Location inputs:    void(int vid, int base, int iid, int binst, T_in0 in0, ..., Function* v4 out0, ...)
    // Passing distinct Function-scope output pointers per corner avoids Private globals and works around
    // SPIRV-Cross expression forwarding across repeated calls to the same helper.
    const std::uint32_t t_fn_orig = id();
    {
        std::vector<std::uint32_t> fn_args = {t_fn_orig, t_void, t_int, t_int};
        if (has_loc_inputs) {
            fn_args.push_back(t_int);
            fn_args.push_back(t_int);
            for (const LocInput& li : loc_inputs) fn_args.push_back(li.elem_type);
        }
        fn_args.insert(fn_args.end(), out_vars.size(), t_pfn_v4);
        extra_types.push_back((std::uint32_t(fn_args.size()) + 1) << 16 | op_type_function);
        extra_types.insert(extra_types.end(), fn_args.begin(), fn_args.end());
    }

    const std::uint32_t i1 = id(), i2 = id(), u1 = id(), u2 = id(), u3 = id(), u4 = id(), u5 = id(), u6 = id(),
                        null_v4 = id();
    emit(extra_types, op_constant, {t_int, i1, 1});
    emit(extra_types, op_constant, {t_int, i2, 2});
    emit(extra_types, op_constant, {t_uint, u1, 1});
    emit(extra_types, op_constant, {t_uint, u2, 2});
    emit(extra_types, op_constant, {t_uint, u3, 3});
    emit(extra_types, op_constant, {t_uint, u4, 4});
    emit(extra_types, op_constant, {t_uint, u5, 5});
    emit(extra_types, op_constant, {t_uint, u6, 6});
    emit(extra_types, op_constant_null, {t_v4, null_v4});

    std::vector<std::uint32_t> new_ep_inputs;
    if (!var_vid) {
        var_vid = id();
        emit(extra_decos, op_decorate, {var_vid, deco_builtin, builtin_vertex_index});
        emit(extra_vars, op_variable, {t_pin_int, var_vid, storage_input});
        new_ep_inputs.push_back(var_vid);
    }
    if (!var_base) {
        var_base = id();
        emit(extra_decos, op_decorate, {var_base, deco_builtin, builtin_base_vertex});
        emit(extra_vars, op_variable, {t_pin_int, var_base, storage_input});
        new_ep_inputs.push_back(var_base);
    }
    if (has_loc_inputs) {
        if (!var_iid) {
            var_iid = id();
            emit(extra_decos, op_decorate, {var_iid, deco_builtin, builtin_instance_index});
            emit(extra_vars, op_variable, {t_pin_int, var_iid, storage_input});
            new_ep_inputs.push_back(var_iid);
        }
        if (!var_binst) {
            var_binst = id();
            emit(extra_decos, op_decorate, {var_binst, deco_builtin, builtin_base_instance});
            emit(extra_vars, op_variable, {t_pin_int, var_binst, storage_input});
            new_ep_inputs.push_back(var_binst);
        }
    }
    // For each Location input j, corner 0 reuses loc_inputs[j].var_id (redecorated to 3*j+0); corners 1 and 2 get
    // new Input variables at locations 3*j+1 and 3*j+2.
    std::vector<std::uint32_t> in_c1(loc_inputs.size()), in_c2(loc_inputs.size());
    for (std::size_t j = 0; j < loc_inputs.size(); ++j) {
        in_c1[j] = id();
        in_c2[j] = id();
        emit(extra_decos, op_decorate, {in_c1[j], deco_location, std::uint32_t(3 * j + 1)});
        emit(extra_decos, op_decorate, {in_c2[j], deco_location, std::uint32_t(3 * j + 2)});
        emit(extra_vars, op_variable, {loc_inputs[j].ptr_type, in_c1[j], storage_input});
        emit(extra_vars, op_variable, {loc_inputs[j].ptr_type, in_c2[j], storage_input});
        new_ep_inputs.push_back(in_c1[j]);
        new_ep_inputs.push_back(in_c2[j]);
    }

    const std::uint32_t p_vid = id(), p_base = id(), wrapper_fn = id();
    const std::uint32_t p_iid = has_loc_inputs ? id() : 0;
    const std::uint32_t p_binst = has_loc_inputs ? id() : 0;
    std::vector<std::uint32_t> p_ins(loc_inputs.size());
    for (std::size_t j = 0; j < loc_inputs.size(); ++j) p_ins[j] = id();
    std::vector<std::uint32_t> p_outs(out_vars.size());
    for (std::size_t idx = 0; idx < out_vars.size(); ++idx) p_outs[idx] = id();

    // Build wrapper entry point body.
    std::vector<std::uint32_t> wrap;
    const std::uint32_t wrap_entry = id();
    emit(wrap, op_function, {t_void, wrapper_fn, 0, t_fn_void});
    emit(wrap, op_label, {wrap_entry});
    // Function-scope variables must appear at the start of the entry block.
    std::array<std::vector<std::uint32_t>, 3> slots;
    for (int k = 0; k < 3; ++k) {
        slots[k].resize(out_vars.size());
        for (std::size_t idx = 0; idx < out_vars.size(); ++idx) {
            slots[k][idx] = id();
            emit(wrap, op_variable, {t_pfn_v4, slots[k][idx], storage_function, null_v4});
        }
    }
    const std::uint32_t raw_vid = id(), raw_base = id(), local_i = id(), local_u = id(),
                        rect_u = id(), corner_k = id(), rect_3u = id(), rect_3 = id(),
                        vid0 = id(), vid1 = id(), vid2 = id();
    emit(wrap, op_load, {t_int, raw_vid, var_vid});
    emit(wrap, op_load, {t_int, raw_base, var_base});
    emit(wrap, op_isub, {t_int, local_i, raw_vid, raw_base});
    emit(wrap, op_bitcast, {t_uint, local_u, local_i});
    std::uint32_t raw_binst = 0;
    if (has_loc_inputs) {
        // Instanced draw: vkCmdDraw(6, rects, first, 0) -> local_u is corner_k (0..5), InstanceIndex - BaseInstance
        // is rect_u (0..rects-1).
        const std::uint32_t raw_iid = id(), rect_i = id();
        raw_binst = id();
        emit(wrap, op_load, {t_int, raw_iid, var_iid});
        emit(wrap, op_load, {t_int, raw_binst, var_binst});
        emit(wrap, op_copy_object, {t_uint, corner_k, local_u});
        emit(wrap, op_isub, {t_int, rect_i, raw_iid, raw_binst});
        emit(wrap, op_bitcast, {t_uint, rect_u, rect_i});
    } else {
        emit(wrap, op_udiv, {t_uint, rect_u, local_u, u6});
        emit(wrap, op_umod, {t_uint, corner_k, local_u, u6});
    }
    emit(wrap, op_imul, {t_uint, rect_3u, rect_u, u3});
    emit(wrap, op_bitcast, {t_int, rect_3, rect_3u});
    emit(wrap, op_iadd, {t_int, vid0, raw_base, rect_3});
    emit(wrap, op_iadd, {t_int, vid1, vid0, i1});
    emit(wrap, op_iadd, {t_int, vid2, vid0, i2});

    struct Corners { std::uint32_t v[3]; };
    std::vector<Corners> corners(out_vars.size());
    const std::uint32_t vids[3] = {vid0, vid1, vid2};
    for (int k = 0; k < 3; ++k) {
        std::vector<std::uint32_t> loaded_ins(loc_inputs.size());
        for (std::size_t j = 0; j < loc_inputs.size(); ++j) {
            const std::uint32_t src_var = (k == 0) ? loc_inputs[j].var_id : (k == 1 ? in_c1[j] : in_c2[j]);
            loaded_ins[j] = id();
            emit(wrap, op_load, {loc_inputs[j].elem_type, loaded_ins[j], src_var});
        }
        const std::uint32_t call_res = id();
        std::vector<std::uint32_t> call_args = {t_void, call_res, entry_fn, vids[k], raw_base};
        if (has_loc_inputs) {
            call_args.push_back(raw_binst);
            call_args.push_back(raw_binst);
            call_args.insert(call_args.end(), loaded_ins.begin(), loaded_ins.end());
        }
        call_args.insert(call_args.end(), slots[k].begin(), slots[k].end());
        wrap.push_back((std::uint32_t(call_args.size()) + 1) << 16 | op_function_call);
        wrap.insert(wrap.end(), call_args.begin(), call_args.end());
    }
    for (int k = 0; k < 3; ++k) {
        for (std::size_t idx = 0; idx < out_vars.size(); ++idx) {
            corners[idx].v[k] = id();
            emit(wrap, op_load, {t_v4, corners[idx].v[k], slots[k][idx]});
        }
    }

    const auto extract = [&](std::uint32_t vec, std::uint32_t comp) {
        const std::uint32_t r = id();
        emit(wrap, op_composite_extract, {t_float, r, vec, comp});
        return r;
    };
    const auto length2 = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t d = id(), xx = id(), yy = id(), sum = id();
        emit(wrap, op_fsub, {t_v4, d, a, b});
        const std::uint32_t x = extract(d, 0), y = extract(d, 1);
        emit(wrap, op_fmul, {t_float, xx, x, x});
        emit(wrap, op_fmul, {t_float, yy, y, y});
        emit(wrap, op_fadd, {t_float, sum, xx, yy});
        return sum;
    };
    const auto at_least = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t r = id();
        emit(wrap, op_ford_greater_than_equal, {t_bool, r, a, b});
        return r;
    };
    const auto both = [&](std::uint32_t a, std::uint32_t b) {
        const std::uint32_t r = id();
        emit(wrap, op_logical_and, {t_bool, r, a, b});
        return r;
    };
    const auto splat = [&](std::uint32_t b) {
        const std::uint32_t r = id();
        emit(wrap, op_composite_construct, {t_bv4, r, b, b, b, b});
        return r;
    };
    const auto pick = [&](std::uint32_t cond, std::uint32_t yes, std::uint32_t no) {
        const std::uint32_t r = id();
        emit(wrap, op_select, {t_v4, r, cond, yes, no});
        return r;
    };

    const std::uint32_t* p = corners[0].v;  // Position
    const std::uint32_t l12 = length2(p[0], p[1]), l23 = length2(p[1], p[2]), l31 = length2(p[2], p[0]);
    const std::uint32_t at_v1 = both(at_least(l23, l12), at_least(l23, l31)), not_v1 = id();
    emit(wrap, op_logical_not, {t_bool, not_v1, at_v1});
    const std::uint32_t at_v2 = both(not_v1, at_least(l31, l12));
    const std::uint32_t c_v1 = splat(at_v1), c_v2 = splat(at_v2);

    const std::uint32_t ku[5] = {u1, u2, u3, u4, u5};
    std::uint32_t ksel[5]{};
    for (int m = 0; m < 5; ++m) {
        const std::uint32_t eq = id();
        emit(wrap, op_iequal, {t_bool, eq, corner_k, ku[m]});
        ksel[m] = splat(eq);
    }

    for (std::size_t idx = 0; idx < out_vars.size(); ++idx) {
        const Corners& c = corners[idx];
        const std::uint32_t corner = pick(c_v1, c.v[0], pick(c_v2, c.v[1], c.v[2]));
        const std::uint32_t s12 = id(), s123 = id(), less = id(), fourth = id();
        emit(wrap, op_fadd, {t_v4, s12, c.v[0], c.v[1]});
        emit(wrap, op_fadd, {t_v4, s123, s12, c.v[2]});
        emit(wrap, op_fsub, {t_v4, less, s123, corner});
        emit(wrap, op_fsub, {t_v4, fourth, less, corner});
        const std::uint32_t e[6] = {
            c.v[0], c.v[1], pick(c_v1, fourth, c.v[2]),
            c.v[0], pick(c_v2, c.v[2], fourth), pick(c_v1, c.v[2], pick(c_v2, fourth, c.v[1]))
        };
        std::uint32_t chosen = e[0];
        for (int m = 0; m < 5; ++m) chosen = pick(ksel[m], e[m + 1], chosen);
        emit(wrap, op_store, {out_vars[idx], chosen});
    }
    emit(wrap, op_return, {});
    emit(wrap, op_function_end, {});

    // Assemble the rewritten module.
    std::vector<std::uint32_t> out;
    out.reserve(vertex_words.size() + extra_decos.size() + extra_types.size() + extra_vars.size() + wrap.size() + 32);
    out.insert(out.end(), vertex_words.begin(), vertex_words.begin() + 5);

    bool caps_inserted = false, decos_inserted = false, types_inserted = false;
    in_entry = false;
    for (std::size_t i = 5; i < vertex_words.size();) {
        if (!caps_inserted && i == after_caps) {
            if (!has_draw_params) emit(out, op_capability, {cap_draw_parameters});
            caps_inserted = true;
        }
        if (!decos_inserted && i == after_decos) {
            out.insert(out.end(), extra_decos.begin(), extra_decos.end());
            decos_inserted = true;
        }
        if (!types_inserted && i == first_func) {
            out.insert(out.end(), extra_types.begin(), extra_types.end());
            out.insert(out.end(), extra_vars.begin(), extra_vars.end());
            types_inserted = true;
        }
        const std::uint32_t op = vertex_words[i] & 0xffffu, n = vertex_words[i] >> 16;
        const std::uint32_t* w = &vertex_words[i];

        if (op == op_entry_point && n >= 4 && w[1] == 0 && w[2] == entry_fn) {
            std::vector<std::uint32_t> ep(w, w + n);
            ep[2] = wrapper_fn;
            for (std::uint32_t v : new_ep_inputs) ep.push_back(v);
            ep[0] = (std::uint32_t(ep.size()) << 16) | op_entry_point;
            out.insert(out.end(), ep.begin(), ep.end());
        } else if ((op == op_execution_mode || op == op_execution_mode_id) && n >= 3 && w[1] == entry_fn) {
            out.push_back(w[0]);
            out.push_back(wrapper_fn);
            out.insert(out.end(), w + 2, w + n);
        } else if (op == op_decorate && n == 4 && w[2] == deco_location && loc_in_index.count(w[1])) {
            out.push_back(w[0]);
            out.push_back(w[1]);
            out.push_back(w[2]);
            out.push_back(std::uint32_t(3 * loc_in_index[w[1]]));
        } else if (op == op_function && n == 5 && w[2] == entry_fn) {
            in_entry = true;
            out.push_back(w[0]);
            out.push_back(w[1]);
            out.push_back(w[2]);
            out.push_back(w[3]);
            out.push_back(t_fn_orig);
            emit(out, op_function_parameter, {t_int, p_vid});
            emit(out, op_function_parameter, {t_int, p_base});
            if (has_loc_inputs) {
                emit(out, op_function_parameter, {t_int, p_iid});
                emit(out, op_function_parameter, {t_int, p_binst});
                for (std::size_t j = 0; j < loc_inputs.size(); ++j)
                    emit(out, op_function_parameter, {loc_inputs[j].elem_type, p_ins[j]});
            }
            for (std::uint32_t po : p_outs) emit(out, op_function_parameter, {t_pfn_v4, po});
        } else if (op == op_function_end && in_entry) {
            in_entry = false;
            out.insert(out.end(), w, w + n);
        } else if (in_entry && op == op_load && n >= 4 && w[3] == var_vid) {
            emit(out, w[1] == t_int ? op_copy_object : op_bitcast, {w[1], w[2], p_vid});
        } else if (in_entry && op == op_load && n >= 4 && w[3] == var_base) {
            emit(out, w[1] == t_int ? op_copy_object : op_bitcast, {w[1], w[2], p_base});
        } else if (in_entry && has_loc_inputs && op == op_load && n >= 4 && w[3] == var_iid) {
            emit(out, w[1] == t_int ? op_copy_object : op_bitcast, {w[1], w[2], p_iid});
        } else if (in_entry && has_loc_inputs && op == op_load && n >= 4 && w[3] == var_binst) {
            emit(out, w[1] == t_int ? op_copy_object : op_bitcast, {w[1], w[2], p_binst});
        } else if (in_entry && op == op_load && n >= 4 && loc_in_index.count(w[3])) {
            emit(out, op_copy_object, {w[1], w[2], p_ins[loc_in_index[w[3]]]});
        } else if (in_entry && op == op_store && n >= 3 && out_index.count(w[1])) {
            out.push_back(w[0]);
            out.push_back(p_outs[out_index[w[1]]]);
            out.insert(out.end(), w + 2, w + n);
        } else if (in_entry && op == op_load && n >= 4 && out_index.count(w[3])) {
            out.push_back(w[0]);
            out.push_back(w[1]);
            out.push_back(w[2]);
            out.push_back(p_outs[out_index[w[3]]]);
            out.insert(out.end(), w + 4, w + n);
        } else {
            out.insert(out.end(), w, w + n);
        }
        i += n;
    }
    out.insert(out.end(), wrap.begin(), wrap.end());
    out[3] = next;
    if (input_locations) {
        input_locations->reserve(loc_inputs.size());
        for (const LocInput& li : loc_inputs) input_locations->push_back(li.orig_loc);
    }
    return out;
}

}  // namespace leonida
