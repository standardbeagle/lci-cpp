#include <gtest/gtest.h>

#include <lci/file_info.h>
#include <lci/reference.h>
#include <lci/scope.h>
#include <lci/symbol.h>
#include <lci/types.h>

namespace lci {
namespace {

// ---------------------------------------------------------------------------
// SymbolType enum: all 26 variants
// ---------------------------------------------------------------------------
TEST(SymbolTypeTest, Has26Variants) {
    EXPECT_EQ(static_cast<int>(SymbolType::Function), 0);
    EXPECT_EQ(static_cast<int>(SymbolType::Constructor), 25);
    EXPECT_EQ(kSymbolTypeCount, 26);
}

TEST(SymbolTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(SymbolType::Function), "function");
    EXPECT_EQ(to_string(SymbolType::Class), "class");
    EXPECT_EQ(to_string(SymbolType::Method), "method");
    EXPECT_EQ(to_string(SymbolType::Variable), "variable");
    EXPECT_EQ(to_string(SymbolType::Constant), "constant");
    EXPECT_EQ(to_string(SymbolType::Interface), "interface");
    EXPECT_EQ(to_string(SymbolType::Type), "type");
    EXPECT_EQ(to_string(SymbolType::Struct), "struct");
    EXPECT_EQ(to_string(SymbolType::Module), "module");
    EXPECT_EQ(to_string(SymbolType::Namespace), "namespace");
    EXPECT_EQ(to_string(SymbolType::Property), "property");
    EXPECT_EQ(to_string(SymbolType::Event), "event");
    EXPECT_EQ(to_string(SymbolType::Delegate), "delegate");
    EXPECT_EQ(to_string(SymbolType::Enum), "enum");
    EXPECT_EQ(to_string(SymbolType::Record), "record");
    EXPECT_EQ(to_string(SymbolType::Operator), "operator");
    EXPECT_EQ(to_string(SymbolType::Indexer), "indexer");
    EXPECT_EQ(to_string(SymbolType::Object), "object");
    EXPECT_EQ(to_string(SymbolType::Companion), "companion");
    EXPECT_EQ(to_string(SymbolType::Extension), "extension");
    EXPECT_EQ(to_string(SymbolType::Annotation), "annotation");
    EXPECT_EQ(to_string(SymbolType::Field), "field");
    EXPECT_EQ(to_string(SymbolType::EnumMember), "enum_member");
    EXPECT_EQ(to_string(SymbolType::Trait), "trait");
    EXPECT_EQ(to_string(SymbolType::Impl), "impl");
    EXPECT_EQ(to_string(SymbolType::Constructor), "constructor");
}

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------
TEST(TypeAliasTest, FileIDIsUint32) {
    static_assert(sizeof(FileID) == 4);
    FileID fid = 42;
    EXPECT_EQ(fid, 42u);
}

TEST(TypeAliasTest, SymbolIDIsUint64) {
    static_assert(sizeof(SymbolID) == 8);
    SymbolID sid = 123456789ULL;
    EXPECT_EQ(sid, 123456789ULL);
}

// ---------------------------------------------------------------------------
// ContextAttributeType
// ---------------------------------------------------------------------------
TEST(ContextAttributeTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(ContextAttributeType::Directive), "directive");
    EXPECT_EQ(to_string(ContextAttributeType::Unsafe), "unsafe");
    EXPECT_EQ(to_string(ContextAttributeType::Lock), "lock");
    EXPECT_EQ(to_string(ContextAttributeType::Decorator), "decorator");
    EXPECT_EQ(to_string(ContextAttributeType::Pragma), "pragma");
    EXPECT_EQ(to_string(ContextAttributeType::Iterator), "iterator");
    EXPECT_EQ(to_string(ContextAttributeType::Async), "async");
    EXPECT_EQ(to_string(ContextAttributeType::Volatile), "volatile");
    EXPECT_EQ(to_string(ContextAttributeType::Deprecated), "deprecated");
    EXPECT_EQ(to_string(ContextAttributeType::Experimental), "experimental");
    EXPECT_EQ(to_string(ContextAttributeType::Pure), "pure");
    EXPECT_EQ(to_string(ContextAttributeType::NoThrow), "nothrow");
    EXPECT_EQ(to_string(ContextAttributeType::SideEffect), "side_effect");
    EXPECT_EQ(to_string(ContextAttributeType::Recursive), "recursive");
    EXPECT_EQ(to_string(ContextAttributeType::Exported), "exported");
    EXPECT_EQ(to_string(ContextAttributeType::Inline), "inline");
    EXPECT_EQ(to_string(ContextAttributeType::Virtual), "virtual");
    EXPECT_EQ(to_string(ContextAttributeType::Abstract), "abstract");
    EXPECT_EQ(to_string(ContextAttributeType::Static), "static");
    EXPECT_EQ(to_string(ContextAttributeType::Final), "final");
    EXPECT_EQ(to_string(ContextAttributeType::Const), "const");
    EXPECT_EQ(to_string(ContextAttributeType::Generator), "generator");
    EXPECT_EQ(to_string(ContextAttributeType::Coroutine), "coroutine");
}

// ---------------------------------------------------------------------------
// VariableType
// ---------------------------------------------------------------------------
TEST(VariableTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(VariableType::Global), "global");
    EXPECT_EQ(to_string(VariableType::Local), "local");
    EXPECT_EQ(to_string(VariableType::Parameter), "parameter");
    EXPECT_EQ(to_string(VariableType::Field), "field");
    EXPECT_EQ(to_string(VariableType::Member), "member");
    EXPECT_EQ(to_string(VariableType::Constant), "constant");
}

// ---------------------------------------------------------------------------
// Bitfield flags: VariableFlags
// ---------------------------------------------------------------------------
TEST(VariableFlagsTest, BitPositions) {
    EXPECT_EQ(variable_flags::kConst, 0x01);
    EXPECT_EQ(variable_flags::kStatic, 0x02);
    EXPECT_EQ(variable_flags::kPointer, 0x04);
    EXPECT_EQ(variable_flags::kArray, 0x08);
    EXPECT_EQ(variable_flags::kChannel, 0x10);
    EXPECT_EQ(variable_flags::kInterface, 0x20);
}

TEST(VariableFlagsTest, FitInUint8) {
    uint8_t all = variable_flags::kConst | variable_flags::kStatic | variable_flags::kPointer |
                  variable_flags::kArray | variable_flags::kChannel | variable_flags::kInterface;
    EXPECT_EQ(all, 0x3F);
}

// ---------------------------------------------------------------------------
// Bitfield flags: FunctionFlags
// ---------------------------------------------------------------------------
TEST(FunctionFlagsTest, BitPositions) {
    EXPECT_EQ(function_flags::kAsync, 0x01);
    EXPECT_EQ(function_flags::kGenerator, 0x02);
    EXPECT_EQ(function_flags::kMethod, 0x04);
    EXPECT_EQ(function_flags::kVariadic, 0x08);
}

TEST(FunctionFlagsTest, FitInUint8) {
    uint8_t all = function_flags::kAsync | function_flags::kGenerator |
                  function_flags::kMethod | function_flags::kVariadic;
    EXPECT_EQ(all, 0x0F);
}

// ---------------------------------------------------------------------------
// Symbol struct
// ---------------------------------------------------------------------------
TEST(SymbolTest, DefaultConstruction) {
    Symbol s;
    EXPECT_TRUE(s.name.empty());
    EXPECT_EQ(s.type, SymbolType::Function);
    EXPECT_EQ(s.file_id, 0u);
    EXPECT_EQ(s.line, 0);
    EXPECT_EQ(s.column, 0);
    EXPECT_EQ(s.end_line, 0);
    EXPECT_EQ(s.end_column, 0);
}

// ---------------------------------------------------------------------------
// EnhancedSymbol with flag accessors
// ---------------------------------------------------------------------------
TEST(EnhancedSymbolTest, VariableFlagAccessors) {
    EnhancedSymbol es;
    es.variable_flags = 0;
    EXPECT_FALSE(es.is_const());
    EXPECT_FALSE(es.is_static());
    EXPECT_FALSE(es.is_pointer());
    EXPECT_FALSE(es.is_array());
    EXPECT_FALSE(es.is_channel());
    EXPECT_FALSE(es.is_interface());

    es.variable_flags = variable_flags::kConst | variable_flags::kPointer;
    EXPECT_TRUE(es.is_const());
    EXPECT_FALSE(es.is_static());
    EXPECT_TRUE(es.is_pointer());
    EXPECT_FALSE(es.is_array());
}

TEST(EnhancedSymbolTest, FunctionFlagAccessors) {
    EnhancedSymbol es;
    es.function_flags = 0;
    EXPECT_FALSE(es.is_async_func());
    EXPECT_FALSE(es.is_generator_func());
    EXPECT_FALSE(es.is_method_func());
    EXPECT_FALSE(es.is_variadic_func());

    es.function_flags = function_flags::kAsync | function_flags::kVariadic;
    EXPECT_TRUE(es.is_async_func());
    EXPECT_FALSE(es.is_generator_func());
    EXPECT_FALSE(es.is_method_func());
    EXPECT_TRUE(es.is_variadic_func());
}

TEST(EnhancedSymbolTest, BitfieldLayoutMatchesGo) {
    // Go uses uint8 for both variable_flags and function_flags
    static_assert(sizeof(EnhancedSymbol::variable_flags) == 1);
    static_assert(sizeof(EnhancedSymbol::function_flags) == 1);
    static_assert(sizeof(EnhancedSymbol::parameter_count) == 1);
}

// ---------------------------------------------------------------------------
// ReferenceType
// ---------------------------------------------------------------------------
TEST(ReferenceTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(ReferenceType::Import), "import");
    EXPECT_EQ(to_string(ReferenceType::Call), "call");
    EXPECT_EQ(to_string(ReferenceType::Inheritance), "inheritance");
    EXPECT_EQ(to_string(ReferenceType::Assignment), "assignment");
    EXPECT_EQ(to_string(ReferenceType::Declaration), "declaration");
    EXPECT_EQ(to_string(ReferenceType::Parameter), "parameter");
    EXPECT_EQ(to_string(ReferenceType::Return), "return");
    EXPECT_EQ(to_string(ReferenceType::TypeAnnotation), "type_annotation");
    EXPECT_EQ(to_string(ReferenceType::Implements), "implements");
    EXPECT_EQ(to_string(ReferenceType::Extends), "extends");
    EXPECT_EQ(to_string(ReferenceType::Usage), "usage");
}

// ---------------------------------------------------------------------------
// RefStrength
// ---------------------------------------------------------------------------
TEST(RefStrengthTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(RefStrength::Tight), "tight");
    EXPECT_EQ(to_string(RefStrength::Loose), "loose");
    EXPECT_EQ(to_string(RefStrength::Transitive), "transitive");
}

// ---------------------------------------------------------------------------
// Reference struct
// ---------------------------------------------------------------------------
TEST(ReferenceTest, DefaultConstruction) {
    Reference r;
    EXPECT_EQ(r.id, 0u);
    EXPECT_EQ(r.source_symbol, 0u);
    EXPECT_EQ(r.target_symbol, 0u);
    EXPECT_EQ(r.file_id, 0u);
    EXPECT_EQ(r.line, 0);
    EXPECT_EQ(r.column, 0);
    EXPECT_EQ(r.type, ReferenceType::Import);
    EXPECT_EQ(r.strength, RefStrength::Tight);
    EXPECT_FALSE(r.ambiguous);
}

// ---------------------------------------------------------------------------
// BlockType
// ---------------------------------------------------------------------------
TEST(BlockTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(BlockType::Function), "function");
    EXPECT_EQ(to_string(BlockType::Class), "class");
    EXPECT_EQ(to_string(BlockType::Method), "method");
    EXPECT_EQ(to_string(BlockType::Interface), "interface");
    EXPECT_EQ(to_string(BlockType::Struct), "struct");
    EXPECT_EQ(to_string(BlockType::Variable), "variable");
    EXPECT_EQ(to_string(BlockType::Block), "block");
    EXPECT_EQ(to_string(BlockType::Enum), "enum");
    EXPECT_EQ(to_string(BlockType::Trait), "trait");
    EXPECT_EQ(to_string(BlockType::Impl), "impl");
    EXPECT_EQ(to_string(BlockType::Module), "module");
    EXPECT_EQ(to_string(BlockType::Namespace), "namespace");
    EXPECT_EQ(to_string(BlockType::Constructor), "constructor");
    EXPECT_EQ(to_string(BlockType::Other), "other");
}

// ---------------------------------------------------------------------------
// ScopeType
// ---------------------------------------------------------------------------
TEST(ScopeTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(ScopeType::Folder), "folder");
    EXPECT_EQ(to_string(ScopeType::File), "file");
    EXPECT_EQ(to_string(ScopeType::Package), "package");
    EXPECT_EQ(to_string(ScopeType::Namespace), "namespace");
    EXPECT_EQ(to_string(ScopeType::Class), "class");
    EXPECT_EQ(to_string(ScopeType::Interface), "interface");
    EXPECT_EQ(to_string(ScopeType::Function), "function");
    EXPECT_EQ(to_string(ScopeType::Method), "method");
    EXPECT_EQ(to_string(ScopeType::Variable), "variable");
    EXPECT_EQ(to_string(ScopeType::Block), "block");
    EXPECT_EQ(to_string(ScopeType::Struct), "struct");
}

// ---------------------------------------------------------------------------
// SymbolScopeType
// ---------------------------------------------------------------------------
TEST(SymbolScopeTypeTest, ToStringCoversAllVariants) {
    EXPECT_EQ(to_string(SymbolScopeType::Global), "global");
    EXPECT_EQ(to_string(SymbolScopeType::Module), "module");
    EXPECT_EQ(to_string(SymbolScopeType::Package), "package");
    EXPECT_EQ(to_string(SymbolScopeType::Class), "class");
    EXPECT_EQ(to_string(SymbolScopeType::Function), "function");
    EXPECT_EQ(to_string(SymbolScopeType::Method), "method");
    EXPECT_EQ(to_string(SymbolScopeType::Block), "block");
    EXPECT_EQ(to_string(SymbolScopeType::Namespace), "namespace");
    EXPECT_EQ(to_string(SymbolScopeType::Interface), "interface");
}

// ---------------------------------------------------------------------------
// ScopeInfo struct
// ---------------------------------------------------------------------------
TEST(ScopeInfoTest, DefaultConstruction) {
    ScopeInfo si;
    EXPECT_EQ(si.type, ScopeType::Folder);
    EXPECT_TRUE(si.name.empty());
    EXPECT_EQ(si.start_line, 0);
    EXPECT_EQ(si.end_line, 0);
    EXPECT_EQ(si.level, 0);
}

// ---------------------------------------------------------------------------
// CharMask
// ---------------------------------------------------------------------------
TEST(CharMaskTest, DefaultConstruction) {
    CharMask cm;
    for (auto w : cm.ascii_mask) {
        EXPECT_EQ(w, 0u);
    }
    EXPECT_FALSE(cm.has_unicode);
}

TEST(CharMaskTest, SizeofAsciiMask) {
    static_assert(sizeof(CharMask::ascii_mask) == 32);
}

// ---------------------------------------------------------------------------
// FileInfo struct
// ---------------------------------------------------------------------------
TEST(FileInfoTest, DefaultConstruction) {
    FileInfo fi;
    EXPECT_EQ(fi.id, 0u);
    EXPECT_TRUE(fi.path.empty());
    EXPECT_EQ(fi.checksum, 0u);
    EXPECT_EQ(fi.fast_hash, 0u);
    EXPECT_TRUE(fi.blocks.empty());
    EXPECT_TRUE(fi.imports.empty());
    EXPECT_TRUE(fi.references.empty());
    EXPECT_TRUE(fi.line_offsets.empty());
    EXPECT_TRUE(fi.perf_data.empty());
}

TEST(FileInfoTest, ContentHashSize) {
    static_assert(sizeof(FileInfo::content_hash) == 32);
}

// ---------------------------------------------------------------------------
// Import struct
// ---------------------------------------------------------------------------
TEST(ImportTest, DefaultConstruction) {
    Import imp;
    EXPECT_TRUE(imp.path.empty());
    EXPECT_EQ(imp.file_id, 0u);
    EXPECT_EQ(imp.line, 0);
}

// ---------------------------------------------------------------------------
// FunctionPerfData
// ---------------------------------------------------------------------------
TEST(FunctionPerfDataTest, DefaultConstruction) {
    FunctionPerfData fpd;
    EXPECT_TRUE(fpd.name.empty());
    EXPECT_EQ(fpd.start_line, 0);
    EXPECT_EQ(fpd.end_line, 0);
    EXPECT_FALSE(fpd.is_async);
    EXPECT_TRUE(fpd.loops.empty());
    EXPECT_TRUE(fpd.awaits.empty());
    EXPECT_TRUE(fpd.calls.empty());
}

// ---------------------------------------------------------------------------
// RefStats / RefCount
// ---------------------------------------------------------------------------
TEST(RefCountTest, DefaultConstruction) {
    RefCount rc;
    EXPECT_EQ(rc.incoming_count, 0);
    EXPECT_EQ(rc.outgoing_count, 0);
    EXPECT_TRUE(rc.incoming_files.empty());
    EXPECT_TRUE(rc.outgoing_files.empty());
    EXPECT_EQ(rc.strength.tight, 0);
    EXPECT_EQ(rc.strength.loose, 0);
    EXPECT_EQ(rc.strength.transitive, 0);
}

}  // namespace
}  // namespace lci
