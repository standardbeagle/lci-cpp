#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <cstring>
#include <string>

namespace lci::parser {

namespace {

bool is_cpp_family_extension(std::string_view ext) {
    return ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
           ext == ".h" || ext == ".hpp";
}

TSNode pick_cpp_reference_leaf(TSNode node) {
    if (ts_node_is_null(node)) return node;

    const char* raw_type = ts_node_type(node);
    std::string_view node_type(raw_type ? raw_type : "");

    if (node_type == "field_expression") {
        TSNode field = ts_node_child_by_field_name(
            node, "field", static_cast<uint32_t>(std::strlen("field")));
        if (!ts_node_is_null(field)) return field;
    }

    if (node_type == "qualified_identifier" || node_type == "scoped_identifier" ||
        node_type == "template_function" || node_type == "template_method") {
        for (const char* field_name : {"name", "field"}) {
            TSNode child = ts_node_child_by_field_name(
                node, field_name,
                static_cast<uint32_t>(std::strlen(field_name)));
            if (!ts_node_is_null(child)) return pick_cpp_reference_leaf(child);
        }
    }

    uint32_t child_count = ts_node_named_child_count(node);
    for (uint32_t i = child_count; i > 0; --i) {
        TSNode child = ts_node_named_child(node, i - 1);
        if (ts_node_is_null(child)) continue;
        std::string_view child_type(ts_node_type(child));
        if (child_type == "identifier" || child_type == "field_identifier" ||
            child_type == "type_identifier" ||
            child_type == "namespace_identifier" ||
            child_type == "destructor_name") {
            return child;
        }
    }

    return node;
}

bool is_cpp_type_declaration_name_context(TSNode node) {
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) return false;

    std::string_view parent_type(ts_node_type(parent));
    return parent_type == "class_specifier" || parent_type == "struct_specifier" ||
           parent_type == "enum_specifier" ||
           parent_type == "namespace_definition";
}

// First named child of the given type, or a null node. Used to recover names
std::string go_bare_type(std::string_view t) {
    size_t i = 0;
    while (i < t.size() &&
           (t[i] == '*' || t[i] == '&' || t[i] == '[' || t[i] == ']'))
        ++i;
    t = t.substr(i);
    if (auto d = t.rfind('.'); d != std::string_view::npos) t = t.substr(d + 1);
    return std::string(t);
}

// Bare JS/TS type from an annotation/type node: strips ": ", generics
// (Foo<Bar> -> Foo), array suffix (Foo[] -> Foo), and qualifier (ns.Foo -> Foo).
std::string js_bare_type(std::string_view t) {
    while (!t.empty() && (t.front() == ':' || t.front() == ' '))
        t.remove_prefix(1);
    if (auto a = t.find('<'); a != std::string_view::npos) t = t.substr(0, a);
    if (auto b = t.find('['); b != std::string_view::npos) t = t.substr(0, b);
    while (!t.empty() && (t.back() == ' ')) t.remove_suffix(1);
    if (auto d = t.rfind('.'); d != std::string_view::npos) t = t.substr(d + 1);
    return std::string(t);
}

// Bare Python type from an annotation: strips quotes (string annotations),
// subscripts (List[Foo] -> List), and module qualifier (mod.Foo -> Foo).
std::string py_bare_type(std::string_view t) {
    while (!t.empty() && (t.front() == '"' || t.front() == '\'' || t.front() == ' '))
        t.remove_prefix(1);
    while (!t.empty() && (t.back() == '"' || t.back() == '\'' || t.back() == ' '))
        t.remove_suffix(1);
    if (auto b = t.find('['); b != std::string_view::npos) t = t.substr(0, b);
    if (auto d = t.rfind('.'); d != std::string_view::npos) t = t.substr(d + 1);
    while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
    return std::string(t);
}

}  // namespace

void UnifiedExtractor::process_reference_node(TSNode node,
                                              std::string_view node_type) {
    if (ext_ == ".go") {
        process_go_reference(node, node_type);
    } else if (ext_ == ".js" || ext_ == ".jsx" || ext_ == ".ts" ||
               ext_ == ".tsx") {
        process_js_reference(node, node_type);
    } else if (ext_ == ".py") {
        process_python_reference(node, node_type);
    } else if (ext_ == ".java") {
        process_java_reference(node, node_type);
    } else if (ext_ == ".cs") {
        process_csharp_reference(node, node_type);
    } else if (ext_ == ".rs") {
        process_rust_reference(node, node_type);
    } else if (ext_ == ".php") {
        process_php_reference(node, node_type);
    } else if (ext_ == ".kt" || ext_ == ".kts") {
        process_kotlin_reference(node, node_type);
    } else if (ext_ == ".rb") {
        process_ruby_reference(node, node_type);
    } else if (ext_ == ".zig") {
        process_zig_reference(node, node_type);
    } else if (is_cpp_family_extension(ext_)) {
        // Local type env (SCIP base case): this -> enclosing class; `T x;` /
        // `T x = ...` declarations. C++ method calls already resolve by bare
        // name (pick_cpp_reference_leaf returns the field), so this only adds
        // the receiver-type qualification that disambiguates same-named methods.
        if (node_type == "function_definition") {
            local_var_types_.clear();
            std::string cls = enclosing_class_name();
            if (!cls.empty()) local_var_types_["this"] = cls;
        } else if (node_type == "declaration") {
            TSNode ty = ts_node_child_by_field_name(node, "type",
                                                    static_cast<uint32_t>(4));
            TSNode dcl = ts_node_child_by_field_name(
                node, "declarator", static_cast<uint32_t>(10));
            if (!ts_node_is_null(ty) && !ts_node_is_null(dcl)) {
                std::string tn = go_bare_type(node_text(ty));
                // Peel wrapping declarators down to the identifier:
                // `A* a = new A()` is init_declarator > pointer_declarator >
                // identifier, `A** a` nests pointer_declarator, etc. The `*`/`&`
                // live on the declarator, not the type, so `tn` stays "A".
                while (!ts_node_is_null(dcl)) {
                    std::string_view dt(ts_node_type(dcl));
                    if (dt == "init_declarator" || dt == "pointer_declarator" ||
                        dt == "reference_declarator" ||
                        dt == "array_declarator") {
                        dcl = ts_node_child_by_field_name(
                            dcl, "declarator", static_cast<uint32_t>(10));
                    } else {
                        break;
                    }
                }
                if (!ts_node_is_null(dcl) && !tn.empty() &&
                    std::string_view(ts_node_type(dcl)) == "identifier")
                    local_var_types_[std::string(node_text(dcl))] = tn;
            }
        }

        if (node_type == "call_expression") {
            TSNode func = ts_node_child_by_field_name(
                node, "function",
                static_cast<uint32_t>(std::strlen("function")));
            if (!ts_node_is_null(func)) {
                Reference cref = create_reference(
                    pick_cpp_reference_leaf(func), ReferenceType::Call,
                    RefStrength::Tight);
                const char* ft = ts_node_type(func);
                if (ft && std::string_view(ft) == "field_expression") {
                    TSNode arg = ts_node_child_by_field_name(
                        func, "argument", static_cast<uint32_t>(8));
                    TSNode fld = ts_node_child_by_field_name(
                        func, "field", static_cast<uint32_t>(5));
                    if (!ts_node_is_null(arg) && !ts_node_is_null(fld)) {
                        auto lv = local_var_types_.find(
                            std::string(node_text(arg)));
                        if (lv != local_var_types_.end() && !lv->second.empty())
                            cref.referenced_name =
                                lv->second + "." + std::string(node_text(fld));
                    }
                }
                references_.push_back(std::move(cref));
            }
        } else if ((node_type == "type_identifier" ||
                    node_type == "qualified_identifier" ||
                    node_type == "scoped_identifier") &&
                   !is_cpp_type_declaration_name_context(node)) {
            references_.push_back(create_reference(
                pick_cpp_reference_leaf(node), ReferenceType::Usage,
                RefStrength::Loose));
        }
    }
}

std::string UnifiedExtractor::enclosing_class_name() const {
    for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
        if (it->scope_type == ScopeType::Class) return it->name;
    }
    return {};
}

// Seed the per-function local type env from the receiver (methods) and typed
// parameters. Cleared each function/method so types don't leak across funcs.
void UnifiedExtractor::seed_go_local_types(TSNode fn, bool is_method) {
    local_var_types_.clear();
    auto add_plist = [&](TSNode plist) {
        if (ts_node_is_null(plist)) return;
        uint32_t n = ts_node_named_child_count(plist);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode pd = ts_node_named_child(plist, i);
            const char* pt = ts_node_type(pd);
            if (!pt || std::string_view(pt) != "parameter_declaration") continue;
            TSNode ty = ts_node_child_by_field_name(pd, "type", 4);
            if (ts_node_is_null(ty)) continue;
            std::string tn = go_bare_type(node_text(ty));
            if (tn.empty()) continue;
            uint32_t cc = ts_node_named_child_count(pd);
            for (uint32_t j = 0; j < cc; ++j) {
                TSNode c = ts_node_named_child(pd, j);
                const char* ct = ts_node_type(c);
                if (ct && std::string_view(ct) == "identifier")
                    local_var_types_[std::string(node_text(c))] = tn;
            }
        }
    };
    if (is_method)
        add_plist(ts_node_child_by_field_name(fn, "receiver",
                                              static_cast<uint32_t>(8)));
    add_plist(ts_node_child_by_field_name(fn, "parameters",
                                          static_cast<uint32_t>(10)));
}

// Record `var x T` and `x := T{}` / `x := &T{}` into the local type env.
void UnifiedExtractor::record_go_local_var(TSNode decl) {
    const char* dt = ts_node_type(decl);
    std::string_view t(dt ? dt : "");
    if (t == "var_declaration") {
        uint32_t n = ts_node_named_child_count(decl);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode spec = ts_node_named_child(decl, i);
            TSNode ty = ts_node_child_by_field_name(spec, "type",
                                                    static_cast<uint32_t>(4));
            if (ts_node_is_null(ty)) continue;
            std::string tn = go_bare_type(node_text(ty));
            if (tn.empty()) continue;
            uint32_t cc = ts_node_named_child_count(spec);
            for (uint32_t j = 0; j < cc; ++j) {
                TSNode c = ts_node_named_child(spec, j);
                const char* ct = ts_node_type(c);
                if (ct && std::string_view(ct) == "identifier")
                    local_var_types_[std::string(node_text(c))] = tn;
            }
        }
    } else if (t == "short_var_declaration") {
        TSNode left = ts_node_child_by_field_name(decl, "left",
                                                  static_cast<uint32_t>(4));
        TSNode right = ts_node_child_by_field_name(decl, "right",
                                                   static_cast<uint32_t>(5));
        if (ts_node_is_null(left) || ts_node_is_null(right)) return;
        if (ts_node_named_child_count(left) != 1 ||
            ts_node_named_child_count(right) != 1)
            return;  // base case: single binding
        TSNode lid = ts_node_named_child(left, 0);
        TSNode rex = ts_node_named_child(right, 0);
        const char* lt = ts_node_type(lid);
        if (!lt || std::string_view(lt) != "identifier") return;
        const char* rt = ts_node_type(rex);
        std::string_view r(rt ? rt : "");
        if (r == "unary_expression") {  // &T{}
            TSNode op = ts_node_named_child(rex, 0);
            if (!ts_node_is_null(op)) {
                rex = op;
                rt = ts_node_type(rex);
                r = rt ? rt : "";
            }
        }
        if (r == "composite_literal") {  // T{...}
            TSNode ty = ts_node_child_by_field_name(rex, "type",
                                                    static_cast<uint32_t>(4));
            if (!ts_node_is_null(ty)) {
                std::string tn = go_bare_type(node_text(ty));
                if (!tn.empty())
                    local_var_types_[std::string(node_text(lid))] = tn;
            }
        }
    }
}

void UnifiedExtractor::process_go_reference(TSNode node,
                                            std::string_view node_type) {
    auto is_handled = [&](TSNode n) {
        uintptr_t id = reinterpret_cast<uintptr_t>(n.id);
        for (const auto& h : handled_nodes_) {
            if (h.id == id) return true;
        }
        return false;
    };

    // Maintain the local type env (SCIP base case). func_literal (closures)
    // deliberately does NOT clear — it inherits the enclosing function's types.
    if (node_type == "function_declaration") {
        seed_go_local_types(node, false);
        return;
    }
    if (node_type == "method_declaration") {
        seed_go_local_types(node, true);
        return;
    }
    if (node_type == "short_var_declaration" ||
        node_type == "var_declaration") {
        record_go_local_var(node);
        return;
    }

    if (node_type == "call_expression") {
        TSNode func = ts_node_child_by_field_name(
            node, "function",
            static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) return;

        // For a method / qualified call `x.M(...)` (or `pkg.F(...)`) the call
        // target is the method name M, not the whole `x.M` selector — and
        // create_reference names a ref by node text, so a Call ref on the
        // selector is named "x.M" and never resolves to the symbol M. Tag the
        // FIELD (M) as the Call so referenced_name == "M" resolves to the
        // method/function symbol and the call site shows up as a CALLER. Mark
        // the field handled so the selector_expression / field_identifier
        // branches below don't also emit a Usage ref for it (double-count, and
        // — the original bug — the only ref that resolved to M was that Usage,
        // which get_caller_names filters out, so methods had zero callers).
        const char* ftype = ts_node_type(func);
        if (ftype && std::string_view(ftype) == "selector_expression") {
            TSNode field = ts_node_child_by_field_name(
                func, "field", static_cast<uint32_t>(std::strlen("field")));
            if (!ts_node_is_null(field)) {
                handled_nodes_.push_back(
                    {reinterpret_cast<uintptr_t>(field.id)});
                Reference cref =
                    create_reference(field, ReferenceType::Call,
                                     RefStrength::Tight);
                // Receiver-type qualification (SCIP base case): if the receiver
                // is a local identifier whose type we know, emit "Type.M" so the
                // resolver selects the exact method among same-named candidates.
                TSNode operand = ts_node_child_by_field_name(
                    func, "operand", static_cast<uint32_t>(7));
                if (!ts_node_is_null(operand)) {
                    const char* ot = ts_node_type(operand);
                    if (ot && std::string_view(ot) == "identifier") {
                        auto it = local_var_types_.find(
                            std::string(node_text(operand)));
                        if (it != local_var_types_.end() &&
                            !it->second.empty()) {
                            cref.referenced_name =
                                it->second + "." +
                                std::string(node_text(field));
                        }
                    }
                }
                references_.push_back(std::move(cref));
                return;
            }
        }
        references_.push_back(
            create_reference(func, ReferenceType::Call, RefStrength::Tight));
    } else if (node_type == "selector_expression") {
        TSNode field = ts_node_child_by_field_name(
            node, "field", static_cast<uint32_t>(std::strlen("field")));
        if (!ts_node_is_null(field) && !is_handled(field)) {
            references_.push_back(
                create_reference(field, ReferenceType::Usage, RefStrength::Loose));
        }
    } else if (node_type == "type_identifier" ||
               node_type == "field_identifier") {
        if (!is_handled(node)) {
            references_.push_back(
                create_reference(node, ReferenceType::Usage, RefStrength::Loose));
        }
    }
}

void UnifiedExtractor::process_js_reference(TSNode node,
                                            std::string_view node_type) {
    uintptr_t node_id = reinterpret_cast<uintptr_t>(node.id);
    auto is_handled = [&](TSNode n) {
        uintptr_t id = reinterpret_cast<uintptr_t>(n.id);
        for (const auto& h : handled_nodes_) {
            if (h.id == id) return true;
        }
        return false;
    };

    // Local type env (SCIP base case). this -> enclosing class; TS-annotated
    // params/vars (`x: T`, `(x: T)`) and `new T()` constructions.
    if (node_type == "method_definition" ||
        node_type == "function_declaration" ||
        node_type == "function_expression" || node_type == "arrow_function" ||
        node_type == "generator_function_declaration") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["this"] = cls;
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                TSNode pat = ts_node_child_by_field_name(
                    p, "pattern", static_cast<uint32_t>(7));
                if (!ts_node_is_null(ty) && !ts_node_is_null(pat)) {
                    const char* pt = ts_node_type(pat);
                    if (pt && std::string_view(pt) == "identifier") {
                        std::string tn = js_bare_type(node_text(ty));
                        if (!tn.empty())
                            local_var_types_[std::string(node_text(pat))] = tn;
                    }
                }
            }
        }
        // fall through (arrow/function bodies still get their refs walked)
    } else if (node_type == "variable_declarator") {
        TSNode nm = ts_node_child_by_field_name(node, "name",
                                                static_cast<uint32_t>(4));
        if (!ts_node_is_null(nm)) {
            const char* nt = ts_node_type(nm);
            if (nt && std::string_view(nt) == "identifier") {
                std::string ty;
                TSNode tann = ts_node_child_by_field_name(
                    node, "type", static_cast<uint32_t>(4));
                if (!ts_node_is_null(tann)) {
                    ty = js_bare_type(node_text(tann));
                } else {
                    TSNode val = ts_node_child_by_field_name(
                        node, "value", static_cast<uint32_t>(5));
                    if (!ts_node_is_null(val)) {
                        const char* vt = ts_node_type(val);
                        if (vt && std::string_view(vt) == "new_expression") {
                            TSNode ctor = ts_node_child_by_field_name(
                                val, "constructor", static_cast<uint32_t>(11));
                            if (!ts_node_is_null(ctor))
                                ty = js_bare_type(node_text(ctor));
                        }
                    }
                }
                if (!ty.empty())
                    local_var_types_[std::string(node_text(nm))] = ty;
            }
        }
    }

    if (node_type == "call_expression") {
        TSNode func = ts_node_child_by_field_name(
            node, "function",
            static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) return;
        // Method call obj.M(...): tag the PROPERTY (method name) as the Call so
        // it resolves to the method symbol (not the un-resolvable "obj.M"); the
        // member_expression branch then skips it. Qualify "Type.M" when obj's
        // type is known (this -> class, typed local).
        const char* ftype = ts_node_type(func);
        if (ftype && std::string_view(ftype) == "member_expression") {
            TSNode prop = ts_node_child_by_field_name(
                func, "property", static_cast<uint32_t>(std::strlen("property")));
            if (!ts_node_is_null(prop)) {
                handled_nodes_.push_back({reinterpret_cast<uintptr_t>(prop.id)});
                Reference cref = create_reference(prop, ReferenceType::Call,
                                                  RefStrength::Tight);
                TSNode obj = ts_node_child_by_field_name(
                    func, "object", static_cast<uint32_t>(6));
                if (!ts_node_is_null(obj)) {
                    auto it = local_var_types_.find(
                        std::string(node_text(obj)));  // "this" or ident
                    if (it != local_var_types_.end() && !it->second.empty())
                        cref.referenced_name =
                            it->second + "." + std::string(node_text(prop));
                }
                references_.push_back(std::move(cref));
                return;
            }
        }
        handled_nodes_.push_back({reinterpret_cast<uintptr_t>(func.id)});
        references_.push_back(
            create_reference(func, ReferenceType::Call, RefStrength::Tight));
    } else if (node_type == "member_expression") {
        TSNode prop = ts_node_child_by_field_name(
            node, "property",
            static_cast<uint32_t>(std::strlen("property")));
        if (!ts_node_is_null(prop) && !is_handled(prop)) {
            handled_nodes_.push_back(
                {reinterpret_cast<uintptr_t>(prop.id)});
            references_.push_back(
                create_reference(prop, ReferenceType::Usage, RefStrength::Loose));
        }
    } else if (node_type == "import_statement") {
        TSNode source = ts_node_child_by_field_name(
            node, "source", static_cast<uint32_t>(std::strlen("source")));
        if (!ts_node_is_null(source)) {
            references_.push_back(
                create_reference(source, ReferenceType::Import,
                                 RefStrength::Tight));
        }
    } else if (node_type == "identifier") {
        // Skip if already handled or in import context
        for (const auto& h : handled_nodes_) {
            if (h.id == node_id) return;
        }
        if (in_import_context_) return;
        references_.push_back(
            create_reference(node, ReferenceType::Usage, RefStrength::Loose));
    }
}

void UnifiedExtractor::process_python_reference(TSNode node,
                                                std::string_view node_type) {
    auto is_handled = [&](TSNode n) {
        uintptr_t id = reinterpret_cast<uintptr_t>(n.id);
        for (const auto& h : handled_nodes_) {
            if (h.id == id) return true;
        }
        return false;
    };

    // Local type env (SCIP base case). Python uses only UNAMBIGUOUS type
    // sources: self/cls -> enclosing class, and annotated params/vars
    // (`x: T`). `x = Foo()` is intentionally skipped — constructor vs factory
    // call is syntactically identical in Python, so it would mis-type.
    if (node_type == "function_definition") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) {
            local_var_types_["self"] = cls;
            local_var_types_["cls"] = cls;
        }
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                const char* pt = ts_node_type(p);
                if (!pt || std::string_view(pt) != "typed_parameter") continue;
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                TSNode nm = ts_node_named_child(p, 0);
                if (!ts_node_is_null(ty) && !ts_node_is_null(nm)) {
                    std::string tn = py_bare_type(node_text(ty));
                    if (!tn.empty())
                        local_var_types_[std::string(node_text(nm))] = tn;
                }
            }
        }
        return;
    }
    if (node_type == "assignment") {
        TSNode ty = ts_node_child_by_field_name(node, "type",
                                                static_cast<uint32_t>(4));
        TSNode lhs = ts_node_child_by_field_name(node, "left",
                                                 static_cast<uint32_t>(4));
        if (!ts_node_is_null(ty) && !ts_node_is_null(lhs)) {
            const char* lt = ts_node_type(lhs);
            if (lt && std::string_view(lt) == "identifier") {
                std::string tn = py_bare_type(node_text(ty));
                if (!tn.empty())
                    local_var_types_[std::string(node_text(lhs))] = tn;
            }
        }
        return;
    }

    if (node_type == "call") {
        TSNode func = ts_node_child_by_field_name(
            node, "function",
            static_cast<uint32_t>(std::strlen("function")));
        if (ts_node_is_null(func)) return;
        // Method call obj.M(...): tag the attribute (method name) as the Call
        // (was a Call on the un-resolvable "obj.M" + a Usage on "M", so methods
        // had no callers). Qualify "Type.M" when obj's type is known.
        const char* ftype = ts_node_type(func);
        if (ftype && std::string_view(ftype) == "attribute") {
            TSNode attr = ts_node_child_by_field_name(
                func, "attribute", static_cast<uint32_t>(std::strlen("attribute")));
            if (!ts_node_is_null(attr)) {
                handled_nodes_.push_back({reinterpret_cast<uintptr_t>(attr.id)});
                Reference cref = create_reference(attr, ReferenceType::Call,
                                                  RefStrength::Tight);
                TSNode obj = ts_node_child_by_field_name(
                    func, "object", static_cast<uint32_t>(6));
                if (!ts_node_is_null(obj)) {
                    const char* ot = ts_node_type(obj);
                    if (ot && std::string_view(ot) == "identifier") {
                        auto it = local_var_types_.find(
                            std::string(node_text(obj)));
                        if (it != local_var_types_.end() && !it->second.empty())
                            cref.referenced_name =
                                it->second + "." + std::string(node_text(attr));
                    }
                }
                references_.push_back(std::move(cref));
                return;
            }
        }
        references_.push_back(
            create_reference(func, ReferenceType::Call, RefStrength::Tight));
    } else if (node_type == "attribute") {
        TSNode attr = ts_node_child_by_field_name(
            node, "attribute",
            static_cast<uint32_t>(std::strlen("attribute")));
        if (!ts_node_is_null(attr) && !is_handled(attr)) {
            references_.push_back(
                create_reference(attr, ReferenceType::Usage, RefStrength::Loose));
        }
    } else if (node_type == "identifier") {
        if (!is_handled(node)) {
            references_.push_back(
                create_reference(node, ReferenceType::Usage, RefStrength::Loose));
        }
    }
}

// Shared by the class-based-language handlers below: emit a Call ref on the
// method-name node, qualified to "Type.M" when the receiver's type is known
// (receiver text resolved through local_var_types_, e.g. `this`/`self` or a
// typed local). Unknown receivers degrade to the bare method name.
namespace {
void qualify_and_push(std::vector<Reference>& out, Reference cref,
                      const absl::flat_hash_map<std::string, std::string>& env,
                      std::string_view recv_text, std::string_view method_text) {
    auto it = env.find(std::string(recv_text));
    if (it != env.end() && !it->second.empty())
        cref.referenced_name =
            it->second + "." + std::string(method_text);
    out.push_back(std::move(cref));
}
}  // namespace

void UnifiedExtractor::process_java_reference(TSNode node,
                                              std::string_view node_type) {
    // Local type env: this -> enclosing class; typed params; `T x` /
    // `T x = new T()` locals (the declared type is authoritative either way).
    if (node_type == "method_declaration" ||
        node_type == "constructor_declaration") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["this"] = cls;
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                TSNode nm = ts_node_child_by_field_name(
                    p, "name", static_cast<uint32_t>(4));
                if (!ts_node_is_null(ty) && !ts_node_is_null(nm)) {
                    std::string tn = js_bare_type(node_text(ty));
                    if (!tn.empty())
                        local_var_types_[std::string(node_text(nm))] = tn;
                }
            }
        }
        return;
    }
    if (node_type == "local_variable_declaration") {
        TSNode ty = ts_node_child_by_field_name(node, "type",
                                                static_cast<uint32_t>(4));
        TSNode dcl = ts_node_child_by_field_name(node, "declarator",
                                                 static_cast<uint32_t>(10));
        if (!ts_node_is_null(ty) && !ts_node_is_null(dcl)) {
            TSNode nm = ts_node_child_by_field_name(dcl, "name",
                                                    static_cast<uint32_t>(4));
            if (!ts_node_is_null(nm)) {
                std::string tn = js_bare_type(node_text(ty));
                if (!tn.empty())
                    local_var_types_[std::string(node_text(nm))] = tn;
            }
        }
        return;
    }

    if (node_type == "method_invocation") {
        TSNode name = ts_node_child_by_field_name(node, "name",
                                                  static_cast<uint32_t>(4));
        if (ts_node_is_null(name)) return;
        Reference cref =
            create_reference(name, ReferenceType::Call, RefStrength::Tight);
        TSNode obj = ts_node_child_by_field_name(node, "object",
                                                 static_cast<uint32_t>(6));
        std::string_view recv = ts_node_is_null(obj) ? std::string_view("this")
                                                      : node_text(obj);
        qualify_and_push(references_, std::move(cref), local_var_types_, recv,
                         node_text(name));
    }
}

void UnifiedExtractor::process_csharp_reference(TSNode node,
                                                std::string_view node_type) {
    if (node_type == "method_declaration" ||
        node_type == "constructor_declaration") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["this"] = cls;
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                TSNode nm = ts_node_child_by_field_name(
                    p, "name", static_cast<uint32_t>(4));
                if (!ts_node_is_null(ty) && !ts_node_is_null(nm)) {
                    std::string tn = js_bare_type(node_text(ty));
                    if (!tn.empty())
                        local_var_types_[std::string(node_text(nm))] = tn;
                }
            }
        }
        return;
    }
    if (node_type == "variable_declaration") {
        // `T x = ...;` — the declared type is authoritative; for `var x = new
        // T()` fall to the object_creation type on the declarator value.
        TSNode ty = ts_node_child_by_field_name(node, "type",
                                                static_cast<uint32_t>(4));
        if (ts_node_is_null(ty)) return;
        std::string tn = js_bare_type(node_text(ty));
        bool is_var = (node_text(ty) == "var");
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode d = ts_node_named_child(node, i);
            if (std::string_view(ts_node_type(d)) != "variable_declarator")
                continue;
            TSNode nm = ts_node_child_by_field_name(d, "name",
                                                    static_cast<uint32_t>(4));
            if (ts_node_is_null(nm)) nm = ts_node_named_child(d, 0);
            if (ts_node_is_null(nm)) continue;
            std::string vt = tn;
            if (is_var) {
                vt.clear();
                uint32_t dc = ts_node_named_child_count(d);
                for (uint32_t k = 0; k < dc; ++k) {
                    TSNode v = ts_node_named_child(d, k);
                    if (std::string_view(ts_node_type(v)) ==
                        "object_creation_expression") {
                        TSNode ot = ts_node_child_by_field_name(
                            v, "type", static_cast<uint32_t>(4));
                        if (!ts_node_is_null(ot)) vt = js_bare_type(node_text(ot));
                    }
                }
            }
            if (!vt.empty())
                local_var_types_[std::string(node_text(nm))] = vt;
        }
        return;
    }

    if (node_type == "invocation_expression") {
        TSNode func = ts_node_child_by_field_name(node, "function",
                                                  static_cast<uint32_t>(8));
        if (ts_node_is_null(func)) return;
        if (std::string_view(ts_node_type(func)) == "member_access_expression") {
            TSNode nm = ts_node_child_by_field_name(func, "name",
                                                    static_cast<uint32_t>(4));
            TSNode ex = ts_node_child_by_field_name(func, "expression",
                                                    static_cast<uint32_t>(10));
            if (ts_node_is_null(nm)) return;
            Reference cref =
                create_reference(nm, ReferenceType::Call, RefStrength::Tight);
            std::string_view recv =
                ts_node_is_null(ex) ? std::string_view("this") : node_text(ex);
            qualify_and_push(references_, std::move(cref), local_var_types_, recv,
                             node_text(nm));
        } else if (std::string_view(ts_node_type(func)) == "identifier") {
            qualify_and_push(
                references_,
                create_reference(func, ReferenceType::Call, RefStrength::Tight),
                local_var_types_, "this", node_text(func));
        }
    }
}

void UnifiedExtractor::process_rust_reference(TSNode node,
                                              std::string_view node_type) {
    // self -> impl type (the enclosing impl_item opens a Class scope named
    // after its type); typed params; `let x: T` / `let x = T::new()`.
    if (node_type == "function_item") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["self"] = cls;
        TSNode params = ts_node_child_by_field_name(
            node, "parameters", static_cast<uint32_t>(10));
        if (!ts_node_is_null(params)) {
            uint32_t n = ts_node_named_child_count(params);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode p = ts_node_named_child(params, i);
                if (std::string_view(ts_node_type(p)) != "parameter") continue;
                TSNode pat = ts_node_child_by_field_name(
                    p, "pattern", static_cast<uint32_t>(7));
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                if (!ts_node_is_null(pat) && !ts_node_is_null(ty)) {
                    std::string tn = go_bare_type(node_text(ty));
                    if (!tn.empty())
                        local_var_types_[std::string(node_text(pat))] = tn;
                }
            }
        }
        return;
    }
    if (node_type == "let_declaration") {
        TSNode pat = ts_node_child_by_field_name(node, "pattern",
                                                 static_cast<uint32_t>(7));
        if (ts_node_is_null(pat) ||
            std::string_view(ts_node_type(pat)) != "identifier")
            return;
        std::string tn;
        TSNode ty = ts_node_child_by_field_name(node, "type",
                                                static_cast<uint32_t>(4));
        if (!ts_node_is_null(ty)) {
            tn = go_bare_type(node_text(ty));
        } else {
            TSNode val = ts_node_child_by_field_name(node, "value",
                                                     static_cast<uint32_t>(5));
            if (!ts_node_is_null(val)) {
                std::string_view vt(ts_node_type(val));
                if (vt == "struct_expression") {
                    TSNode n = ts_node_named_child(val, 0);
                    if (!ts_node_is_null(n)) tn = go_bare_type(node_text(n));
                } else if (vt == "call_expression") {
                    // `T::new(...)` / `T::default()` — type is the path prefix.
                    TSNode f = ts_node_child_by_field_name(
                        val, "function", static_cast<uint32_t>(8));
                    if (!ts_node_is_null(f) &&
                        std::string_view(ts_node_type(f)) == "scoped_identifier") {
                        TSNode path = ts_node_child_by_field_name(
                            f, "path", static_cast<uint32_t>(4));
                        if (!ts_node_is_null(path))
                            tn = go_bare_type(node_text(path));
                    }
                } else if (vt == "identifier") {
                    // unit struct: `let a = A;`
                    tn = go_bare_type(node_text(val));
                }
            }
        }
        if (!tn.empty())
            local_var_types_[std::string(node_text(pat))] = tn;
        return;
    }

    if (node_type == "call_expression") {
        TSNode func = ts_node_child_by_field_name(node, "function",
                                                  static_cast<uint32_t>(8));
        if (ts_node_is_null(func)) return;
        if (std::string_view(ts_node_type(func)) == "field_expression") {
            TSNode fld = ts_node_child_by_field_name(func, "field",
                                                     static_cast<uint32_t>(5));
            TSNode val = ts_node_child_by_field_name(func, "value",
                                                     static_cast<uint32_t>(5));
            if (ts_node_is_null(fld)) return;
            Reference cref =
                create_reference(fld, ReferenceType::Call, RefStrength::Tight);
            std::string_view recv =
                ts_node_is_null(val) ? std::string_view() : node_text(val);
            qualify_and_push(references_, std::move(cref), local_var_types_, recv,
                             node_text(fld));
        } else if (std::string_view(ts_node_type(func)) == "identifier") {
            references_.push_back(
                create_reference(func, ReferenceType::Call, RefStrength::Tight));
        }
    }
}

void UnifiedExtractor::process_php_reference(TSNode node,
                                             std::string_view node_type) {
    // $this -> enclosing class; `$x = new T()` locals (keys keep the `$`).
    if (node_type == "method_declaration" ||
        node_type == "function_definition") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["$this"] = cls;
        return;
    }
    if (node_type == "assignment_expression") {
        TSNode lhs = ts_node_child_by_field_name(node, "left",
                                                 static_cast<uint32_t>(4));
        TSNode rhs = ts_node_child_by_field_name(node, "right",
                                                 static_cast<uint32_t>(5));
        if (!ts_node_is_null(lhs) && !ts_node_is_null(rhs) &&
            std::string_view(ts_node_type(lhs)) == "variable_name" &&
            std::string_view(ts_node_type(rhs)) == "object_creation_expression") {
            // `new T(...)`: the first `name`/`qualified_name` child is the type.
            uint32_t n = ts_node_named_child_count(rhs);
            for (uint32_t i = 0; i < n; ++i) {
                TSNode c = ts_node_named_child(rhs, i);
                std::string_view ct(ts_node_type(c));
                if (ct == "name" || ct == "qualified_name") {
                    local_var_types_[std::string(node_text(lhs))] =
                        go_bare_type(node_text(c));
                    break;
                }
            }
        }
        return;
    }

    if (node_type == "member_call_expression" ||
        node_type == "nullsafe_member_call_expression") {
        TSNode obj = ts_node_child_by_field_name(node, "object",
                                                 static_cast<uint32_t>(6));
        TSNode nm = ts_node_child_by_field_name(node, "name",
                                                static_cast<uint32_t>(4));
        if (ts_node_is_null(nm)) return;
        Reference cref =
            create_reference(nm, ReferenceType::Call, RefStrength::Tight);
        std::string_view recv =
            ts_node_is_null(obj) ? std::string_view() : node_text(obj);
        qualify_and_push(references_, std::move(cref), local_var_types_, recv,
                         node_text(nm));
    } else if (node_type == "function_call_expression") {
        TSNode func = ts_node_child_by_field_name(node, "function",
                                                  static_cast<uint32_t>(8));
        if (!ts_node_is_null(func) &&
            std::string_view(ts_node_type(func)) == "name") {
            references_.push_back(
                create_reference(func, ReferenceType::Call, RefStrength::Tight));
        }
    }
}

void UnifiedExtractor::process_kotlin_reference(TSNode node,
                                                std::string_view node_type) {
    // Kotlin's grammar is largely fieldless; navigate by ordered children.
    if (node_type == "function_declaration") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["this"] = cls;
        return;
    }
    if (node_type == "property_declaration") {
        // `val a: A` or `val a = A()`. The variable_declaration child holds the
        // name (+ optional user_type); a call_expression sibling yields the
        // constructed type for `= A()`.
        TSNode vd{};
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_named_child(node, i);
            if (std::string_view(ts_node_type(c)) == "variable_declaration") {
                vd = c;
                break;
            }
        }
        if (ts_node_is_null(vd)) return;
        std::string_view name;
        std::string type;
        uint32_t vc = ts_node_named_child_count(vd);
        for (uint32_t i = 0; i < vc; ++i) {
            TSNode c = ts_node_named_child(vd, i);
            std::string_view ct(ts_node_type(c));
            if (ct == "simple_identifier" && name.empty())
                name = node_text(c);
            else if (ct == "user_type")
                type = js_bare_type(node_text(c));
        }
        if (type.empty()) {
            for (uint32_t i = 0; i < n; ++i) {
                TSNode c = ts_node_named_child(node, i);
                if (std::string_view(ts_node_type(c)) == "call_expression") {
                    TSNode callee = ts_node_named_child(c, 0);
                    if (!ts_node_is_null(callee) &&
                        std::string_view(ts_node_type(callee)) ==
                            "simple_identifier")
                        type = std::string(node_text(callee));
                }
            }
        }
        if (!name.empty() && !type.empty())
            local_var_types_[std::string(name)] = type;
        return;
    }

    if (node_type == "call_expression") {
        TSNode first = ts_node_named_child(node, 0);
        if (ts_node_is_null(first)) return;
        std::string_view ft(ts_node_type(first));
        if (ft == "navigation_expression") {
            // receiver . method  — first child is the receiver, then a
            // navigation_suffix whose simple_identifier is the method.
            TSNode recv = ts_node_named_child(first, 0);
            TSNode suffix{};
            uint32_t nc = ts_node_named_child_count(first);
            for (uint32_t i = 0; i < nc; ++i) {
                TSNode c = ts_node_named_child(first, i);
                if (std::string_view(ts_node_type(c)) == "navigation_suffix")
                    suffix = c;
            }
            if (ts_node_is_null(suffix)) return;
            TSNode m = ts_node_named_child(suffix, 0);
            if (ts_node_is_null(m)) return;
            Reference cref =
                create_reference(m, ReferenceType::Call, RefStrength::Tight);
            std::string_view rt =
                ts_node_is_null(recv) ? std::string_view() : node_text(recv);
            qualify_and_push(references_, std::move(cref), local_var_types_, rt,
                             node_text(m));
        } else if (ft == "simple_identifier") {
            qualify_and_push(
                references_,
                create_reference(first, ReferenceType::Call, RefStrength::Tight),
                local_var_types_, "this", node_text(first));
        }
    }
}

void UnifiedExtractor::process_ruby_reference(TSNode node,
                                              std::string_view node_type) {
    // self -> enclosing class; `x = T.new` locals.
    if (node_type == "method" || node_type == "singleton_method") {
        local_var_types_.clear();
        std::string cls = enclosing_class_name();
        if (!cls.empty()) local_var_types_["self"] = cls;
        return;
    }
    if (node_type == "assignment") {
        TSNode lhs = ts_node_child_by_field_name(node, "left",
                                                 static_cast<uint32_t>(4));
        TSNode rhs = ts_node_child_by_field_name(node, "right",
                                                 static_cast<uint32_t>(5));
        if (!ts_node_is_null(lhs) && !ts_node_is_null(rhs) &&
            std::string_view(ts_node_type(lhs)) == "identifier" &&
            std::string_view(ts_node_type(rhs)) == "call") {
            // `T.new` — receiver is the class constant.
            TSNode rc = ts_node_child_by_field_name(rhs, "receiver",
                                                    static_cast<uint32_t>(8));
            TSNode mm = ts_node_child_by_field_name(rhs, "method",
                                                    static_cast<uint32_t>(6));
            if (!ts_node_is_null(rc) && !ts_node_is_null(mm) &&
                std::string_view(ts_node_type(rc)) == "constant" &&
                node_text(mm) == "new")
                local_var_types_[std::string(node_text(lhs))] =
                    std::string(node_text(rc));
        }
        return;
    }

    if (node_type == "call") {
        TSNode mm = ts_node_child_by_field_name(node, "method",
                                                static_cast<uint32_t>(6));
        if (ts_node_is_null(mm) ||
            std::string_view(ts_node_type(mm)) != "identifier")
            return;
        if (node_text(mm) == "new") return;  // constructor, not a call edge
        TSNode recv = ts_node_child_by_field_name(node, "receiver",
                                                  static_cast<uint32_t>(8));
        Reference cref =
            create_reference(mm, ReferenceType::Call, RefStrength::Tight);
        std::string_view rt =
            ts_node_is_null(recv) ? std::string_view("self") : node_text(recv);
        qualify_and_push(references_, std::move(cref), local_var_types_, rt,
                         node_text(mm));
    }
}

void UnifiedExtractor::process_zig_reference(TSNode node,
                                             std::string_view node_type) {
    // Methods take an explicit `self: T` first param; `const a = T{}` locals.
    if (node_type == "function_declaration") {
        local_var_types_.clear();
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_child(node, i);
            if (std::string_view(ts_node_type(c)) != "parameters") continue;
            uint32_t pc = ts_node_named_child_count(c);
            for (uint32_t k = 0; k < pc; ++k) {
                TSNode p = ts_node_named_child(c, k);
                if (std::string_view(ts_node_type(p)) != "parameter") continue;
                TSNode nm = ts_node_child_by_field_name(
                    p, "name", static_cast<uint32_t>(4));
                TSNode ty = ts_node_child_by_field_name(
                    p, "type", static_cast<uint32_t>(4));
                if (!ts_node_is_null(nm) && !ts_node_is_null(ty)) {
                    std::string tn = go_bare_type(node_text(ty));
                    if (!tn.empty())
                        local_var_types_[std::string(node_text(nm))] = tn;
                }
            }
        }
        return;
    }
    if (node_type == "variable_declaration") {
        // `const a = T{};` — identifier name + a struct_initializer / call whose
        // leading identifier is the type.
        std::string_view name;
        std::string type;
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode c = ts_node_child(node, i);
            std::string_view ct(ts_node_type(c));
            if (ct == "identifier" && name.empty()) {
                name = node_text(c);
            } else if (ct == "struct_initializer" || ct == "call_expression" ||
                       ct == "field_expression") {
                TSNode lead = ts_node_named_child(c, 0);
                if (!ts_node_is_null(lead) &&
                    std::string_view(ts_node_type(lead)) == "identifier")
                    type = std::string(node_text(lead));
            }
        }
        if (!name.empty() && !type.empty() && type != std::string(name))
            local_var_types_[std::string(name)] = type;
        return;
    }

    if (node_type == "call_expression") {
        TSNode func = ts_node_child_by_field_name(node, "function",
                                                  static_cast<uint32_t>(8));
        if (ts_node_is_null(func)) return;
        if (std::string_view(ts_node_type(func)) == "field_expression") {
            TSNode obj = ts_node_child_by_field_name(func, "object",
                                                     static_cast<uint32_t>(6));
            TSNode mem = ts_node_child_by_field_name(func, "member",
                                                     static_cast<uint32_t>(6));
            if (ts_node_is_null(mem)) return;
            Reference cref =
                create_reference(mem, ReferenceType::Call, RefStrength::Tight);
            std::string_view recv =
                ts_node_is_null(obj) ? std::string_view() : node_text(obj);
            qualify_and_push(references_, std::move(cref), local_var_types_, recv,
                             node_text(mem));
        } else if (std::string_view(ts_node_type(func)) == "identifier") {
            references_.push_back(
                create_reference(func, ReferenceType::Call, RefStrength::Tight));
        }
    }
}

Reference UnifiedExtractor::create_reference(TSNode node,
                                             ReferenceType ref_type,
                                             RefStrength strength) {
    TSPoint start = ts_node_start_point(node);

    Reference ref;
    ref.id = ref_id_++;
    ref.file_id = file_id_;
    ref.line = static_cast<int>(start.row) + 1;
    ref.column = static_cast<int>(start.column) + 1;
    ref.type = ref_type;
    ref.strength = strength;
    ref.referenced_name = std::string(node_text(node));
    return ref;
}

}  // namespace lci::parser
