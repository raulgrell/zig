/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef ZIG_ALL_TYPES_HPP
#define ZIG_ALL_TYPES_HPP

#include "list.hpp"
#include "buffer.hpp"
#include "zig_llvm.hpp"
#include "hash_map.hpp"
#include "errmsg.hpp"
#include "bignum.hpp"
#include "target.hpp"

struct AstNode;
struct ImportTableEntry;
struct FnTableEntry;
struct Scope;
struct ScopeBlock;
struct ScopeFnDef;
struct TypeTableEntry;
struct VariableTableEntry;
struct ErrorTableEntry;
struct LabelTableEntry;
struct BuiltinFnEntry;
struct TypeStructField;
struct CodeGen;
struct ConstExprValue;
struct IrInstruction;
struct IrInstructionCast;
struct IrBasicBlock;
struct ScopeDecls;

struct IrGotoItem {
    AstNode *source_node;
    IrBasicBlock *bb;
    size_t instruction_index;
    Scope *scope;
};

struct IrExecutable {
    ZigList<IrBasicBlock *> basic_block_list;
    Buf *name;
    size_t mem_slot_count;
    size_t next_debug_id;
    size_t *backward_branch_count;
    size_t backward_branch_quota;
    bool invalid;
    ZigList<LabelTableEntry *> all_labels;
    ZigList<IrGotoItem> goto_list;
    bool is_inline;
    FnTableEntry *fn_entry;
    Buf *c_import_buf;
    AstNode *source_node;
    IrExecutable *parent_exec;
    Scope *begin_scope;
};

enum OutType {
    OutTypeUnknown,
    OutTypeExe,
    OutTypeLib,
    OutTypeObj,
};

enum ConstParentId {
    ConstParentIdNone,
    ConstParentIdStruct,
    ConstParentIdArray,
};

struct ConstParent {
    ConstParentId id;

    union {
        struct {
            ConstExprValue *array_val;
            size_t elem_index;
        } p_array;
        struct {
            ConstExprValue *struct_val;
            size_t field_index;
        } p_struct;
    } data;
};

struct ConstEnumValue {
    uint64_t tag;
    ConstExprValue *payload;
};

struct ConstStructValue {
    ConstExprValue *fields;
    ConstParent parent;
};

struct ConstArrayValue {
    ConstExprValue *elements;
    ConstParent parent;
};

enum ConstPtrSpecial {
    // Enforce explicitly setting this ID by making the zero value invalid.
    ConstPtrSpecialInvalid,
    // The pointer is a reference to a single object.
    ConstPtrSpecialRef,
    // The pointer points to an element in an underlying array.
    ConstPtrSpecialBaseArray,
    // The pointer points to a field in an underlying struct.
    ConstPtrSpecialBaseStruct,
    // This means that we did a compile-time pointer reinterpret and we cannot
    // understand the value of pointee at compile time. However, we will still
    // emit a binary with a compile time known address.
    // In this case index is the numeric address value.
    ConstPtrSpecialHardCodedAddr,
    // This means that the pointer represents memory of assigning to _.
    // That is, storing discards the data, and loading is invalid.
    ConstPtrSpecialDiscard,
};

enum ConstPtrMut {
    // The pointer points to memory that is known at compile time and immutable.
    ConstPtrMutComptimeConst,
    // This means that the pointer points to memory used by a comptime variable,
    // so attempting to write a non-compile-time known value is an error
    // But the underlying value is allowed to change at compile time.
    ConstPtrMutComptimeVar,
    // The pointer points to memory that is known only at runtime.
    // For example it may point to the initializer value of a variable.
    ConstPtrMutRuntimeVar,
};

struct ConstPtrValue {
    ConstPtrSpecial special;
    ConstPtrMut mut;

    union {
        struct {
            ConstExprValue *pointee;
        } ref;
        struct {
            ConstExprValue *array_val;
            size_t elem_index;
            // This helps us preserve the null byte when performing compile-time
            // concatenation on C strings.
            bool is_cstr;
        } base_array;
        struct {
            ConstExprValue *struct_val;
            size_t field_index;
        } base_struct;
        struct {
            uint64_t addr;
        } hard_coded_addr;
    } data;
};

struct ConstErrValue {
    ErrorTableEntry *err;
    ConstExprValue *payload;
};

struct ConstBoundFnValue {
    FnTableEntry *fn;
    IrInstruction *first_arg;
    bool is_inline;
};

struct ConstArgTuple {
    size_t start_index;
    size_t end_index;
};

enum ConstValSpecial {
    ConstValSpecialRuntime,
    ConstValSpecialStatic,
    ConstValSpecialUndef,
};

enum RuntimeHintErrorUnion {
    RuntimeHintErrorUnionUnknown,
    RuntimeHintErrorUnionError,
    RuntimeHintErrorUnionNonError,
};

enum RuntimeHintMaybe {
    RuntimeHintMaybeUnknown,
    RuntimeHintMaybeNull, // TODO is this value even possible? if this is the case it might mean the const value is compile time known.
    RuntimeHintMaybeNonNull,
};

struct ConstFn {
    FnTableEntry *fn_entry;
    bool is_inline;
};

struct ConstExprValue {
    TypeTableEntry *type;
    ConstValSpecial special;
    LLVMValueRef llvm_value;
    LLVMValueRef llvm_global;

    union {
        // populated if special == ConstValSpecialStatic
        BigNum x_bignum;
        bool x_bool;
        ConstFn x_fn;
        ConstBoundFnValue x_bound_fn;
        TypeTableEntry *x_type;
        ConstExprValue *x_maybe;
        ConstErrValue x_err_union;
        ErrorTableEntry *x_pure_err;
        ConstEnumValue x_enum;
        ConstStructValue x_struct;
        ConstArrayValue x_array;
        ConstPtrValue x_ptr;
        ImportTableEntry *x_import;
        Scope *x_block;
        ConstArgTuple x_arg_tuple;

        // populated if special == ConstValSpecialRuntime
        RuntimeHintErrorUnion rh_error_union;
        RuntimeHintMaybe rh_maybe;
    } data;
};

enum ReturnKnowledge {
    ReturnKnowledgeUnknown,
    ReturnKnowledgeKnownError,
    ReturnKnowledgeKnownNonError,
    ReturnKnowledgeKnownNull,
    ReturnKnowledgeKnownNonNull,
    ReturnKnowledgeSkipDefers,
};

enum VisibMod {
    VisibModPrivate,
    VisibModPub,
    VisibModExport,
};

enum GlobalLinkageId {
    GlobalLinkageIdInternal,
    GlobalLinkageIdStrong,
    GlobalLinkageIdWeak,
    GlobalLinkageIdLinkOnce,
};

enum TldId {
    TldIdVar,
    TldIdFn,
    TldIdContainer,
    TldIdCompTime,
};

enum TldResolution {
    TldResolutionUnresolved,
    TldResolutionResolving,
    TldResolutionInvalid,
    TldResolutionOk,
};

struct Tld {
    TldId id;
    Buf *name;
    VisibMod visib_mod;
    AstNode *source_node;

    ImportTableEntry *import;
    Scope *parent_scope;
    // set this flag temporarily to detect infinite loops
    bool dep_loop_flag;
    TldResolution resolution;
};

struct TldVar {
    Tld base;

    VariableTableEntry *var;
    AstNode *set_global_align_node;
    uint32_t alignment;
    AstNode *set_global_section_node;
    Buf *section_name;
    AstNode *set_global_linkage_node;
    GlobalLinkageId linkage;
};

struct TldFn {
    Tld base;

    FnTableEntry *fn_entry;
};

struct TldContainer {
    Tld base;

    ScopeDecls *decls_scope;
    TypeTableEntry *type_entry;
};

struct TldCompTime {
    Tld base;
};

struct TypeEnumField {
    Buf *name;
    TypeTableEntry *type_entry;
    uint32_t value;
    uint32_t gen_index;
};

enum NodeType {
    NodeTypeRoot,
    NodeTypeFnProto,
    NodeTypeFnDef,
    NodeTypeFnDecl,
    NodeTypeParamDecl,
    NodeTypeBlock,
    NodeTypeGroupedExpr,
    NodeTypeReturnExpr,
    NodeTypeDefer,
    NodeTypeVariableDeclaration,
    NodeTypeErrorValueDecl,
    NodeTypeTestDecl,
    NodeTypeBinOpExpr,
    NodeTypeUnwrapErrorExpr,
    NodeTypeNumberLiteral,
    NodeTypeStringLiteral,
    NodeTypeCharLiteral,
    NodeTypeSymbol,
    NodeTypePrefixOpExpr,
    NodeTypeFnCallExpr,
    NodeTypeArrayAccessExpr,
    NodeTypeSliceExpr,
    NodeTypeFieldAccessExpr,
    NodeTypeUse,
    NodeTypeBoolLiteral,
    NodeTypeNullLiteral,
    NodeTypeUndefinedLiteral,
    NodeTypeThisLiteral,
    NodeTypeUnreachable,
    NodeTypeIfBoolExpr,
    NodeTypeIfVarExpr,
    NodeTypeWhileExpr,
    NodeTypeForExpr,
    NodeTypeSwitchExpr,
    NodeTypeSwitchProng,
    NodeTypeSwitchRange,
    NodeTypeLabel,
    NodeTypeGoto,
    NodeTypeCompTime,
    NodeTypeBreak,
    NodeTypeContinue,
    NodeTypeAsmExpr,
    NodeTypeContainerDecl,
    NodeTypeStructField,
    NodeTypeContainerInitExpr,
    NodeTypeStructValueField,
    NodeTypeArrayType,
    NodeTypeErrorType,
    NodeTypeVarLiteral,
    NodeTypeTryExpr,
    NodeTypeInlineExpr,
};

struct AstNodeRoot {
    ZigList<AstNode *> top_level_decls;
};

struct AstNodeFnProto {
    VisibMod visib_mod;
    Buf *name;
    ZigList<AstNode *> params;
    AstNode *return_type;
    bool is_var_args;
    bool is_extern;
    bool is_inline;
    bool is_coldcc;
    bool is_nakedcc;
    AstNode *fn_def_node;
};

struct AstNodeFnDef {
    AstNode *fn_proto;
    AstNode *body;
};

struct AstNodeFnDecl {
    AstNode *fn_proto;
};

struct AstNodeParamDecl {
    Buf *name;
    AstNode *type;
    bool is_noalias;
    bool is_inline;
    bool is_var_args;
};

struct AstNodeBlock {
    ZigList<AstNode *> statements;
    bool last_statement_is_result_expression;
};

enum ReturnKind {
    ReturnKindUnconditional,
    ReturnKindMaybe,
    ReturnKindError,
};

struct AstNodeReturnExpr {
    ReturnKind kind;
    // might be null in case of return void;
    AstNode *expr;
};

struct AstNodeDefer {
    ReturnKind kind;
    AstNode *expr;

    // temporary data used in IR generation
    Scope *child_scope;
    Scope *expr_scope;
};

struct AstNodeVariableDeclaration {
    VisibMod visib_mod;
    Buf *symbol;
    bool is_const;
    bool is_inline;
    bool is_extern;
    // one or both of type and expr will be non null
    AstNode *type;
    AstNode *expr;
};

struct AstNodeErrorValueDecl {
    Buf *name;

    ErrorTableEntry *err;
};

struct AstNodeTestDecl {
    Buf *name;

    AstNode *body;
};

enum BinOpType {
    BinOpTypeInvalid,
    BinOpTypeAssign,
    BinOpTypeAssignTimes,
    BinOpTypeAssignTimesWrap,
    BinOpTypeAssignDiv,
    BinOpTypeAssignMod,
    BinOpTypeAssignPlus,
    BinOpTypeAssignPlusWrap,
    BinOpTypeAssignMinus,
    BinOpTypeAssignMinusWrap,
    BinOpTypeAssignBitShiftLeft,
    BinOpTypeAssignBitShiftLeftWrap,
    BinOpTypeAssignBitShiftRight,
    BinOpTypeAssignBitAnd,
    BinOpTypeAssignBitXor,
    BinOpTypeAssignBitOr,
    BinOpTypeAssignBoolAnd,
    BinOpTypeAssignBoolOr,
    BinOpTypeBoolOr,
    BinOpTypeBoolAnd,
    BinOpTypeCmpEq,
    BinOpTypeCmpNotEq,
    BinOpTypeCmpLessThan,
    BinOpTypeCmpGreaterThan,
    BinOpTypeCmpLessOrEq,
    BinOpTypeCmpGreaterOrEq,
    BinOpTypeBinOr,
    BinOpTypeBinXor,
    BinOpTypeBinAnd,
    BinOpTypeBitShiftLeft,
    BinOpTypeBitShiftLeftWrap,
    BinOpTypeBitShiftRight,
    BinOpTypeAdd,
    BinOpTypeAddWrap,
    BinOpTypeSub,
    BinOpTypeSubWrap,
    BinOpTypeMult,
    BinOpTypeMultWrap,
    BinOpTypeDiv,
    BinOpTypeMod,
    BinOpTypeUnwrapMaybe,
    BinOpTypeArrayCat,
    BinOpTypeArrayMult,
};

struct AstNodeBinOpExpr {
    AstNode *op1;
    BinOpType bin_op;
    AstNode *op2;
};

struct AstNodeUnwrapErrorExpr {
    AstNode *op1;
    AstNode *symbol; // can be null
    AstNode *op2;
};

enum CastOp {
    CastOpNoCast, // signifies the function call expression is not a cast
    CastOpNoop, // fn call expr is a cast, but does nothing
    CastOpIntToFloat,
    CastOpFloatToInt,
    CastOpBoolToInt,
    CastOpResizeSlice,
    CastOpBytesToSlice,
};

struct AstNodeFnCallExpr {
    AstNode *fn_ref_expr;
    ZigList<AstNode *> params;
    bool is_builtin;
};

struct AstNodeArrayAccessExpr {
    AstNode *array_ref_expr;
    AstNode *subscript;
};

struct AstNodeSliceExpr {
    AstNode *array_ref_expr;
    AstNode *start;
    AstNode *end;
    bool is_const;
};

struct AstNodeFieldAccessExpr {
    AstNode *struct_expr;
    Buf *field_name;
};

enum PrefixOp {
    PrefixOpInvalid,
    PrefixOpBoolNot,
    PrefixOpBinNot,
    PrefixOpNegation,
    PrefixOpNegationWrap,
    PrefixOpAddressOf,
    PrefixOpConstAddressOf,
    PrefixOpVolatileAddressOf,
    PrefixOpConstVolatileAddressOf,
    PrefixOpDereference,
    PrefixOpMaybe,
    PrefixOpError,
    PrefixOpUnwrapError,
    PrefixOpUnwrapMaybe,
};

struct AstNodePrefixOpExpr {
    PrefixOp prefix_op;
    AstNode *primary_expr;
};

struct AstNodeUse {
    VisibMod visib_mod;
    AstNode *expr;

    TldResolution resolution;
    IrInstruction *value;
};

struct AstNodeIfBoolExpr {
    AstNode *condition;
    AstNode *then_block;
    AstNode *else_node; // null, block node, or other if expr node
};

struct AstNodeTryExpr {
    bool var_is_const;
    Buf *var_symbol;
    bool var_is_ptr;
    AstNode *target_node;
    AstNode *then_node;
    AstNode *else_node;
    Buf *err_symbol;
};

struct AstNodeIfVarExpr {
    AstNodeVariableDeclaration var_decl;
    AstNode *then_block;
    AstNode *else_node; // null, block node, or other if expr node
    bool var_is_ptr;
};

struct AstNodeWhileExpr {
    AstNode *condition;
    AstNode *continue_expr;
    AstNode *body;
    bool is_inline;
};

struct AstNodeForExpr {
    AstNode *array_expr;
    AstNode *elem_node; // always a symbol
    AstNode *index_node; // always a symbol, might be null
    AstNode *body;
    bool elem_is_ptr;
    bool is_inline;
};

struct AstNodeSwitchExpr {
    AstNode *expr;
    ZigList<AstNode *> prongs;
};

struct AstNodeSwitchProng {
    ZigList<AstNode *> items;
    AstNode *var_symbol;
    AstNode *expr;
    bool var_is_ptr;
    bool any_items_are_range;
};

struct AstNodeSwitchRange {
    AstNode *start;
    AstNode *end;
};

struct AstNodeLabel {
    Buf *name;
};

struct AstNodeGoto {
    Buf *name;
    bool is_inline;
};

struct AstNodeCompTime {
    AstNode *expr;
};

struct AsmOutput {
    Buf *asm_symbolic_name;
    Buf *constraint;
    Buf *variable_name;
    AstNode *return_type; // null unless "=r" and return
};

struct AsmInput {
    Buf *asm_symbolic_name;
    Buf *constraint;
    AstNode *expr;
};

struct SrcPos {
    size_t line;
    size_t column;
};

enum AsmTokenId {
    AsmTokenIdTemplate,
    AsmTokenIdPercent,
    AsmTokenIdVar,
    AsmTokenIdUniqueId,
};

struct AsmToken {
    enum AsmTokenId id;
    size_t start;
    size_t end;
};

struct AstNodeAsmExpr {
    bool is_volatile;
    Buf *asm_template;
    ZigList<AsmToken> token_list;
    ZigList<AsmOutput*> output_list;
    ZigList<AsmInput*> input_list;
    ZigList<Buf*> clobber_list;
};

enum ContainerKind {
    ContainerKindStruct,
    ContainerKindEnum,
    ContainerKindUnion,
};

enum ContainerLayout {
    ContainerLayoutAuto,
    ContainerLayoutExtern,
    ContainerLayoutPacked,
};

struct AstNodeContainerDecl {
    ContainerKind kind;
    ZigList<AstNode *> fields;
    ZigList<AstNode *> decls;
    ContainerLayout layout;
};

struct AstNodeStructField {
    VisibMod visib_mod;
    Buf *name;
    AstNode *type;
};

struct AstNodeStringLiteral {
    Buf *buf;
    bool c;
};

struct AstNodeCharLiteral {
    uint8_t value;
};

struct AstNodeNumberLiteral {
    BigNum *bignum;

    // overflow is true if when parsing the number, we discovered it would not
    // fit without losing data in a uint64_t or double
    bool overflow;
};

struct AstNodeStructValueField {
    Buf *name;
    AstNode *expr;
};

enum ContainerInitKind {
    ContainerInitKindStruct,
    ContainerInitKindArray,
};

struct AstNodeContainerInitExpr {
    AstNode *type;
    ZigList<AstNode *> entries;
    ContainerInitKind kind;
};

struct AstNodeNullLiteral {
};

struct AstNodeUndefinedLiteral {
};

struct AstNodeThisLiteral {
};

struct AstNodeSymbolExpr {
    Buf *symbol;
};

struct AstNodeBoolLiteral {
    bool value;
};

struct AstNodeBreakExpr {
};

struct AstNodeContinueExpr {
};
struct AstNodeUnreachableExpr {
};


struct AstNodeArrayType {
    AstNode *size;
    AstNode *child_type;
    bool is_const;
};

struct AstNodeErrorType {
};

struct AstNodeVarLiteral {
};

struct AstNodeInlineExpr {
    AstNode *body;
};

struct AstNode {
    enum NodeType type;
    size_t line;
    size_t column;
    uint32_t create_index; // for determinism purposes
    ImportTableEntry *owner;
    union {
        AstNodeRoot root;
        AstNodeFnDef fn_def;
        AstNodeFnDecl fn_decl;
        AstNodeFnProto fn_proto;
        AstNodeParamDecl param_decl;
        AstNodeBlock block;
        AstNode * grouped_expr;
        AstNodeReturnExpr return_expr;
        AstNodeDefer defer;
        AstNodeVariableDeclaration variable_declaration;
        AstNodeErrorValueDecl error_value_decl;
        AstNodeTestDecl test_decl;
        AstNodeBinOpExpr bin_op_expr;
        AstNodeUnwrapErrorExpr unwrap_err_expr;
        AstNodePrefixOpExpr prefix_op_expr;
        AstNodeFnCallExpr fn_call_expr;
        AstNodeArrayAccessExpr array_access_expr;
        AstNodeSliceExpr slice_expr;
        AstNodeUse use;
        AstNodeIfBoolExpr if_bool_expr;
        AstNodeIfVarExpr if_var_expr;
        AstNodeTryExpr try_expr;
        AstNodeWhileExpr while_expr;
        AstNodeForExpr for_expr;
        AstNodeSwitchExpr switch_expr;
        AstNodeSwitchProng switch_prong;
        AstNodeSwitchRange switch_range;
        AstNodeLabel label;
        AstNodeGoto goto_expr;
        AstNodeCompTime comptime_expr;
        AstNodeAsmExpr asm_expr;
        AstNodeFieldAccessExpr field_access_expr;
        AstNodeContainerDecl container_decl;
        AstNodeStructField struct_field;
        AstNodeStringLiteral string_literal;
        AstNodeCharLiteral char_literal;
        AstNodeNumberLiteral number_literal;
        AstNodeContainerInitExpr container_init_expr;
        AstNodeStructValueField struct_val_field;
        AstNodeNullLiteral null_literal;
        AstNodeUndefinedLiteral undefined_literal;
        AstNodeThisLiteral this_literal;
        AstNodeSymbolExpr symbol_expr;
        AstNodeBoolLiteral bool_literal;
        AstNodeBreakExpr break_expr;
        AstNodeContinueExpr continue_expr;
        AstNodeUnreachableExpr unreachable_expr;
        AstNodeArrayType array_type;
        AstNodeErrorType error_type;
        AstNodeVarLiteral var_literal;
        AstNodeInlineExpr inline_expr;
    } data;
};

// this struct is allocated with allocate_nonzero
struct FnTypeParamInfo {
    bool is_noalias;
    TypeTableEntry *type;
};

struct GenericFnTypeId {
    FnTableEntry *fn_entry;
    ConstExprValue *params;
    size_t param_count;
};

uint32_t generic_fn_type_id_hash(GenericFnTypeId *id);
bool generic_fn_type_id_eql(GenericFnTypeId *a, GenericFnTypeId *b);

struct FnTypeId {
    TypeTableEntry *return_type;
    FnTypeParamInfo *param_info;
    size_t param_count;
    size_t next_param_index;
    bool is_var_args;
    bool is_naked;
    bool is_cold;
    bool is_extern;
};

uint32_t fn_type_id_hash(FnTypeId*);
bool fn_type_id_eql(FnTypeId *a, FnTypeId *b);

struct TypeTableEntryPointer {
    TypeTableEntry *child_type;
    bool is_const;
    bool is_volatile;
    uint32_t bit_offset;
    uint32_t unaligned_bit_count;
};

struct TypeTableEntryInt {
    uint32_t bit_count;
    bool is_signed;
};

struct TypeTableEntryFloat {
    size_t bit_count;
};

struct TypeTableEntryArray {
    TypeTableEntry *child_type;
    uint64_t len;
};

struct TypeStructField {
    Buf *name;
    TypeTableEntry *type_entry;
    size_t src_index;
    size_t gen_index;
    // offset from the memory at gen_index
    size_t packed_bits_offset;
    size_t packed_bits_size;
    size_t unaligned_bit_count;
};
struct TypeTableEntryStruct {
    AstNode *decl_node;
    ContainerLayout layout;
    uint32_t src_field_count;
    uint32_t gen_field_count;
    TypeStructField *fields;
    uint64_t size_bytes;
    bool is_invalid; // true if any fields are invalid
    bool is_slice;
    ScopeDecls *decls_scope;

    // set this flag temporarily to detect infinite loops
    bool embedded_in_current;
    bool reported_infinite_err;
    // whether we've finished resolving it
    bool complete;

    bool zero_bits_loop_flag;
    bool zero_bits_known;
};

struct TypeTableEntryMaybe {
    TypeTableEntry *child_type;
};

struct TypeTableEntryError {
    TypeTableEntry *child_type;
};

struct TypeTableEntryEnum {
    AstNode *decl_node;
    ContainerLayout layout;
    uint32_t src_field_count;
    // number of fields in the union. 0 if enum with no payload
    uint32_t gen_field_count;
    TypeEnumField *fields;
    bool is_invalid; // true if any fields are invalid
    TypeTableEntry *tag_type;
    TypeTableEntry *union_type;

    ScopeDecls *decls_scope;

    // set this flag temporarily to detect infinite loops
    bool embedded_in_current;
    bool reported_infinite_err;
    // whether we've finished resolving it
    bool complete;

    bool zero_bits_loop_flag;
    bool zero_bits_known;
};

struct TypeTableEntryEnumTag {
    TypeTableEntry *enum_type;
    TypeTableEntry *int_type;
    bool generate_name_table;
    LLVMValueRef name_table;
};

struct TypeTableEntryUnion {
    AstNode *decl_node;
    ContainerLayout layout;
    uint32_t src_field_count;
    uint32_t gen_field_count;
    TypeStructField *fields;
    uint64_t size_bytes;
    bool is_invalid; // true if any fields are invalid
    ScopeDecls *decls_scope;

    // set this flag temporarily to detect infinite loops
    bool embedded_in_current;
    bool reported_infinite_err;
    // whether we've finished resolving it
    bool complete;

    bool zero_bits_loop_flag;
    bool zero_bits_known;
};

struct FnGenParamInfo {
    size_t src_index;
    size_t gen_index;
    bool is_byval;
    TypeTableEntry *type;
};

struct TypeTableEntryFn {
    FnTypeId fn_type_id;
    bool is_generic;
    TypeTableEntry *gen_return_type;
    size_t gen_param_count;
    FnGenParamInfo *gen_param_info;

    LLVMTypeRef raw_type_ref;
    LLVMCallConv calling_convention;

    TypeTableEntry *bound_fn_parent;
};

struct TypeTableEntryBoundFn {
    TypeTableEntry *fn_type;
};

enum TypeTableEntryId {
    TypeTableEntryIdInvalid,
    TypeTableEntryIdVar,
    TypeTableEntryIdMetaType,
    TypeTableEntryIdVoid,
    TypeTableEntryIdBool,
    TypeTableEntryIdUnreachable,
    TypeTableEntryIdInt,
    TypeTableEntryIdFloat,
    TypeTableEntryIdPointer,
    TypeTableEntryIdArray,
    TypeTableEntryIdStruct,
    TypeTableEntryIdNumLitFloat,
    TypeTableEntryIdNumLitInt,
    TypeTableEntryIdUndefLit,
    TypeTableEntryIdNullLit,
    TypeTableEntryIdMaybe,
    TypeTableEntryIdErrorUnion,
    TypeTableEntryIdPureError,
    TypeTableEntryIdEnum,
    TypeTableEntryIdEnumTag,
    TypeTableEntryIdUnion,
    TypeTableEntryIdFn,
    TypeTableEntryIdNamespace,
    TypeTableEntryIdBlock,
    TypeTableEntryIdBoundFn,
    TypeTableEntryIdArgTuple,
    TypeTableEntryIdOpaque,
};

struct TypeTableEntry {
    TypeTableEntryId id;
    Buf name;

    LLVMTypeRef type_ref;
    ZigLLVMDIType *di_type;

    bool zero_bits;
    bool is_copyable;

    union {
        TypeTableEntryPointer pointer;
        TypeTableEntryInt integral;
        TypeTableEntryFloat floating;
        TypeTableEntryArray array;
        TypeTableEntryStruct structure;
        TypeTableEntryMaybe maybe;
        TypeTableEntryError error;
        TypeTableEntryEnum enumeration;
        TypeTableEntryEnumTag enum_tag;
        TypeTableEntryUnion unionation;
        TypeTableEntryFn fn;
        TypeTableEntryBoundFn bound_fn;
    } data;

    // use these fields to make sure we don't duplicate type table entries for the same type
    TypeTableEntry *pointer_parent[2]; // [0 - mut, 1 - const]
    TypeTableEntry *slice_parent[2]; // [0 - mut, 1 - const]
    TypeTableEntry *maybe_parent;
    TypeTableEntry *error_parent;
    // If we generate a constant name value for this type, we memoize it here.
    // The type of this is array
    ConstExprValue *cached_const_name_val;
};

struct PackageTableEntry {
    Buf root_src_dir;
    Buf root_src_path; // relative to root_src_dir

    // reminder: hash tables must be initialized before use
    HashMap<Buf *, PackageTableEntry *, buf_hash, buf_eql_buf> package_table;
};

struct ImportTableEntry {
    AstNode *root;
    Buf *path; // relative to root_package->root_src_dir
    PackageTableEntry *package;
    ZigLLVMDIFile *di_file;
    Buf *source_code;
    ZigList<size_t> *line_offsets;
    ScopeDecls *decls_scope;
    AstNode *c_import_node;
    bool any_imports_failed;

    ZigList<AstNode *> use_decls;
};

enum FnAnalState {
    FnAnalStateReady,
    FnAnalStateProbing,
    FnAnalStateComplete,
    FnAnalStateInvalid,
};

enum FnInline {
    FnInlineAuto,
    FnInlineAlways,
    FnInlineNever,
};

struct FnTableEntry {
    LLVMValueRef llvm_value;
    AstNode *proto_node;
    AstNode *body_node;
    ScopeFnDef *fndef_scope; // parent should be the top level decls or container decls
    Scope *child_scope; // parent is scope for last parameter
    ScopeBlock *def_scope; // parent is child_scope
    Buf symbol_name;
    TypeTableEntry *type_entry; // function type
    TypeTableEntry *implicit_return_type;
    bool is_test;
    FnInline fn_inline;
    FnAnalState anal_state;
    IrExecutable ir_executable;
    IrExecutable analyzed_executable;
    size_t prealloc_bbc;
    AstNode **param_source_nodes;
    Buf **param_names;

    AstNode *fn_no_inline_set_node;
    AstNode *fn_static_eval_set_node;

    ZigList<IrInstruction *> alloca_list;
    ZigList<VariableTableEntry *> variable_list;

    AstNode *set_global_align_node;
    uint32_t alignment;
    AstNode *set_global_section_node;
    Buf *section_name;
    AstNode *set_global_linkage_node;
    GlobalLinkageId linkage;
};

uint32_t fn_table_entry_hash(FnTableEntry*);
bool fn_table_entry_eql(FnTableEntry *a, FnTableEntry *b);

enum BuiltinFnId {
    BuiltinFnIdInvalid,
    BuiltinFnIdMemcpy,
    BuiltinFnIdMemset,
    BuiltinFnIdSizeof,
    BuiltinFnIdAlignof,
    BuiltinFnIdMaxValue,
    BuiltinFnIdMinValue,
    BuiltinFnIdMemberCount,
    BuiltinFnIdTypeof,
    BuiltinFnIdAddWithOverflow,
    BuiltinFnIdSubWithOverflow,
    BuiltinFnIdMulWithOverflow,
    BuiltinFnIdShlWithOverflow,
    BuiltinFnIdCInclude,
    BuiltinFnIdCDefine,
    BuiltinFnIdCUndef,
    BuiltinFnIdCompileVar,
    BuiltinFnIdCompileErr,
    BuiltinFnIdCompileLog,
    BuiltinFnIdGeneratedCode,
    BuiltinFnIdCtz,
    BuiltinFnIdClz,
    BuiltinFnIdImport,
    BuiltinFnIdCImport,
    BuiltinFnIdErrName,
    BuiltinFnIdBreakpoint,
    BuiltinFnIdReturnAddress,
    BuiltinFnIdFrameAddress,
    BuiltinFnIdEmbedFile,
    BuiltinFnIdCmpExchange,
    BuiltinFnIdFence,
    BuiltinFnIdDivExact,
    BuiltinFnIdTruncate,
    BuiltinFnIdIntType,
    BuiltinFnIdSetDebugSafety,
    BuiltinFnIdTypeName,
    BuiltinFnIdIsInteger,
    BuiltinFnIdIsFloat,
    BuiltinFnIdCanImplicitCast,
    BuiltinFnIdSetGlobalAlign,
    BuiltinFnIdSetGlobalSection,
    BuiltinFnIdSetGlobalLinkage,
    BuiltinFnIdPanic,
    BuiltinFnIdPtrCast,
    BuiltinFnIdIntToPtr,
    BuiltinFnIdEnumTagName,
    BuiltinFnIdFieldParentPtr,
    BuiltinFnIdOffsetOf,
};

struct BuiltinFnEntry {
    BuiltinFnId id;
    Buf name;
    size_t param_count;
    uint32_t ref_count;
    LLVMValueRef fn_val;
};

enum PanicMsgId {
    PanicMsgIdUnreachable,
    PanicMsgIdBoundsCheckFailure,
    PanicMsgIdCastNegativeToUnsigned,
    PanicMsgIdCastTruncatedData,
    PanicMsgIdIntegerOverflow,
    PanicMsgIdShiftOverflowedBits,
    PanicMsgIdDivisionByZero,
    PanicMsgIdRemainderDivisionByZero,
    PanicMsgIdExactDivisionRemainder,
    PanicMsgIdSliceWidenRemainder,
    PanicMsgIdUnwrapMaybeFail,
    PanicMsgIdInvalidErrorCode,

    PanicMsgIdCount,
};

uint32_t fn_eval_hash(Scope*);
bool fn_eval_eql(Scope *a, Scope *b);

struct TypeId {
    TypeTableEntryId id;

    union {
        struct {
            TypeTableEntry *child_type;
            bool is_const;
            bool is_volatile;
            uint32_t bit_offset;
            uint32_t unaligned_bit_count;
        } pointer;
        struct {
            TypeTableEntry *child_type;
            uint64_t size;
        } array;
        struct {
            bool is_signed;
            uint32_t bit_count;
        } integer;
    } data;
};

uint32_t type_id_hash(TypeId);
bool type_id_eql(TypeId a, TypeId b);

enum ZigLLVMFnId {
    ZigLLVMFnIdCtz,
    ZigLLVMFnIdClz,
    ZigLLVMFnIdOverflowArithmetic,
};

enum AddSubMul {
    AddSubMulAdd = 0,
    AddSubMulSub = 1,
    AddSubMulMul = 2,
};

struct ZigLLVMFnKey {
    ZigLLVMFnId id;

    union {
        struct {
            uint32_t bit_count;
        } ctz;
        struct {
            uint32_t bit_count;
        } clz;
        struct {
            AddSubMul add_sub_mul;
            uint32_t bit_count;
            bool is_signed;
        } overflow_arithmetic;
    } data;
};

uint32_t zig_llvm_fn_key_hash(ZigLLVMFnKey);
bool zig_llvm_fn_key_eql(ZigLLVMFnKey a, ZigLLVMFnKey b);

struct CodeGen {
    LLVMModuleRef module;
    ZigList<ErrorMsg*> errors;
    LLVMBuilderRef builder;
    ZigLLVMDIBuilder *dbuilder;
    ZigLLVMDICompileUnit *compile_unit;

    ZigList<Buf *> link_libs; // non-libc link libs
    // add -framework [name] args to linker
    ZigList<Buf *> darwin_frameworks;
    // add -rpath [name] args to linker
    ZigList<Buf *> rpath_list;


    // reminder: hash tables must be initialized before use
    HashMap<Buf *, ImportTableEntry *, buf_hash, buf_eql_buf> import_table;
    HashMap<Buf *, BuiltinFnEntry *, buf_hash, buf_eql_buf> builtin_fn_table;
    HashMap<Buf *, TypeTableEntry *, buf_hash, buf_eql_buf> primitive_type_table;
    HashMap<TypeId, TypeTableEntry *, type_id_hash, type_id_eql> type_table;
    HashMap<FnTypeId *, TypeTableEntry *, fn_type_id_hash, fn_type_id_eql> fn_type_table;
    HashMap<Buf *, ErrorTableEntry *, buf_hash, buf_eql_buf> error_table;
    HashMap<GenericFnTypeId *, FnTableEntry *, generic_fn_type_id_hash, generic_fn_type_id_eql> generic_table;
    HashMap<Scope *, IrInstruction *, fn_eval_hash, fn_eval_eql> memoized_fn_eval_table;
    HashMap<ZigLLVMFnKey, LLVMValueRef, zig_llvm_fn_key_hash, zig_llvm_fn_key_eql> llvm_fn_table;
    HashMap<Buf *, ConstExprValue *, buf_hash, buf_eql_buf> compile_vars;
    HashMap<Buf *, Tld *, buf_hash, buf_eql_buf> exported_symbol_names;
    HashMap<Buf *, Tld *, buf_hash, buf_eql_buf> external_prototypes;

    ZigList<ImportTableEntry *> import_queue;
    size_t import_queue_index;
    ZigList<Tld *> resolve_queue;
    size_t resolve_queue_index;
    ZigList<AstNode *> use_queue;
    size_t use_queue_index;

    uint32_t next_unresolved_index;

    struct {
        TypeTableEntry *entry_bool;
        TypeTableEntry *entry_int[2][4]; // [signed,unsigned][8,16,32,64]
        TypeTableEntry *entry_c_int[CIntTypeCount];
        TypeTableEntry *entry_c_long_double;
        TypeTableEntry *entry_c_void;
        TypeTableEntry *entry_u8;
        TypeTableEntry *entry_u16;
        TypeTableEntry *entry_u32;
        TypeTableEntry *entry_u64;
        TypeTableEntry *entry_i8;
        TypeTableEntry *entry_i16;
        TypeTableEntry *entry_i32;
        TypeTableEntry *entry_i64;
        TypeTableEntry *entry_isize;
        TypeTableEntry *entry_usize;
        TypeTableEntry *entry_f32;
        TypeTableEntry *entry_f64;
        TypeTableEntry *entry_void;
        TypeTableEntry *entry_unreachable;
        TypeTableEntry *entry_type;
        TypeTableEntry *entry_invalid;
        TypeTableEntry *entry_namespace;
        TypeTableEntry *entry_block;
        TypeTableEntry *entry_num_lit_int;
        TypeTableEntry *entry_num_lit_float;
        TypeTableEntry *entry_undef;
        TypeTableEntry *entry_null;
        TypeTableEntry *entry_var;
        TypeTableEntry *entry_pure_error;
        TypeTableEntry *entry_os_enum;
        TypeTableEntry *entry_arch_enum;
        TypeTableEntry *entry_environ_enum;
        TypeTableEntry *entry_oformat_enum;
        TypeTableEntry *entry_atomic_order_enum;
        TypeTableEntry *entry_global_linkage_enum;
        TypeTableEntry *entry_arg_tuple;
    } builtin_types;

    ZigTarget zig_target;
    LLVMTargetDataRef target_data_ref;
    unsigned pointer_size_bytes;
    bool is_big_endian;
    bool is_static;
    bool strip_debug_symbols;
    bool want_h_file;
    bool have_pub_main;
    bool have_c_main;
    bool have_pub_panic;
    bool link_libc;
    Buf *libc_lib_dir;
    Buf *libc_static_lib_dir;
    Buf *libc_include_dir;
    Buf *zig_std_dir;
    Buf *zig_std_special_dir;
    Buf *dynamic_linker;
    Buf *ar_path;
    Buf triple_str;
    bool is_release_build;
    bool is_test_build;
    uint32_t target_os_index;
    uint32_t target_arch_index;
    uint32_t target_environ_index;
    uint32_t target_oformat_index;
    LLVMTargetMachineRef target_machine;
    ZigLLVMDIFile *dummy_di_file;
    bool is_native_target;
    PackageTableEntry *root_package;
    PackageTableEntry *std_package;
    PackageTableEntry *zigrt_package;
    Buf *root_out_name;
    bool windows_subsystem_windows;
    bool windows_subsystem_console;
    bool windows_linker_unicode;
    Buf *darwin_linker_version;
    Buf *mmacosx_version_min;
    Buf *mios_version_min;
    bool linker_rdynamic;
    const char *linker_script;
    bool omit_zigrt;

    // The function definitions this module includes. There must be a corresponding
    // fn_protos entry.
    ZigList<FnTableEntry *> fn_defs;
    size_t fn_defs_index;
    // The function prototypes this module includes. In the case of external declarations,
    // there will not be a corresponding fn_defs entry.
    ZigList<FnTableEntry *> fn_protos;
    ZigList<TldVar *> global_vars;

    OutType out_type;
    FnTableEntry *cur_fn;
    FnTableEntry *main_fn;
    FnTableEntry *user_panic_fn;
    FnTableEntry *extern_panic_fn;
    LLVMValueRef cur_ret_ptr;
    LLVMValueRef cur_fn_val;
    ZigList<LLVMBasicBlockRef> break_block_stack;
    ZigList<LLVMBasicBlockRef> continue_block_stack;
    bool c_want_stdint;
    bool c_want_stdbool;
    AstNode *root_export_decl;
    size_t version_major;
    size_t version_minor;
    size_t version_patch;
    bool verbose;
    ErrColor err_color;
    ImportTableEntry *root_import;
    ImportTableEntry *bootstrap_import;
    ImportTableEntry *test_runner_import;
    LLVMValueRef memcpy_fn_val;
    LLVMValueRef memset_fn_val;
    LLVMValueRef trap_fn_val;
    LLVMValueRef return_address_fn_val;
    LLVMValueRef frame_address_fn_val;
    bool error_during_imports;
    uint32_t next_node_index;
    TypeTableEntry *err_tag_type;

    const char **clang_argv;
    size_t clang_argv_len;
    ZigList<const char *> lib_dirs;

    uint32_t test_fn_count;
    TypeTableEntry *test_fn_type;

    bool each_lib_rpath;

    ZigList<AstNode *> error_decls;
    bool generate_error_name_table;
    LLVMValueRef err_name_table;
    size_t largest_err_name_len;
    LLVMValueRef safety_crash_err_fn;

    IrInstruction *invalid_instruction;
    ConstExprValue const_void_val;

    ConstExprValue panic_msg_vals[PanicMsgIdCount];

    Buf global_asm;
    ZigList<Buf *> link_objects;

    ZigList<TypeTableEntry *> name_table_enums;

    Buf *test_filter;
    Buf *test_name_prefix;
};

enum VarLinkage {
    VarLinkageInternal,
    VarLinkageExport,
    VarLinkageExternal,
};

struct VariableTableEntry {
    Buf name;
    ConstExprValue *value;
    LLVMValueRef value_ref;
    bool src_is_const;
    bool gen_is_const;
    IrInstruction *is_comptime;
    // which node is the declaration of the variable
    AstNode *decl_node;
    ZigLLVMDILocalVariable *di_loc_var;
    size_t src_arg_index;
    size_t gen_arg_index;
    Scope *parent_scope;
    Scope *child_scope;
    LLVMValueRef param_value_ref;
    bool shadowable;
    size_t mem_slot_index;
    size_t ref_count;
    VarLinkage linkage;
};

struct ErrorTableEntry {
    Buf name;
    uint32_t value;
    AstNode *decl_node;
    // If we generate a constant error name value for this error, we memoize it here.
    // The type of this is array
    ConstExprValue *cached_error_name_val;
};

struct LabelTableEntry {
    AstNode *decl_node;
    IrBasicBlock *bb;
    bool used;
};

enum ScopeId {
    ScopeIdDecls,
    ScopeIdBlock,
    ScopeIdDefer,
    ScopeIdDeferExpr,
    ScopeIdVarDecl,
    ScopeIdCImport,
    ScopeIdLoop,
    ScopeIdFnDef,
    ScopeIdCompTime,
};

struct Scope {
    ScopeId id;
    AstNode *source_node;

    // if the scope has a parent, this is it
    Scope *parent;

    ZigLLVMDIScope *di_scope;
};

// This scope comes from global declarations or from
// declarations in a container declaration
// NodeTypeRoot, NodeTypeContainerDecl
struct ScopeDecls {
    Scope base;

    HashMap<Buf *, Tld *, buf_hash, buf_eql_buf> decl_table;
    bool safety_off;
    AstNode *safety_set_node;
    ImportTableEntry *import;
    // If this is a scope from a container, this is the type entry, otherwise null
    TypeTableEntry *container_type;
};

// This scope comes from a block expression in user code.
// NodeTypeBlock
struct ScopeBlock {
    Scope base;

    HashMap<Buf *, LabelTableEntry *, buf_hash, buf_eql_buf> label_table; 
    bool safety_off;
    AstNode *safety_set_node;
};

// This scope is created from every defer expression.
// It's the code following the defer statement.
// NodeTypeDefer
struct ScopeDefer {
    Scope base;
};

// This scope is created from every defer expression.
// It's the parent of the defer expression itself.
// NodeTypeDefer
struct ScopeDeferExpr {
    Scope base;

    bool reported_err;
};

// This scope is created for every variable declaration inside an IrExecutable
// NodeTypeVariableDeclaration, NodeTypeParamDecl
struct ScopeVarDecl {
    Scope base;

    // The variable that creates this scope
    VariableTableEntry *var;
};

// This scope is created for a @cImport
// NodeTypeFnCallExpr
struct ScopeCImport {
    Scope base;

    Buf buf;
};

// This scope is created for a loop such as for or while in order to
// make break and continue statements work.
// NodeTypeForExpr or NodeTypeWhileExpr
// TODO I think we can get rid of this
struct ScopeLoop {
    Scope base;
};

// This scope is created for a comptime expression.
// NodeTypeCompTime, NodeTypeSwitchExpr
struct ScopeCompTime {
    Scope base;
};


// This scope is created for a function definition.
// NodeTypeFnDef
struct ScopeFnDef {
    Scope base;

    FnTableEntry *fn_entry;
};

enum AtomicOrder {
    AtomicOrderUnordered,
    AtomicOrderMonotonic,
    AtomicOrderAcquire,
    AtomicOrderRelease,
    AtomicOrderAcqRel,
    AtomicOrderSeqCst,
};

// A basic block contains no branching. Branches send control flow
// to another basic block.
// Phi instructions must be first in a basic block.
// The last instruction in a basic block must be of type unreachable.
struct IrBasicBlock {
    ZigList<IrInstruction *> instruction_list;
    IrBasicBlock *other;
    Scope *scope;
    const char *name_hint;
    size_t debug_id;
    size_t ref_count;
    LLVMBasicBlockRef llvm_block;
    LLVMBasicBlockRef llvm_exit_block;
    // The instruction that referenced this basic block and caused us to
    // analyze the basic block. If the same instruction wants us to emit
    // the same basic block, then we re-generate it instead of saving it.
    IrInstruction *ref_instruction;
    // When this is non-null, a branch to this basic block is only allowed
    // if the branch is comptime. The instruction points to the reason
    // the basic block must be comptime.
    IrInstruction *must_be_comptime_source_instr;
};

struct LVal {
    bool is_ptr;
    bool is_const;
    bool is_volatile;
};

enum IrInstructionId {
    IrInstructionIdInvalid,
    IrInstructionIdBr,
    IrInstructionIdCondBr,
    IrInstructionIdSwitchBr,
    IrInstructionIdSwitchVar,
    IrInstructionIdSwitchTarget,
    IrInstructionIdPhi,
    IrInstructionIdUnOp,
    IrInstructionIdBinOp,
    IrInstructionIdDeclVar,
    IrInstructionIdLoadPtr,
    IrInstructionIdStorePtr,
    IrInstructionIdFieldPtr,
    IrInstructionIdStructFieldPtr,
    IrInstructionIdEnumFieldPtr,
    IrInstructionIdElemPtr,
    IrInstructionIdVarPtr,
    IrInstructionIdCall,
    IrInstructionIdConst,
    IrInstructionIdReturn,
    IrInstructionIdCast,
    IrInstructionIdContainerInitList,
    IrInstructionIdContainerInitFields,
    IrInstructionIdStructInit,
    IrInstructionIdUnreachable,
    IrInstructionIdTypeOf,
    IrInstructionIdToPtrType,
    IrInstructionIdPtrTypeChild,
    IrInstructionIdSetDebugSafety,
    IrInstructionIdArrayType,
    IrInstructionIdSliceType,
    IrInstructionIdAsm,
    IrInstructionIdCompileVar,
    IrInstructionIdSizeOf,
    IrInstructionIdTestNonNull,
    IrInstructionIdUnwrapMaybe,
    IrInstructionIdMaybeWrap,
    IrInstructionIdEnumTag,
    IrInstructionIdClz,
    IrInstructionIdCtz,
    IrInstructionIdGeneratedCode,
    IrInstructionIdImport,
    IrInstructionIdCImport,
    IrInstructionIdCInclude,
    IrInstructionIdCDefine,
    IrInstructionIdCUndef,
    IrInstructionIdArrayLen,
    IrInstructionIdRef,
    IrInstructionIdMinValue,
    IrInstructionIdMaxValue,
    IrInstructionIdCompileErr,
    IrInstructionIdCompileLog,
    IrInstructionIdErrName,
    IrInstructionIdEmbedFile,
    IrInstructionIdCmpxchg,
    IrInstructionIdFence,
    IrInstructionIdDivExact,
    IrInstructionIdTruncate,
    IrInstructionIdIntType,
    IrInstructionIdBoolNot,
    IrInstructionIdMemset,
    IrInstructionIdMemcpy,
    IrInstructionIdSlice,
    IrInstructionIdMemberCount,
    IrInstructionIdBreakpoint,
    IrInstructionIdReturnAddress,
    IrInstructionIdFrameAddress,
    IrInstructionIdAlignOf,
    IrInstructionIdOverflowOp,
    IrInstructionIdTestErr,
    IrInstructionIdUnwrapErrCode,
    IrInstructionIdUnwrapErrPayload,
    IrInstructionIdErrWrapCode,
    IrInstructionIdErrWrapPayload,
    IrInstructionIdFnProto,
    IrInstructionIdTestComptime,
    IrInstructionIdInitEnum,
    IrInstructionIdPtrCast,
    IrInstructionIdWidenOrShorten,
    IrInstructionIdIntToPtr,
    IrInstructionIdPtrToInt,
    IrInstructionIdIntToEnum,
    IrInstructionIdIntToErr,
    IrInstructionIdErrToInt,
    IrInstructionIdCheckSwitchProngs,
    IrInstructionIdTestType,
    IrInstructionIdTypeName,
    IrInstructionIdCanImplicitCast,
    IrInstructionIdSetGlobalAlign,
    IrInstructionIdSetGlobalSection,
    IrInstructionIdSetGlobalLinkage,
    IrInstructionIdDeclRef,
    IrInstructionIdPanic,
    IrInstructionIdEnumTagName,
    IrInstructionIdSetFnRefInline,
    IrInstructionIdFieldParentPtr,
    IrInstructionIdOffsetOf,
};

struct IrInstruction {
    IrInstructionId id;
    Scope *scope;
    AstNode *source_node;
    ConstExprValue value;
    size_t debug_id;
    LLVMValueRef llvm_value;
    // if ref_count is zero and the instruction has no side effects,
    // the instruction can be omitted in codegen
    size_t ref_count;
    IrInstruction *other;
    IrBasicBlock *owner_bb;
    // true if this instruction was generated by zig and not from user code
    bool is_gen;
};

struct IrInstructionCondBr {
    IrInstruction base;

    IrInstruction *condition;
    IrBasicBlock *then_block;
    IrBasicBlock *else_block;
    IrInstruction *is_comptime;
};

struct IrInstructionBr {
    IrInstruction base;

    IrBasicBlock *dest_block;
    IrInstruction *is_comptime;
};

struct IrInstructionSwitchBrCase {
    IrInstruction *value;
    IrBasicBlock *block;
};

struct IrInstructionSwitchBr {
    IrInstruction base;

    IrInstruction *target_value;
    IrBasicBlock *else_block;
    size_t case_count;
    IrInstructionSwitchBrCase *cases;
    IrInstruction *is_comptime;
};

struct IrInstructionSwitchVar {
    IrInstruction base;

    IrInstruction *target_value_ptr;
    IrInstruction *prong_value;
};

struct IrInstructionSwitchTarget {
    IrInstruction base;

    IrInstruction *target_value_ptr;
};

struct IrInstructionPhi {
    IrInstruction base;

    size_t incoming_count;
    IrBasicBlock **incoming_blocks;
    IrInstruction **incoming_values;
};

enum IrUnOp {
    IrUnOpInvalid,
    IrUnOpBinNot,
    IrUnOpNegation,
    IrUnOpNegationWrap,
    IrUnOpDereference,
    IrUnOpError,
    IrUnOpMaybe,
};

struct IrInstructionUnOp {
    IrInstruction base;

    IrUnOp op_id;
    IrInstruction *value;
};

enum IrBinOp {
    IrBinOpInvalid,
    IrBinOpBoolOr,
    IrBinOpBoolAnd,
    IrBinOpCmpEq,
    IrBinOpCmpNotEq,
    IrBinOpCmpLessThan,
    IrBinOpCmpGreaterThan,
    IrBinOpCmpLessOrEq,
    IrBinOpCmpGreaterOrEq,
    IrBinOpBinOr,
    IrBinOpBinXor,
    IrBinOpBinAnd,
    IrBinOpBitShiftLeft,
    IrBinOpBitShiftLeftWrap,
    IrBinOpBitShiftRight,
    IrBinOpAdd,
    IrBinOpAddWrap,
    IrBinOpSub,
    IrBinOpSubWrap,
    IrBinOpMult,
    IrBinOpMultWrap,
    IrBinOpDiv,
    IrBinOpRem,
    IrBinOpArrayCat,
    IrBinOpArrayMult,
};

struct IrInstructionBinOp {
    IrInstruction base;

    IrInstruction *op1;
    IrBinOp op_id;
    IrInstruction *op2;
    bool safety_check_on;
};

struct IrInstructionDeclVar {
    IrInstruction base;

    VariableTableEntry *var;
    IrInstruction *var_type;
    IrInstruction *init_value;
};

struct IrInstructionLoadPtr {
    IrInstruction base;

    IrInstruction *ptr;
};

struct IrInstructionStorePtr {
    IrInstruction base;

    IrInstruction *ptr;
    IrInstruction *value;
};

struct IrInstructionFieldPtr {
    IrInstruction base;

    IrInstruction *container_ptr;
    Buf *field_name;
    bool is_const;
};

struct IrInstructionStructFieldPtr {
    IrInstruction base;

    IrInstruction *struct_ptr;
    TypeStructField *field;
    bool is_const;
};

struct IrInstructionEnumFieldPtr {
    IrInstruction base;

    IrInstruction *enum_ptr;
    TypeEnumField *field;
    bool is_const;
};

struct IrInstructionElemPtr {
    IrInstruction base;

    IrInstruction *array_ptr;
    IrInstruction *elem_index;
    bool is_const;
    bool safety_check_on;
};

struct IrInstructionVarPtr {
    IrInstruction base;

    VariableTableEntry *var;
    bool is_const;
    bool is_volatile;
};

struct IrInstructionCall {
    IrInstruction base;

    IrInstruction *fn_ref;
    FnTableEntry *fn_entry;
    size_t arg_count;
    IrInstruction **args;
    bool is_comptime;
    LLVMValueRef tmp_ptr;
    bool is_inline;
};

struct IrInstructionConst {
    IrInstruction base;
};

// When an IrExecutable is not in a function, a return instruction means that
// the expression returns with that value, even though a return statement from
// an AST perspective is invalid.
struct IrInstructionReturn {
    IrInstruction base;

    IrInstruction *value;
};

// TODO get rid of this instruction, replace with instructions for each op code
struct IrInstructionCast {
    IrInstruction base;

    IrInstruction *value;
    TypeTableEntry *dest_type;
    CastOp cast_op;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionContainerInitList {
    IrInstruction base;

    IrInstruction *container_type;
    size_t item_count;
    IrInstruction **items;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionContainerInitFieldsField {
    Buf *name;
    IrInstruction *value;
    AstNode *source_node;
    TypeStructField *type_struct_field;
};

struct IrInstructionContainerInitFields {
    IrInstruction base;

    IrInstruction *container_type;
    size_t field_count;
    IrInstructionContainerInitFieldsField *fields;
};

struct IrInstructionStructInitField {
    IrInstruction *value;
    TypeStructField *type_struct_field;
};

struct IrInstructionStructInit {
    IrInstruction base;

    TypeTableEntry *struct_type;
    size_t field_count;
    IrInstructionStructInitField *fields;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionUnreachable {
    IrInstruction base;
};

struct IrInstructionTypeOf {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionToPtrType {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionPtrTypeChild {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionSetDebugSafety {
    IrInstruction base;

    IrInstruction *scope_value;
    IrInstruction *debug_safety_on;
};

struct IrInstructionArrayType {
    IrInstruction base;

    IrInstruction *size;
    IrInstruction *child_type;
};

struct IrInstructionSliceType {
    IrInstruction base;

    bool is_const;
    IrInstruction *child_type;
};

struct IrInstructionAsm {
    IrInstruction base;

    // Most information on inline assembly comes from the source node.
    IrInstruction **input_list;
    IrInstruction **output_types;
    VariableTableEntry **output_vars;
    size_t return_count;
    bool has_side_effects;
};

struct IrInstructionCompileVar {
    IrInstruction base;

    IrInstruction *name;
};

struct IrInstructionSizeOf {
    IrInstruction base;

    IrInstruction *type_value;
};

// returns true if nonnull, returns false if null
// this is so that `zeroes` sets maybe values to null
struct IrInstructionTestNonNull {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionUnwrapMaybe {
    IrInstruction base;

    IrInstruction *value;
    bool safety_check_on;
};

struct IrInstructionCtz {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionClz {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionEnumTag {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionGeneratedCode {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionImport {
    IrInstruction base;

    IrInstruction *name;
};

struct IrInstructionArrayLen {
    IrInstruction base;

    IrInstruction *array_value;
};

struct IrInstructionRef {
    IrInstruction base;

    IrInstruction *value;
    LLVMValueRef tmp_ptr;
    bool is_const;
    bool is_volatile;
};

struct IrInstructionMinValue {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionMaxValue {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionCompileErr {
    IrInstruction base;

    IrInstruction *msg;
};

struct IrInstructionCompileLog {
    IrInstruction base;

    size_t msg_count;
    IrInstruction **msg_list;
};

struct IrInstructionErrName {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionCImport {
    IrInstruction base;
};

struct IrInstructionCInclude {
    IrInstruction base;

    IrInstruction *name;
};

struct IrInstructionCDefine {
    IrInstruction base;

    IrInstruction *name;
    IrInstruction *value;
};

struct IrInstructionCUndef {
    IrInstruction base;

    IrInstruction *name;
};

struct IrInstructionEmbedFile {
    IrInstruction base;

    IrInstruction *name;
};

struct IrInstructionCmpxchg {
    IrInstruction base;

    IrInstruction *ptr;
    IrInstruction *cmp_value;
    IrInstruction *new_value;
    IrInstruction *success_order_value;
    IrInstruction *failure_order_value;

    // if this instruction gets to runtime then we know these values:
    AtomicOrder success_order;
    AtomicOrder failure_order;
};

struct IrInstructionFence {
    IrInstruction base;

    IrInstruction *order_value;

    // if this instruction gets to runtime then we know these values:
    AtomicOrder order;
};

struct IrInstructionDivExact {
    IrInstruction base;

    IrInstruction *op1;
    IrInstruction *op2;
};

struct IrInstructionTruncate {
    IrInstruction base;

    IrInstruction *dest_type;
    IrInstruction *target;
};

struct IrInstructionIntType {
    IrInstruction base;

    IrInstruction *is_signed;
    IrInstruction *bit_count;
};

struct IrInstructionBoolNot {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionMemset {
    IrInstruction base;

    IrInstruction *dest_ptr;
    IrInstruction *byte;
    IrInstruction *count;
};

struct IrInstructionMemcpy {
    IrInstruction base;

    IrInstruction *dest_ptr;
    IrInstruction *src_ptr;
    IrInstruction *count;
};

struct IrInstructionSlice {
    IrInstruction base;

    IrInstruction *ptr;
    IrInstruction *start;
    IrInstruction *end;
    bool is_const;
    bool safety_check_on;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionMemberCount {
    IrInstruction base;

    IrInstruction *container;
};

struct IrInstructionBreakpoint {
    IrInstruction base;
};

struct IrInstructionReturnAddress {
    IrInstruction base;
};

struct IrInstructionFrameAddress {
    IrInstruction base;
};

enum IrOverflowOp {
    IrOverflowOpAdd,
    IrOverflowOpSub,
    IrOverflowOpMul,
    IrOverflowOpShl,
};

struct IrInstructionOverflowOp {
    IrInstruction base;

    IrOverflowOp op;
    IrInstruction *type_value;
    IrInstruction *op1;
    IrInstruction *op2;
    IrInstruction *result_ptr;

    TypeTableEntry *result_ptr_type;
};

struct IrInstructionAlignOf {
    IrInstruction base;

    IrInstruction *type_value;
};

// returns true if error, returns false if not error
struct IrInstructionTestErr {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionUnwrapErrCode {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionUnwrapErrPayload {
    IrInstruction base;

    IrInstruction *value;
    bool safety_check_on;
};

struct IrInstructionMaybeWrap {
    IrInstruction base;

    IrInstruction *value;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionErrWrapPayload {
    IrInstruction base;

    IrInstruction *value;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionErrWrapCode {
    IrInstruction base;

    IrInstruction *value;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionFnProto {
    IrInstruction base;

    IrInstruction **param_types;
    IrInstruction *return_type;
};

// true if the target value is compile time known, false otherwise
struct IrInstructionTestComptime {
    IrInstruction base;

    IrInstruction *value;
};

struct IrInstructionInitEnum {
    IrInstruction base;

    TypeTableEntry *enum_type;
    TypeEnumField *field;
    IrInstruction *init_value;
    LLVMValueRef tmp_ptr;
};

struct IrInstructionPtrCast {
    IrInstruction base;

    IrInstruction *dest_type;
    IrInstruction *ptr;
};

struct IrInstructionWidenOrShorten {
    IrInstruction base;

    IrInstruction *target;
};

struct IrInstructionPtrToInt {
    IrInstruction base;

    IrInstruction *target;
};

struct IrInstructionIntToPtr {
    IrInstruction base;

    IrInstruction *dest_type;
    IrInstruction *target;
};

struct IrInstructionIntToEnum {
    IrInstruction base;

    IrInstruction *target;
};

struct IrInstructionIntToErr {
    IrInstruction base;

    IrInstruction *target;
};

struct IrInstructionErrToInt {
    IrInstruction base;

    IrInstruction *target;
};

struct IrInstructionCheckSwitchProngsRange {
    IrInstruction *start;
    IrInstruction *end;
};

struct IrInstructionCheckSwitchProngs {
    IrInstruction base;

    IrInstruction *target_value;
    IrInstructionCheckSwitchProngsRange *ranges;
    size_t range_count;
};

struct IrInstructionTestType {
    IrInstruction base;

    IrInstruction *type_value;
    TypeTableEntryId type_id;
};

struct IrInstructionTypeName {
    IrInstruction base;

    IrInstruction *type_value;
};

struct IrInstructionCanImplicitCast {
    IrInstruction base;

    IrInstruction *type_value;
    IrInstruction *target_value;
};

struct IrInstructionSetGlobalAlign {
    IrInstruction base;

    Tld *tld;
    IrInstruction *value;
};

struct IrInstructionSetGlobalSection {
    IrInstruction base;

    Tld *tld;
    IrInstruction *value;
};

struct IrInstructionSetGlobalLinkage {
    IrInstruction base;

    Tld *tld;
    IrInstruction *value;
};

struct IrInstructionDeclRef {
    IrInstruction base;

    Tld *tld;
    LVal lval;
};

struct IrInstructionPanic {
    IrInstruction base;

    IrInstruction *msg;
};

struct IrInstructionEnumTagName {
    IrInstruction base;

    IrInstruction *target;
};

struct IrInstructionSetFnRefInline {
    IrInstruction base;

    IrInstruction *fn_ref;
};

struct IrInstructionFieldParentPtr {
    IrInstruction base;

    IrInstruction *type_value;
    IrInstruction *field_name;
    IrInstruction *field_ptr;
    TypeStructField *field;
};

struct IrInstructionOffsetOf {
    IrInstruction base;

    IrInstruction *type_value;
    IrInstruction *field_name;
};

static const size_t slice_ptr_index = 0;
static const size_t slice_len_index = 1;

static const size_t maybe_child_index = 0;
static const size_t maybe_null_index = 1;

static const size_t enum_gen_tag_index = 0;
static const size_t enum_gen_union_index = 1;

static const size_t err_union_err_index = 0;
static const size_t err_union_payload_index = 1;

#endif
