#include <lci/parser/unified_extractor.h>

#include <tree_sitter/api.h>

#include <cstring>

namespace lci::parser {

// ---------------------------------------------------------------------------
// Type relationship extraction (implements, extends, embeds)
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_type_relationships(
    TSNode node, std::string_view node_type) {
    if (ext_ == ".go") {
        process_go_type_relationships(node, node_type);
    } else if (ext_ == ".js" || ext_ == ".jsx" || ext_ == ".ts" ||
               ext_ == ".tsx") {
        process_js_type_relationships(node, node_type);
    } else if (ext_ == ".py") {
        process_python_type_relationships(node, node_type);
    }
}

// ---------------------------------------------------------------------------
// Go type relationships
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_go_type_relationships(
    TSNode node, std::string_view node_type) {
    if (node_type != "type_declaration") return;

    // Find the type_spec to get type name and underlying type
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode type_spec = ts_node_child(node, i);
        if (get_node_type(type_spec) != "type_spec") continue;

        std::string type_name;
        TSNode underlying{};
        bool has_underlying = false;

        uint32_t spec_count = ts_node_child_count(type_spec);
        for (uint32_t j = 0; j < spec_count; ++j) {
            TSNode child = ts_node_child(type_spec, j);
            std::string_view ct = get_node_type(child);
            if (ct == "type_identifier") {
                type_name = std::string(node_text(child));
            } else if (ct == "interface_type" || ct == "struct_type") {
                underlying = child;
                has_underlying = true;
            }
        }

        if (type_name.empty() || !has_underlying) continue;

        std::string_view underlying_type = get_node_type(underlying);

        if (underlying_type == "interface_type" ||
            underlying_type == "struct_type") {
            // Extract embedded types as references
            uint32_t ucount = ts_node_child_count(underlying);
            for (uint32_t k = 0; k < ucount; ++k) {
                TSNode member = ts_node_child(underlying, k);
                std::string_view mt = get_node_type(member);

                // Interface embedding: type_identifier children
                if (mt == "type_identifier") {
                    Reference ref;
                    ref.id = ref_id_++;
                    ref.file_id = file_id_;
                    TSPoint sp = ts_node_start_point(member);
                    ref.line = static_cast<int>(sp.row) + 1;
                    ref.column = static_cast<int>(sp.column) + 1;
                    ref.type = (underlying_type == "interface_type")
                                   ? ReferenceType::Extends
                                   : ReferenceType::Extends;
                    ref.strength = RefStrength::Tight;
                    ref.referenced_name = std::string(node_text(member));
                    references_.push_back(std::move(ref));
                }

                // Struct embedding: field_declaration with anonymous type
                if (mt == "field_declaration_list") {
                    uint32_t fcount = ts_node_child_count(member);
                    for (uint32_t f = 0; f < fcount; ++f) {
                        TSNode field = ts_node_child(member, f);
                        if (get_node_type(field) != "field_declaration") {
                            continue;
                        }
                        // Anonymous embed: only has type, no name
                        uint32_t fc = ts_node_child_count(field);
                        bool has_field_name = false;
                        TSNode embed_type{};
                        bool has_embed = false;
                        for (uint32_t g = 0; g < fc; ++g) {
                            TSNode fc_child = ts_node_child(field, g);
                            std::string_view fct = get_node_type(fc_child);
                            if (fct == "field_identifier") {
                                has_field_name = true;
                            } else if (fct == "type_identifier" ||
                                       fct == "qualified_type") {
                                embed_type = fc_child;
                                has_embed = true;
                            }
                        }
                        if (!has_field_name && has_embed) {
                            Reference ref;
                            ref.id = ref_id_++;
                            ref.file_id = file_id_;
                            TSPoint sp = ts_node_start_point(embed_type);
                            ref.line = static_cast<int>(sp.row) + 1;
                            ref.column = static_cast<int>(sp.column) + 1;
                            ref.type = ReferenceType::Extends;
                            ref.strength = RefStrength::Tight;
                            ref.referenced_name =
                                std::string(node_text(embed_type));
                            references_.push_back(std::move(ref));
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// JavaScript/TypeScript type relationships
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_js_type_relationships(
    TSNode node, std::string_view node_type) {
    if (node_type != "class_declaration" &&
        node_type != "class_definition") {
        return;
    }

    // Look for extends clause
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(node, i);
        std::string_view ct = get_node_type(child);
        if (ct == "class_heritage") {
            // TypeScript: class_heritage contains extends_clause
            uint32_t hcount = ts_node_child_count(child);
            for (uint32_t j = 0; j < hcount; ++j) {
                TSNode hchild = ts_node_child(child, j);
                std::string_view ht = get_node_type(hchild);
                if (ht == "extends_clause" || ht == "implements_clause") {
                    ReferenceType rt = (ht == "extends_clause")
                                           ? ReferenceType::Extends
                                           : ReferenceType::Implements;
                    // Extract type names from the clause
                    uint32_t ec = ts_node_child_count(hchild);
                    for (uint32_t k = 0; k < ec; ++k) {
                        TSNode type_node = ts_node_child(hchild, k);
                        std::string_view tt = get_node_type(type_node);
                        if (tt == "identifier" || tt == "type_identifier" ||
                            tt == "member_expression") {
                            Reference ref;
                            ref.id = ref_id_++;
                            ref.file_id = file_id_;
                            TSPoint sp = ts_node_start_point(type_node);
                            ref.line = static_cast<int>(sp.row) + 1;
                            ref.column = static_cast<int>(sp.column) + 1;
                            ref.type = rt;
                            ref.strength = RefStrength::Tight;
                            ref.referenced_name =
                                std::string(node_text(type_node));
                            references_.push_back(std::move(ref));
                        }
                    }
                }
            }
        }
        // Simple extends for JavaScript
        if (ct == "identifier" && i > 0) {
            TSNode prev = ts_node_child(node, i - 1);
            if (!ts_node_is_null(prev) && get_node_type(prev) == "extends") {
                Reference ref;
                ref.id = ref_id_++;
                ref.file_id = file_id_;
                TSPoint sp = ts_node_start_point(child);
                ref.line = static_cast<int>(sp.row) + 1;
                ref.column = static_cast<int>(sp.column) + 1;
                ref.type = ReferenceType::Extends;
                ref.strength = RefStrength::Tight;
                ref.referenced_name = std::string(node_text(child));
                references_.push_back(std::move(ref));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Python type relationships
// ---------------------------------------------------------------------------

void UnifiedExtractor::process_python_type_relationships(
    TSNode node, std::string_view node_type) {
    if (node_type != "class_definition") return;

    // Python class inheritance: class Foo(Bar, Baz):
    TSNode superclasses = ts_node_child_by_field_name(
        node, "superclasses",
        static_cast<uint32_t>(std::strlen("superclasses")));
    if (ts_node_is_null(superclasses)) return;

    uint32_t count = ts_node_child_count(superclasses);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(superclasses, i);
        std::string_view ct = get_node_type(child);
        if (ct == "identifier" || ct == "attribute") {
            Reference ref;
            ref.id = ref_id_++;
            ref.file_id = file_id_;
            TSPoint sp = ts_node_start_point(child);
            ref.line = static_cast<int>(sp.row) + 1;
            ref.column = static_cast<int>(sp.column) + 1;
            ref.type = ReferenceType::Extends;
            ref.strength = RefStrength::Tight;
            ref.referenced_name = std::string(node_text(child));
            references_.push_back(std::move(ref));
        }
    }
}

}  // namespace lci::parser
