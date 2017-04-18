/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "parser.hpp"
#include "errmsg.hpp"
#include "analyze.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

struct ParseContext {
    Buf *buf;
    AstNode *root;
    ZigList<Token> *tokens;
    ImportTableEntry *owner;
    ErrColor err_color;
    uint32_t *next_node_index;
    // These buffers are used freqently so we preallocate them once here.
    Buf *void_buf;
    Buf *empty_buf;
};

__attribute__ ((format (printf, 4, 5)))
__attribute__ ((noreturn))
static void ast_asm_error(ParseContext *pc, AstNode *node, size_t offset, const char *format, ...) {
    assert(node->type == NodeTypeAsmExpr);


    // TODO calculate or otherwise keep track of originating line/column number for strings
    //SrcPos pos = node->data.asm_expr.offset_map.at(offset);
    SrcPos pos = { node->line, node->column };

    va_list ap;
    va_start(ap, format);
    Buf *msg = buf_vprintf(format, ap);
    va_end(ap);

    ErrorMsg *err = err_msg_create_with_line(pc->owner->path, pos.line, pos.column,
            pc->owner->source_code, pc->owner->line_offsets, msg);

    print_err_msg(err, pc->err_color);
    exit(EXIT_FAILURE);
}

__attribute__ ((format (printf, 3, 4)))
__attribute__ ((noreturn))
static void ast_error(ParseContext *pc, Token *token, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    Buf *msg = buf_vprintf(format, ap);
    va_end(ap);


    ErrorMsg *err = err_msg_create_with_line(pc->owner->path, token->start_line, token->start_column,
            pc->owner->source_code, pc->owner->line_offsets, msg);
    err->line_start = token->start_line;
    err->column_start = token->start_column;

    print_err_msg(err, pc->err_color);
    exit(EXIT_FAILURE);
}

static AstNode *ast_create_node_no_line_info(ParseContext *pc, NodeType type) {
    AstNode *node = allocate<AstNode>(1);
    node->type = type;
    node->owner = pc->owner;
    node->create_index = *pc->next_node_index;
    *pc->next_node_index += 1;
    return node;
}

static void ast_update_node_line_info(AstNode *node, Token *first_token) {
    assert(first_token);
    node->line = first_token->start_line;
    node->column = first_token->start_column;
}

static AstNode *ast_create_node(ParseContext *pc, NodeType type, Token *first_token) {
    assert(first_token);
    AstNode *node = ast_create_node_no_line_info(pc, type);
    ast_update_node_line_info(node, first_token);
    return node;
}

static AstNode *ast_create_void_type_node(ParseContext *pc, Token *token) {
    AstNode *node = ast_create_node(pc, NodeTypeSymbol, token);
    node->data.symbol_expr.symbol = pc->void_buf;
    return node;
}

static void parse_asm_template(ParseContext *pc, AstNode *node) {
    Buf *asm_template = node->data.asm_expr.asm_template;

    enum State {
        StateStart,
        StatePercent,
        StateTemplate,
        StateVar,
    };

    ZigList<AsmToken> *tok_list = &node->data.asm_expr.token_list;
    assert(tok_list->length == 0);

    AsmToken *cur_tok = nullptr;

    enum State state = StateStart;

    for (size_t i = 0; i < buf_len(asm_template); i += 1) {
        uint8_t c = *((uint8_t*)buf_ptr(asm_template) + i);
        switch (state) {
            case StateStart:
                if (c == '%') {
                    tok_list->add_one();
                    cur_tok = &tok_list->last();
                    cur_tok->id = AsmTokenIdPercent;
                    cur_tok->start = i;
                    state = StatePercent;
                } else {
                    tok_list->add_one();
                    cur_tok = &tok_list->last();
                    cur_tok->id = AsmTokenIdTemplate;
                    cur_tok->start = i;
                    state = StateTemplate;
                }
                break;
            case StatePercent:
                if (c == '%') {
                    cur_tok->end = i;
                    state = StateStart;
                } else if (c == '[') {
                    cur_tok->id = AsmTokenIdVar;
                    state = StateVar;
                } else if (c == '=') {
                    cur_tok->id = AsmTokenIdUniqueId;
                    cur_tok->end = i;
                    state = StateStart;
                } else {
                    ast_asm_error(pc, node, i, "expected a '%%' or '['");
                }
                break;
            case StateTemplate:
                if (c == '%') {
                    cur_tok->end = i;
                    i -= 1;
                    cur_tok = nullptr;
                    state = StateStart;
                }
                break;
            case StateVar:
                if (c == ']') {
                    cur_tok->end = i;
                    state = StateStart;
                } else if ((c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        (c == '_'))
                {
                    // do nothing
                } else {
                    ast_asm_error(pc, node, i, "invalid substitution character: '%c'", c);
                }
                break;
        }
    }

    switch (state) {
        case StateStart:
            break;
        case StatePercent:
        case StateVar:
            ast_asm_error(pc, node, buf_len(asm_template), "unexpected end of assembly template");
            break;
        case StateTemplate:
            cur_tok->end = buf_len(asm_template);
            break;
    }
}

static Buf *token_buf(Token *token) {
    assert(token->id == TokenIdStringLiteral || token->id == TokenIdSymbol);
    return &token->data.str_lit.str;
}

static BigNum *token_bignum(Token *token) {
    assert(token->id == TokenIdNumberLiteral);
    return &token->data.num_lit.bignum;
}

static uint8_t token_char_lit(Token *token) {
    assert(token->id == TokenIdCharLiteral);
    return token->data.char_lit.c;
}

static void ast_buf_from_token(ParseContext *pc, Token *token, Buf *buf) {
    if (token->id == TokenIdSymbol) {
        buf_init_from_buf(buf, token_buf(token));
    } else {
        buf_init_from_mem(buf, buf_ptr(pc->buf) + token->start_pos, token->end_pos - token->start_pos);
    }
}

__attribute__ ((noreturn))
static void ast_invalid_token_error(ParseContext *pc, Token *token) {
    Buf token_value = BUF_INIT;
    ast_buf_from_token(pc, token, &token_value);
    ast_error(pc, token, "invalid token: '%s'", buf_ptr(&token_value));
}

static AstNode *ast_parse_block_or_expression(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_block_expr_or_expression(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_expression(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_block(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_if_expr(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_block_expr(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_unwrap_expr(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_prefix_op_expr(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_fn_proto(ParseContext *pc, size_t *token_index, bool mandatory, VisibMod visib_mod);
static AstNode *ast_parse_return_expr(ParseContext *pc, size_t *token_index);
static AstNode *ast_parse_grouped_expr(ParseContext *pc, size_t *token_index, bool mandatory);
static AstNode *ast_parse_container_decl(ParseContext *pc, size_t *token_index, bool mandatory);

static void ast_expect_token(ParseContext *pc, Token *token, TokenId token_id) {
    if (token->id == token_id) {
        return;
    }

    Buf token_value = BUF_INIT;
    ast_buf_from_token(pc, token, &token_value);
    ast_error(pc, token, "expected token '%s', found '%s'", token_name(token_id), token_name(token->id));
}

static Token *ast_eat_token(ParseContext *pc, size_t *token_index, TokenId token_id) {
    Token *token = &pc->tokens->at(*token_index);
    ast_expect_token(pc, token, token_id);
    *token_index += 1;
    return token;
}

/*
TypeExpr = PrefixOpExpression | "var"
*/
static AstNode *ast_parse_type_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    if (token->id == TokenIdKeywordVar) {
        AstNode *node = ast_create_node(pc, NodeTypeVarLiteral, token);
        *token_index += 1;
        return node;
    } else {
        return ast_parse_prefix_op_expr(pc, token_index, mandatory);
    }
}

/*
ParamDecl = option("noalias" | "comptime") option(Symbol ":") (TypeExpr | "...")
*/
static AstNode *ast_parse_param_decl(ParseContext *pc, size_t *token_index) {
    Token *token = &pc->tokens->at(*token_index);

    AstNode *node = ast_create_node(pc, NodeTypeParamDecl, token);

    if (token->id == TokenIdKeywordNoAlias) {
        node->data.param_decl.is_noalias = true;
        *token_index += 1;
        token = &pc->tokens->at(*token_index);
    } else if (token->id == TokenIdKeywordCompTime) {
        node->data.param_decl.is_inline = true;
        *token_index += 1;
        token = &pc->tokens->at(*token_index);
    }

    node->data.param_decl.name = pc->empty_buf;

    if (token->id == TokenIdSymbol) {
        Token *next_token = &pc->tokens->at(*token_index + 1);
        if (next_token->id == TokenIdColon) {
            node->data.param_decl.name = token_buf(token);
            *token_index += 2;
        }
    }

    Token *ellipsis_tok = &pc->tokens->at(*token_index);
    if (ellipsis_tok->id == TokenIdEllipsis) {
        *token_index += 1;
        node->data.param_decl.is_var_args = true;
    } else {
        node->data.param_decl.type = ast_parse_type_expr(pc, token_index, true);
    }

    return node;
}


static void ast_parse_param_decl_list(ParseContext *pc, size_t *token_index,
        ZigList<AstNode *> *params, bool *is_var_args)
{
    *is_var_args = false;

    ast_eat_token(pc, token_index, TokenIdLParen);

    Token *token = &pc->tokens->at(*token_index);
    if (token->id == TokenIdRParen) {
        *token_index += 1;
        return;
    }

    for (;;) {
        AstNode *param_decl_node = ast_parse_param_decl(pc, token_index);
        bool expect_end = false;
        assert(param_decl_node);
        params->append(param_decl_node);
        expect_end = param_decl_node->data.param_decl.is_var_args;
        *is_var_args = expect_end;

        Token *token = &pc->tokens->at(*token_index);
        *token_index += 1;
        if (token->id == TokenIdRParen) {
            return;
        } else if (expect_end) {
            ast_invalid_token_error(pc, token);
        } else {
            ast_expect_token(pc, token, TokenIdComma);
        }
    }
    zig_unreachable();
}

static void ast_parse_fn_call_param_list(ParseContext *pc, size_t *token_index, ZigList<AstNode*> *params) {
    Token *token = &pc->tokens->at(*token_index);
    if (token->id == TokenIdRParen) {
        *token_index += 1;
        return;
    }

    for (;;) {
        AstNode *expr = ast_parse_expression(pc, token_index, true);
        params->append(expr);

        Token *token = &pc->tokens->at(*token_index);
        *token_index += 1;
        if (token->id == TokenIdRParen) {
            return;
        } else {
            ast_expect_token(pc, token, TokenIdComma);
        }
    }
    zig_unreachable();
}

/*
GroupedExpression : token(LParen) Expression token(RParen)
*/
static AstNode *ast_parse_grouped_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *l_paren = &pc->tokens->at(*token_index);
    if (l_paren->id != TokenIdLParen) {
        if (mandatory) {
            ast_expect_token(pc, l_paren, TokenIdLParen);
        } else {
            return nullptr;
        }
    }
    *token_index += 1;

    AstNode *node = ast_create_node(pc, NodeTypeGroupedExpr, l_paren);

    node->data.grouped_expr = ast_parse_expression(pc, token_index, true);

    Token *r_paren = &pc->tokens->at(*token_index);
    *token_index += 1;
    ast_expect_token(pc, r_paren, TokenIdRParen);

    return node;
}

/*
ArrayType : "[" option(Expression) "]" option("const") PrefixOpExpression
*/
static AstNode *ast_parse_array_type_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *l_bracket = &pc->tokens->at(*token_index);
    if (l_bracket->id != TokenIdLBracket) {
        if (mandatory) {
            ast_expect_token(pc, l_bracket, TokenIdLBracket);
        } else {
            return nullptr;
        }
    }

    *token_index += 1;

    AstNode *node = ast_create_node(pc, NodeTypeArrayType, l_bracket);
    node->data.array_type.size = ast_parse_expression(pc, token_index, false);

    ast_eat_token(pc, token_index, TokenIdRBracket);

    Token *const_tok = &pc->tokens->at(*token_index);
    if (const_tok->id == TokenIdKeywordConst) {
        *token_index += 1;
        node->data.array_type.is_const = true;
    }

    node->data.array_type.child_type = ast_parse_type_expr(pc, token_index, true);

    return node;
}

/*
AsmInputItem : token(LBracket) token(Symbol) token(RBracket) token(String) token(LParen) Expression token(RParen)
*/
static void ast_parse_asm_input_item(ParseContext *pc, size_t *token_index, AstNode *node) {
    ast_eat_token(pc, token_index, TokenIdLBracket);
    Token *alias = ast_eat_token(pc, token_index, TokenIdSymbol);
    ast_eat_token(pc, token_index, TokenIdRBracket);

    Token *constraint = ast_eat_token(pc, token_index, TokenIdStringLiteral);

    ast_eat_token(pc, token_index, TokenIdLParen);
    AstNode *expr_node = ast_parse_expression(pc, token_index, true);
    ast_eat_token(pc, token_index, TokenIdRParen);

    AsmInput *asm_input = allocate<AsmInput>(1);
    asm_input->asm_symbolic_name = token_buf(alias);
    asm_input->constraint = token_buf(constraint);
    asm_input->expr = expr_node;
    node->data.asm_expr.input_list.append(asm_input);
}

/*
AsmOutputItem : "[" "Symbol" "]" "String" "(" ("Symbol" | "->" PrefixOpExpression) ")"
*/
static void ast_parse_asm_output_item(ParseContext *pc, size_t *token_index, AstNode *node) {
    ast_eat_token(pc, token_index, TokenIdLBracket);
    Token *alias = ast_eat_token(pc, token_index, TokenIdSymbol);
    ast_eat_token(pc, token_index, TokenIdRBracket);

    Token *constraint = ast_eat_token(pc, token_index, TokenIdStringLiteral);

    AsmOutput *asm_output = allocate<AsmOutput>(1);

    ast_eat_token(pc, token_index, TokenIdLParen);

    Token *token = &pc->tokens->at(*token_index);
    *token_index += 1;
    if (token->id == TokenIdSymbol) {
        asm_output->variable_name = token_buf(token);
    } else if (token->id == TokenIdArrow) {
        asm_output->return_type = ast_parse_type_expr(pc, token_index, true);
    } else {
        ast_invalid_token_error(pc, token);
    }

    ast_eat_token(pc, token_index, TokenIdRParen);

    asm_output->asm_symbolic_name = token_buf(alias);
    asm_output->constraint = token_buf(constraint);
    node->data.asm_expr.output_list.append(asm_output);
}

/*
AsmClobbers: token(Colon) list(token(String), token(Comma))
*/
static void ast_parse_asm_clobbers(ParseContext *pc, size_t *token_index, AstNode *node) {
    Token *colon_tok = &pc->tokens->at(*token_index);

    if (colon_tok->id != TokenIdColon)
        return;

    *token_index += 1;

    for (;;) {
        Token *string_tok = &pc->tokens->at(*token_index);
        ast_expect_token(pc, string_tok, TokenIdStringLiteral);
        *token_index += 1;

        Buf *clobber_buf = token_buf(string_tok);
        node->data.asm_expr.clobber_list.append(clobber_buf);

        Token *comma = &pc->tokens->at(*token_index);

        if (comma->id == TokenIdComma) {
            *token_index += 1;
            continue;
        } else {
            break;
        }
    }
}

/*
AsmInput : token(Colon) list(AsmInputItem, token(Comma)) option(AsmClobbers)
*/
static void ast_parse_asm_input(ParseContext *pc, size_t *token_index, AstNode *node) {
    Token *colon_tok = &pc->tokens->at(*token_index);

    if (colon_tok->id != TokenIdColon)
        return;

    *token_index += 1;

    Token *colon_again = &pc->tokens->at(*token_index);
    if (colon_again->id == TokenIdColon) {
        ast_parse_asm_clobbers(pc, token_index, node);
        return;
    }

    for (;;) {
        ast_parse_asm_input_item(pc, token_index, node);

        Token *comma = &pc->tokens->at(*token_index);

        if (comma->id == TokenIdComma) {
            *token_index += 1;
            continue;
        } else {
            break;
        }
    }

    ast_parse_asm_clobbers(pc, token_index, node);
}

/*
AsmOutput : token(Colon) list(AsmOutputItem, token(Comma)) option(AsmInput)
*/
static void ast_parse_asm_output(ParseContext *pc, size_t *token_index, AstNode *node) {
    Token *colon_tok = &pc->tokens->at(*token_index);

    if (colon_tok->id != TokenIdColon)
        return;

    *token_index += 1;

    Token *colon_again = &pc->tokens->at(*token_index);
    if (colon_again->id == TokenIdColon) {
        ast_parse_asm_input(pc, token_index, node);
        return;
    }

    for (;;) {
        ast_parse_asm_output_item(pc, token_index, node);

        Token *comma = &pc->tokens->at(*token_index);

        if (comma->id == TokenIdComma) {
            *token_index += 1;
            continue;
        } else {
            break;
        }
    }

    ast_parse_asm_input(pc, token_index, node);
}

/*
AsmExpression : token(Asm) option(token(Volatile)) token(LParen) token(String) option(AsmOutput) token(RParen)
*/
static AstNode *ast_parse_asm_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *asm_token = &pc->tokens->at(*token_index);

    if (asm_token->id != TokenIdKeywordAsm) {
        if (mandatory) {
            ast_expect_token(pc, asm_token, TokenIdKeywordAsm);
        } else {
            return nullptr;
        }
    }

    AstNode *node = ast_create_node(pc, NodeTypeAsmExpr, asm_token);

    *token_index += 1;
    Token *lparen_tok = &pc->tokens->at(*token_index);

    if (lparen_tok->id == TokenIdKeywordVolatile) {
        node->data.asm_expr.is_volatile = true;

        *token_index += 1;
        lparen_tok = &pc->tokens->at(*token_index);
    }

    ast_expect_token(pc, lparen_tok, TokenIdLParen);
    *token_index += 1;

    Token *template_tok = ast_eat_token(pc, token_index, TokenIdStringLiteral);

    node->data.asm_expr.asm_template = token_buf(template_tok);
    parse_asm_template(pc, node);

    ast_parse_asm_output(pc, token_index, node);

    ast_eat_token(pc, token_index, TokenIdRParen);

    return node;
}

/*
GotoExpression = "goto" Symbol
*/
static AstNode *ast_parse_goto_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *goto_token = &pc->tokens->at(*token_index);
    if (goto_token->id == TokenIdKeywordGoto) {
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, goto_token, TokenIdKeywordGoto);
        zig_unreachable();
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeGoto, goto_token);

    Token *dest_symbol = ast_eat_token(pc, token_index, TokenIdSymbol);
    node->data.goto_expr.name = token_buf(dest_symbol);
    return node;
}

/*
CompTimeExpression(body) = "comptime" body
*/
static AstNode *ast_parse_comptime_expr(ParseContext *pc, size_t *token_index, bool require_block_body, bool mandatory) {
    Token *comptime_token = &pc->tokens->at(*token_index);
    if (comptime_token->id == TokenIdKeywordCompTime) {
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, comptime_token, TokenIdKeywordCompTime);
        zig_unreachable();
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeCompTime, comptime_token);
    if (require_block_body)
        node->data.comptime_expr.expr = ast_parse_block(pc, token_index, true);
    else
        node->data.comptime_expr.expr = ast_parse_block_or_expression(pc, token_index, true);
    return node;
}

/*
TryExpression(body) = "try" "(" option(("const" | "var") option("*") Symbol "=") Expression  ")" body option("else" option("|" Symbol "|") body)
*/
static AstNode *ast_parse_try_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *try_token = &pc->tokens->at(*token_index);
    if (try_token->id == TokenIdKeywordTry) {
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, try_token, TokenIdKeywordTry);
        zig_unreachable();
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeTryExpr, try_token);

    ast_eat_token(pc, token_index, TokenIdLParen);

    Token *var_token = &pc->tokens->at(*token_index);
    bool have_vars;
    if (var_token->id == TokenIdKeywordVar) {
        node->data.try_expr.var_is_const = false;
        *token_index += 1;
        have_vars = true;
    } else if (var_token->id == TokenIdKeywordConst) {
        node->data.try_expr.var_is_const = true;
        *token_index += 1;
        have_vars = true;
    } else {
        have_vars = false;
    }

    if (have_vars) {
        Token *star_token = &pc->tokens->at(*token_index);
        if (star_token->id == TokenIdStar) {
            node->data.try_expr.var_is_ptr = true;
            *token_index += 1;
        }

        Token *var_name_tok = ast_eat_token(pc, token_index, TokenIdSymbol);
        node->data.try_expr.var_symbol = token_buf(var_name_tok);

        ast_eat_token(pc, token_index, TokenIdEq);
    }

    node->data.try_expr.target_node = ast_parse_expression(pc, token_index, true);

    ast_eat_token(pc, token_index, TokenIdRParen);

    node->data.try_expr.then_node = ast_parse_block_or_expression(pc, token_index, true);

    Token *else_token = &pc->tokens->at(*token_index);
    if (else_token->id == TokenIdKeywordElse) {
        *token_index += 1;
        Token *open_bar_tok = &pc->tokens->at(*token_index);
        if (open_bar_tok->id == TokenIdBinOr) {
            *token_index += 1;

            Token *err_name_tok = ast_eat_token(pc, token_index, TokenIdSymbol);
            node->data.try_expr.err_symbol = token_buf(err_name_tok);

            ast_eat_token(pc, token_index, TokenIdBinOr);
        }

        node->data.try_expr.else_node = ast_parse_block_expr_or_expression(pc, token_index, true);
    }

    return node;
}

/*
PrimaryExpression = Number | String | CharLiteral | KeywordLiteral | GroupedExpression | GotoExpression | BlockExpression(BlockOrExpression) | Symbol | ("@" Symbol FnCallExpression) | ArrayType | (option("extern") FnProto) | AsmExpression | ("error" "." Symbol) | ContainerDecl
KeywordLiteral = "true" | "false" | "null" | "break" | "continue" | "undefined" | "error" | "this" | "unreachable"
*/
static AstNode *ast_parse_primary_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);

    if (token->id == TokenIdNumberLiteral) {
        AstNode *node = ast_create_node(pc, NodeTypeNumberLiteral, token);
        node->data.number_literal.bignum = token_bignum(token);
        node->data.number_literal.overflow = token->data.num_lit.overflow;
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdStringLiteral) {
        AstNode *node = ast_create_node(pc, NodeTypeStringLiteral, token);
        node->data.string_literal.buf = token_buf(token);
        node->data.string_literal.c = token->data.str_lit.is_c_str;
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdCharLiteral) {
        AstNode *node = ast_create_node(pc, NodeTypeCharLiteral, token);
        node->data.char_literal.value = token_char_lit(token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordTrue) {
        AstNode *node = ast_create_node(pc, NodeTypeBoolLiteral, token);
        node->data.bool_literal.value = true;
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordFalse) {
        AstNode *node = ast_create_node(pc, NodeTypeBoolLiteral, token);
        node->data.bool_literal.value = false;
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordNull) {
        AstNode *node = ast_create_node(pc, NodeTypeNullLiteral, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordBreak) {
        AstNode *node = ast_create_node(pc, NodeTypeBreak, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordContinue) {
        AstNode *node = ast_create_node(pc, NodeTypeContinue, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordUndefined) {
        AstNode *node = ast_create_node(pc, NodeTypeUndefinedLiteral, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordThis) {
        AstNode *node = ast_create_node(pc, NodeTypeThisLiteral, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordUnreachable) {
        AstNode *node = ast_create_node(pc, NodeTypeUnreachable, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdKeywordError) {
        AstNode *node = ast_create_node(pc, NodeTypeErrorType, token);
        *token_index += 1;
        return node;
    } else if (token->id == TokenIdAtSign) {
        *token_index += 1;
        Token *name_tok = ast_eat_token(pc, token_index, TokenIdSymbol);
        AstNode *name_node = ast_create_node(pc, NodeTypeSymbol, name_tok);
        name_node->data.symbol_expr.symbol = token_buf(name_tok);

        AstNode *node = ast_create_node(pc, NodeTypeFnCallExpr, token);
        node->data.fn_call_expr.fn_ref_expr = name_node;
        ast_eat_token(pc, token_index, TokenIdLParen);
        ast_parse_fn_call_param_list(pc, token_index, &node->data.fn_call_expr.params);
        node->data.fn_call_expr.is_builtin = true;

        return node;
    } else if (token->id == TokenIdSymbol) {
        *token_index += 1;
        AstNode *node = ast_create_node(pc, NodeTypeSymbol, token);
        node->data.symbol_expr.symbol = token_buf(token);
        return node;
    }

    AstNode *goto_node = ast_parse_goto_expr(pc, token_index, false);
    if (goto_node)
        return goto_node;

    AstNode *grouped_expr_node = ast_parse_grouped_expr(pc, token_index, false);
    if (grouped_expr_node) {
        return grouped_expr_node;
    }

    AstNode *block_expr_node = ast_parse_block_expr(pc, token_index, false);
    if (block_expr_node) {
        return block_expr_node;
    }

    AstNode *array_type_node = ast_parse_array_type_expr(pc, token_index, false);
    if (array_type_node) {
        return array_type_node;
    }

    AstNode *fn_proto_node = ast_parse_fn_proto(pc, token_index, false, VisibModPrivate);
    if (fn_proto_node) {
        return fn_proto_node;
    }

    AstNode *asm_expr = ast_parse_asm_expr(pc, token_index, false);
    if (asm_expr) {
        return asm_expr;
    }

    AstNode *container_decl = ast_parse_container_decl(pc, token_index, false);
    if (container_decl)
        return container_decl;

    if (token->id == TokenIdKeywordExtern) {
        *token_index += 1;
        AstNode *node = ast_parse_fn_proto(pc, token_index, true, VisibModPrivate);
        node->data.fn_proto.is_extern = true;
        return node;
    }

    if (!mandatory)
        return nullptr;

    ast_invalid_token_error(pc, token);
}

/*
CurlySuffixExpression : PrefixOpExpression option(ContainerInitExpression)
ContainerInitExpression : token(LBrace) ContainerInitBody token(RBrace)
ContainerInitBody : list(StructLiteralField, token(Comma)) | list(Expression, token(Comma))
*/
static AstNode *ast_parse_curly_suffix_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *prefix_op_expr = ast_parse_prefix_op_expr(pc, token_index, mandatory);
    if (!prefix_op_expr) {
        return nullptr;
    }

    while (true) {
        Token *first_token = &pc->tokens->at(*token_index);
        if (first_token->id == TokenIdLBrace) {
            *token_index += 1;

            AstNode *node = ast_create_node(pc, NodeTypeContainerInitExpr, first_token);
            node->data.container_init_expr.type = prefix_op_expr;

            Token *token = &pc->tokens->at(*token_index);
            if (token->id == TokenIdDot) {
                node->data.container_init_expr.kind = ContainerInitKindStruct;
                for (;;) {
                    if (token->id == TokenIdDot) {
                        ast_eat_token(pc, token_index, TokenIdDot);
                        Token *field_name_tok = ast_eat_token(pc, token_index, TokenIdSymbol);
                        ast_eat_token(pc, token_index, TokenIdEq);

                        AstNode *field_node = ast_create_node(pc, NodeTypeStructValueField, token);

                        field_node->data.struct_val_field.name = token_buf(field_name_tok);
                        field_node->data.struct_val_field.expr = ast_parse_expression(pc, token_index, true);

                        node->data.container_init_expr.entries.append(field_node);

                        Token *comma_tok = &pc->tokens->at(*token_index);
                        if (comma_tok->id == TokenIdComma) {
                            *token_index += 1;
                            token = &pc->tokens->at(*token_index);
                            continue;
                        } else if (comma_tok->id != TokenIdRBrace) {
                            ast_expect_token(pc, comma_tok, TokenIdRBrace);
                        } else {
                            *token_index += 1;
                            break;
                        }
                    } else if (token->id == TokenIdRBrace) {
                        *token_index += 1;
                        break;
                    } else {
                        ast_invalid_token_error(pc, token);
                    }
                }

            } else {
                node->data.container_init_expr.kind = ContainerInitKindArray;
                for (;;) {
                    if (token->id == TokenIdRBrace) {
                        *token_index += 1;
                        break;
                    } else {
                        AstNode *elem_node = ast_parse_expression(pc, token_index, true);
                        node->data.container_init_expr.entries.append(elem_node);

                        Token *comma_tok = &pc->tokens->at(*token_index);
                        if (comma_tok->id == TokenIdComma) {
                            *token_index += 1;
                            token = &pc->tokens->at(*token_index);
                            continue;
                        } else if (comma_tok->id != TokenIdRBrace) {
                            ast_expect_token(pc, comma_tok, TokenIdRBrace);
                        } else {
                            *token_index += 1;
                            break;
                        }
                    }
                }
            }

            prefix_op_expr = node;
        } else {
            return prefix_op_expr;
        }
    }
}

/*
InlineExpression = option("inline") PrimaryExpression
*/
static AstNode *ast_parse_inline_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);

    if (token->id == TokenIdKeywordInline) {
        *token_index += 1;
        AstNode *primary_expr_node = ast_parse_primary_expr(pc, token_index, true);
        if (primary_expr_node->type == NodeTypeWhileExpr) {
            primary_expr_node->data.while_expr.is_inline = true;
            return primary_expr_node;
        } else if (primary_expr_node->type == NodeTypeForExpr) {
            primary_expr_node->data.for_expr.is_inline = true;
            return primary_expr_node;
        } else {
            AstNode *node = ast_create_node(pc, NodeTypeInlineExpr, token);
            node->data.inline_expr.body = primary_expr_node;
            return node;
        }
    } else {
        return ast_parse_primary_expr(pc, token_index, mandatory);
    }
}

/*
SuffixOpExpression = InlineExpression option(FnCallExpression | ArrayAccessExpression | FieldAccessExpression | SliceExpression)
FnCallExpression : token(LParen) list(Expression, token(Comma)) token(RParen)
ArrayAccessExpression : token(LBracket) Expression token(RBracket)
SliceExpression : token(LBracket) Expression token(Ellipsis) option(Expression) token(RBracket) option(token(Const))
FieldAccessExpression : token(Dot) token(Symbol)
StructLiteralField : token(Dot) token(Symbol) token(Eq) Expression
*/
static AstNode *ast_parse_suffix_op_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *inline_expr = ast_parse_inline_expr(pc, token_index, mandatory);
    if (!inline_expr)
        return nullptr;

    while (true) {
        Token *first_token = &pc->tokens->at(*token_index);
        if (first_token->id == TokenIdLParen) {
            *token_index += 1;

            AstNode *node = ast_create_node(pc, NodeTypeFnCallExpr, first_token);
            node->data.fn_call_expr.fn_ref_expr = inline_expr;
            ast_parse_fn_call_param_list(pc, token_index, &node->data.fn_call_expr.params);

            inline_expr = node;
        } else if (first_token->id == TokenIdLBracket) {
            *token_index += 1;

            AstNode *expr_node = ast_parse_expression(pc, token_index, true);

            Token *ellipsis_or_r_bracket = &pc->tokens->at(*token_index);

            if (ellipsis_or_r_bracket->id == TokenIdEllipsis) {
                *token_index += 1;

                AstNode *node = ast_create_node(pc, NodeTypeSliceExpr, first_token);
                node->data.slice_expr.array_ref_expr = inline_expr;
                node->data.slice_expr.start = expr_node;
                node->data.slice_expr.end = ast_parse_expression(pc, token_index, false);

                ast_eat_token(pc, token_index, TokenIdRBracket);

                Token *const_tok = &pc->tokens->at(*token_index);
                if (const_tok->id == TokenIdKeywordConst) {
                    *token_index += 1;
                    node->data.slice_expr.is_const = true;
                }

                inline_expr = node;
            } else if (ellipsis_or_r_bracket->id == TokenIdRBracket) {
                *token_index += 1;

                AstNode *node = ast_create_node(pc, NodeTypeArrayAccessExpr, first_token);
                node->data.array_access_expr.array_ref_expr = inline_expr;
                node->data.array_access_expr.subscript = expr_node;

                inline_expr = node;
            } else {
                ast_invalid_token_error(pc, first_token);
            }
        } else if (first_token->id == TokenIdDot) {
            *token_index += 1;

            Token *name_token = ast_eat_token(pc, token_index, TokenIdSymbol);

            AstNode *node = ast_create_node(pc, NodeTypeFieldAccessExpr, first_token);
            node->data.field_access_expr.struct_expr = inline_expr;
            node->data.field_access_expr.field_name = token_buf(name_token);

            inline_expr = node;
        } else {
            return inline_expr;
        }
    }
}

static PrefixOp tok_to_prefix_op(Token *token) {
    switch (token->id) {
        case TokenIdBang: return PrefixOpBoolNot;
        case TokenIdDash: return PrefixOpNegation;
        case TokenIdMinusPercent: return PrefixOpNegationWrap;
        case TokenIdTilde: return PrefixOpBinNot;
        case TokenIdAmpersand: return PrefixOpAddressOf;
        case TokenIdStar: return PrefixOpDereference;
        case TokenIdMaybe: return PrefixOpMaybe;
        case TokenIdPercent: return PrefixOpError;
        case TokenIdPercentPercent: return PrefixOpUnwrapError;
        case TokenIdDoubleQuestion: return PrefixOpUnwrapMaybe;
        case TokenIdStarStar: return PrefixOpDereference;
        default: return PrefixOpInvalid;
    }
}

/*
PrefixOpExpression : PrefixOp PrefixOpExpression | SuffixOpExpression
PrefixOp = "!" | "-" | "~" | "*" | ("&" option("const") option("volatile")) | "?" | "%" | "%%" | "??" | "-%"
*/
static AstNode *ast_parse_prefix_op_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    PrefixOp prefix_op = tok_to_prefix_op(token);
    if (prefix_op == PrefixOpInvalid) {
        return ast_parse_suffix_op_expr(pc, token_index, mandatory);
    }

    if (prefix_op == PrefixOpError || prefix_op == PrefixOpMaybe) {
        Token *maybe_return = &pc->tokens->at(*token_index + 1);
        if (maybe_return->id == TokenIdKeywordReturn) {
            return ast_parse_return_expr(pc, token_index);
        }
    }

    *token_index += 1;


    AstNode *node = ast_create_node(pc, NodeTypePrefixOpExpr, token);
    AstNode *parent_node = node;
    if (token->id == TokenIdStarStar) {
        // pretend that we got 2 star tokens

        parent_node = ast_create_node(pc, NodeTypePrefixOpExpr, token);
        parent_node->data.prefix_op_expr.primary_expr = node;
        parent_node->data.prefix_op_expr.prefix_op = PrefixOpDereference;

        node->column += 1;
    }

    if (prefix_op == PrefixOpAddressOf) {
        Token *const_or_volatile_tok = &pc->tokens->at(*token_index);
        if (const_or_volatile_tok->id == TokenIdKeywordConst) {
            *token_index += 1;
            Token *volatile_token = &pc->tokens->at(*token_index);
            if (volatile_token->id == TokenIdKeywordVolatile) {
                *token_index += 1;
                prefix_op = PrefixOpConstVolatileAddressOf;
            } else {
                prefix_op = PrefixOpConstAddressOf;
            }
        } else if (const_or_volatile_tok->id == TokenIdKeywordVolatile) {
            prefix_op = PrefixOpVolatileAddressOf;
            *token_index += 1;
        }
    }

    AstNode *prefix_op_expr = ast_parse_prefix_op_expr(pc, token_index, true);
    node->data.prefix_op_expr.primary_expr = prefix_op_expr;
    node->data.prefix_op_expr.prefix_op = prefix_op;

    return parent_node;
}


static BinOpType tok_to_mult_op(Token *token) {
    switch (token->id) {
        case TokenIdStar:           return BinOpTypeMult;
        case TokenIdTimesPercent:   return BinOpTypeMultWrap;
        case TokenIdStarStar:       return BinOpTypeArrayMult;
        case TokenIdSlash:          return BinOpTypeDiv;
        case TokenIdPercent:        return BinOpTypeMod;
        default:                    return BinOpTypeInvalid;
    }
}

/*
MultiplyOperator = "*" | "/" | "%" | "**" | "*%"
*/
static BinOpType ast_parse_mult_op(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    BinOpType result = tok_to_mult_op(token);
    if (result == BinOpTypeInvalid) {
        if (mandatory) {
            ast_invalid_token_error(pc, token);
        } else {
            return BinOpTypeInvalid;
        }
    }
    *token_index += 1;
    return result;
}

/*
MultiplyExpression : CurlySuffixExpression MultiplyOperator MultiplyExpression | CurlySuffixExpression
*/
static AstNode *ast_parse_mult_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_curly_suffix_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        BinOpType mult_op = ast_parse_mult_op(pc, token_index, false);
        if (mult_op == BinOpTypeInvalid)
            return operand_1;

        AstNode *operand_2 = ast_parse_curly_suffix_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = mult_op;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

static BinOpType tok_to_add_op(Token *token) {
    switch (token->id) {
        case TokenIdPlus:           return BinOpTypeAdd;
        case TokenIdPlusPercent:    return BinOpTypeAddWrap;
        case TokenIdDash:           return BinOpTypeSub;
        case TokenIdMinusPercent:   return BinOpTypeSubWrap;
        case TokenIdPlusPlus:       return BinOpTypeArrayCat;
        default:                    return BinOpTypeInvalid;
    }
}

/*
AdditionOperator = "+" | "-" | "++" | "+%" | "-%"
*/
static BinOpType ast_parse_add_op(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    BinOpType result = tok_to_add_op(token);
    if (result == BinOpTypeInvalid) {
        if (mandatory) {
            ast_invalid_token_error(pc, token);
        } else {
            return BinOpTypeInvalid;
        }
    }
    *token_index += 1;
    return result;
}

/*
AdditionExpression : MultiplyExpression AdditionOperator AdditionExpression | MultiplyExpression
*/
static AstNode *ast_parse_add_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_mult_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        BinOpType add_op = ast_parse_add_op(pc, token_index, false);
        if (add_op == BinOpTypeInvalid)
            return operand_1;

        AstNode *operand_2 = ast_parse_mult_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = add_op;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

static BinOpType tok_to_bit_shift_op(Token *token) {
    switch (token->id) {
        case TokenIdBitShiftLeft:           return BinOpTypeBitShiftLeft;
        case TokenIdBitShiftLeftPercent:    return BinOpTypeBitShiftLeftWrap;
        case TokenIdBitShiftRight:          return BinOpTypeBitShiftRight;
        default: return BinOpTypeInvalid;
    }
}

/*
BitShiftOperator = "<<" | ">>" | "<<%"
*/
static BinOpType ast_parse_bit_shift_op(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    BinOpType result = tok_to_bit_shift_op(token);
    if (result == BinOpTypeInvalid) {
        if (mandatory) {
            ast_invalid_token_error(pc, token);
        } else {
            return BinOpTypeInvalid;
        }
    }
    *token_index += 1;
    return result;
}

/*
BitShiftExpression : AdditionExpression BitShiftOperator BitShiftExpression | AdditionExpression
*/
static AstNode *ast_parse_bit_shift_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_add_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        BinOpType bit_shift_op = ast_parse_bit_shift_op(pc, token_index, false);
        if (bit_shift_op == BinOpTypeInvalid)
            return operand_1;

        AstNode *operand_2 = ast_parse_add_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = bit_shift_op;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}


/*
BinaryAndExpression : BitShiftExpression token(Ampersand) BinaryAndExpression | BitShiftExpression
*/
static AstNode *ast_parse_bin_and_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_bit_shift_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        if (token->id != TokenIdAmpersand)
            return operand_1;
        *token_index += 1;

        AstNode *operand_2 = ast_parse_bit_shift_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = BinOpTypeBinAnd;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

/*
BinaryXorExpression : BinaryAndExpression token(BinXor) BinaryXorExpression | BinaryAndExpression
*/
static AstNode *ast_parse_bin_xor_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_bin_and_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        if (token->id != TokenIdBinXor)
            return operand_1;
        *token_index += 1;

        AstNode *operand_2 = ast_parse_bin_and_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = BinOpTypeBinXor;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

/*
BinaryOrExpression : BinaryXorExpression token(BinOr) BinaryOrExpression | BinaryXorExpression
*/
static AstNode *ast_parse_bin_or_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_bin_xor_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        if (token->id != TokenIdBinOr)
            return operand_1;
        *token_index += 1;

        AstNode *operand_2 = ast_parse_bin_xor_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = BinOpTypeBinOr;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

static BinOpType tok_to_cmp_op(Token *token) {
    switch (token->id) {
        case TokenIdCmpEq: return BinOpTypeCmpEq;
        case TokenIdCmpNotEq: return BinOpTypeCmpNotEq;
        case TokenIdCmpLessThan: return BinOpTypeCmpLessThan;
        case TokenIdCmpGreaterThan: return BinOpTypeCmpGreaterThan;
        case TokenIdCmpLessOrEq: return BinOpTypeCmpLessOrEq;
        case TokenIdCmpGreaterOrEq: return BinOpTypeCmpGreaterOrEq;
        default: return BinOpTypeInvalid;
    }
}

static BinOpType ast_parse_comparison_operator(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    BinOpType result = tok_to_cmp_op(token);
    if (result == BinOpTypeInvalid) {
        if (mandatory) {
            ast_invalid_token_error(pc, token);
        } else {
            return BinOpTypeInvalid;
        }
    }
    *token_index += 1;
    return result;
}

/*
ComparisonExpression : BinaryOrExpression ComparisonOperator BinaryOrExpression | BinaryOrExpression
*/
static AstNode *ast_parse_comparison_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_bin_or_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    Token *token = &pc->tokens->at(*token_index);
    BinOpType cmp_op = ast_parse_comparison_operator(pc, token_index, false);
    if (cmp_op == BinOpTypeInvalid)
        return operand_1;

    AstNode *operand_2 = ast_parse_bin_or_expr(pc, token_index, true);

    AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
    node->data.bin_op_expr.op1 = operand_1;
    node->data.bin_op_expr.bin_op = cmp_op;
    node->data.bin_op_expr.op2 = operand_2;

    return node;
}

/*
BoolAndExpression = ComparisonExpression "and" BoolAndExpression | ComparisonExpression
 */
static AstNode *ast_parse_bool_and_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_comparison_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        if (token->id != TokenIdKeywordAnd)
            return operand_1;
        *token_index += 1;

        AstNode *operand_2 = ast_parse_comparison_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = BinOpTypeBoolAnd;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

/*
IfExpression(body) = IfVarExpression(body) | IfBoolExpression(body)
IfBoolExpression(body) = "if" "(" Expression ")" body option("else" body)
IfVarExpression(body) = "if" "(" ("const" | "var") option("*") Symbol option(":" TypeExpr) "?=" Expression ")" body Option("else" body)
*/
static AstNode *ast_parse_if_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *if_token = &pc->tokens->at(*token_index);

    if (if_token->id == TokenIdKeywordIf) {
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, if_token, TokenIdKeywordIf);
        zig_unreachable();
    } else {
        return nullptr;
    }

    ast_eat_token(pc, token_index, TokenIdLParen);

    Token *token = &pc->tokens->at(*token_index);
    if (token->id == TokenIdKeywordConst || token->id == TokenIdKeywordVar) {
        AstNode *node = ast_create_node(pc, NodeTypeIfVarExpr, if_token);
        node->data.if_var_expr.var_decl.is_const = (token->id == TokenIdKeywordConst);
        *token_index += 1;

        Token *star_or_symbol = &pc->tokens->at(*token_index);
        if (star_or_symbol->id == TokenIdStar) {
            *token_index += 1;
            node->data.if_var_expr.var_is_ptr = true;
            Token *name_token = ast_eat_token(pc, token_index, TokenIdSymbol);
            node->data.if_var_expr.var_decl.symbol = token_buf(name_token);
        } else if (star_or_symbol->id == TokenIdSymbol) {
            *token_index += 1;
            node->data.if_var_expr.var_decl.symbol = token_buf(star_or_symbol);
        } else {
            ast_invalid_token_error(pc, star_or_symbol);
        }


        Token *eq_or_colon = &pc->tokens->at(*token_index);
        if (eq_or_colon->id == TokenIdMaybeAssign) {
            *token_index += 1;
            node->data.if_var_expr.var_decl.expr = ast_parse_expression(pc, token_index, true);
        } else if (eq_or_colon->id == TokenIdColon) {
            *token_index += 1;
            node->data.if_var_expr.var_decl.type = ast_parse_type_expr(pc, token_index, true);

            ast_eat_token(pc, token_index, TokenIdMaybeAssign);
            node->data.if_var_expr.var_decl.expr = ast_parse_expression(pc, token_index, true);
        } else {
            ast_invalid_token_error(pc, eq_or_colon);
        }
        ast_eat_token(pc, token_index, TokenIdRParen);
        node->data.if_var_expr.then_block = ast_parse_block_or_expression(pc, token_index, true);

        Token *else_token = &pc->tokens->at(*token_index);
        if (else_token->id == TokenIdKeywordElse) {
            *token_index += 1;
            node->data.if_var_expr.else_node = ast_parse_block_expr_or_expression(pc, token_index, true);
        }

        return node;
    } else {
        AstNode *node = ast_create_node(pc, NodeTypeIfBoolExpr, if_token);
        node->data.if_bool_expr.condition = ast_parse_expression(pc, token_index, true);
        ast_eat_token(pc, token_index, TokenIdRParen);
        node->data.if_bool_expr.then_block = ast_parse_block_or_expression(pc, token_index, true);

        Token *else_token = &pc->tokens->at(*token_index);
        if (else_token->id == TokenIdKeywordElse) {
            *token_index += 1;
            node->data.if_bool_expr.else_node = ast_parse_block_expr_or_expression(pc, token_index, true);
        }

        return node;
    }
}

/*
ReturnExpression : option("%" | "?") "return" option(Expression)
*/
static AstNode *ast_parse_return_expr(ParseContext *pc, size_t *token_index) {
    Token *token = &pc->tokens->at(*token_index);

    NodeType node_type;
    ReturnKind kind;

    if (token->id == TokenIdPercent) {
        Token *next_token = &pc->tokens->at(*token_index + 1);
        if (next_token->id == TokenIdKeywordReturn) {
            kind = ReturnKindError;
            node_type = NodeTypeReturnExpr;
            *token_index += 2;
        } else {
            return nullptr;
        }
    } else if (token->id == TokenIdMaybe) {
        Token *next_token = &pc->tokens->at(*token_index + 1);
        if (next_token->id == TokenIdKeywordReturn) {
            kind = ReturnKindMaybe;
            node_type = NodeTypeReturnExpr;
            *token_index += 2;
        } else {
            return nullptr;
        }
    } else if (token->id == TokenIdKeywordReturn) {
        kind = ReturnKindUnconditional;
        node_type = NodeTypeReturnExpr;
        *token_index += 1;
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, node_type, token);
    node->data.return_expr.kind = kind;
    node->data.return_expr.expr = ast_parse_expression(pc, token_index, false);

    return node;
}

/*
Defer(body) = option("%" | "?") "defer" body
*/
static AstNode *ast_parse_defer_expr(ParseContext *pc, size_t *token_index) {
    Token *token = &pc->tokens->at(*token_index);

    NodeType node_type;
    ReturnKind kind;

    if (token->id == TokenIdPercent) {
        Token *next_token = &pc->tokens->at(*token_index + 1);
        if (next_token->id == TokenIdKeywordDefer) {
            kind = ReturnKindError;
            node_type = NodeTypeDefer;
            *token_index += 2;
        } else {
            return nullptr;
        }
    } else if (token->id == TokenIdMaybe) {
        Token *next_token = &pc->tokens->at(*token_index + 1);
        if (next_token->id == TokenIdKeywordDefer) {
            kind = ReturnKindMaybe;
            node_type = NodeTypeDefer;
            *token_index += 2;
        } else {
            return nullptr;
        }
    } else if (token->id == TokenIdKeywordDefer) {
        kind = ReturnKindUnconditional;
        node_type = NodeTypeDefer;
        *token_index += 1;
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, node_type, token);
    node->data.defer.kind = kind;
    node->data.defer.expr = ast_parse_block_or_expression(pc, token_index, true);

    return node;
}

/*
VariableDeclaration = option("comptime") ("var" | "const") Symbol option(":" TypeExpr) "=" Expression
*/
static AstNode *ast_parse_variable_declaration_expr(ParseContext *pc, size_t *token_index, bool mandatory,
        VisibMod visib_mod)
{
    Token *first_token = &pc->tokens->at(*token_index);
    Token *var_token;

    bool is_const;
    bool is_comptime;
    if (first_token->id == TokenIdKeywordCompTime) {
        is_comptime = true;
        var_token = &pc->tokens->at(*token_index + 1);

        if (var_token->id == TokenIdKeywordVar) {
            is_const = false;
        } else if (var_token->id == TokenIdKeywordConst) {
            is_const = true;
        } else if (mandatory) {
            ast_invalid_token_error(pc, var_token);
        } else {
            return nullptr;
        }

        *token_index += 2;
    } else if (first_token->id == TokenIdKeywordVar) {
        is_comptime = false;
        is_const = false;
        var_token = first_token;
        *token_index += 1;
    } else if (first_token->id == TokenIdKeywordConst) {
        is_comptime = false;
        is_const = true;
        var_token = first_token;
        *token_index += 1;
    } else if (mandatory) {
        ast_invalid_token_error(pc, first_token);
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeVariableDeclaration, var_token);

    node->data.variable_declaration.is_inline = is_comptime;
    node->data.variable_declaration.is_const = is_const;
    node->data.variable_declaration.visib_mod = visib_mod;

    Token *name_token = ast_eat_token(pc, token_index, TokenIdSymbol);
    node->data.variable_declaration.symbol = token_buf(name_token);

    Token *eq_or_colon = &pc->tokens->at(*token_index);
    *token_index += 1;
    if (eq_or_colon->id == TokenIdEq) {
        node->data.variable_declaration.expr = ast_parse_expression(pc, token_index, true);
    } else if (eq_or_colon->id == TokenIdColon) {
        node->data.variable_declaration.type = ast_parse_type_expr(pc, token_index, true);
        Token *eq_token = &pc->tokens->at(*token_index);
        if (eq_token->id == TokenIdEq) {
            *token_index += 1;

            node->data.variable_declaration.expr = ast_parse_expression(pc, token_index, true);
        }
    } else {
        ast_invalid_token_error(pc, eq_or_colon);
    }

    // peek ahead and ensure that all variable declarations are followed by a semicolon
    Token *semicolon_token = &pc->tokens->at(*token_index);
    ast_expect_token(pc, semicolon_token, TokenIdSemicolon);

    return node;
}

/*
BoolOrExpression = BoolAndExpression "or" BoolOrExpression | BoolAndExpression
*/
static AstNode *ast_parse_bool_or_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *operand_1 = ast_parse_bool_and_expr(pc, token_index, mandatory);
    if (!operand_1)
        return nullptr;

    while (true) {
        Token *token = &pc->tokens->at(*token_index);
        if (token->id != TokenIdKeywordOr)
            return operand_1;
        *token_index += 1;

        AstNode *operand_2 = ast_parse_bool_and_expr(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = operand_1;
        node->data.bin_op_expr.bin_op = BinOpTypeBoolOr;
        node->data.bin_op_expr.op2 = operand_2;

        operand_1 = node;
    }
}

/*
WhileExpression(body) = option("inline") "while" "(" Expression option(";" Expression) ")" body
*/
static AstNode *ast_parse_while_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *first_token = &pc->tokens->at(*token_index);
    Token *while_token;

    bool is_inline;
    if (first_token->id == TokenIdKeywordInline) {
        while_token = &pc->tokens->at(*token_index + 1);
        if (while_token->id == TokenIdKeywordWhile) {
            is_inline = true;
            *token_index += 2;
        } else if (mandatory) {
            ast_expect_token(pc, while_token, TokenIdKeywordWhile);
            zig_unreachable();
        } else {
            return nullptr;
        }
    } else if (first_token->id == TokenIdKeywordWhile) {
        while_token = first_token;
        is_inline = false;
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, first_token, TokenIdKeywordWhile);
        zig_unreachable();
    } else {
        return nullptr;
    }
    AstNode *node = ast_create_node(pc, NodeTypeWhileExpr, while_token);
    node->data.while_expr.is_inline = is_inline;

    ast_eat_token(pc, token_index, TokenIdLParen);
    node->data.while_expr.condition = ast_parse_expression(pc, token_index, true);

    Token *semi_or_rparen = &pc->tokens->at(*token_index);

    if (semi_or_rparen->id == TokenIdRParen) {
        *token_index += 1;
        node->data.while_expr.body = ast_parse_block_or_expression(pc, token_index, true);
    } else if (semi_or_rparen->id == TokenIdSemicolon) {
        *token_index += 1;
        node->data.while_expr.continue_expr = ast_parse_expression(pc, token_index, true);
        ast_eat_token(pc, token_index, TokenIdRParen);
        node->data.while_expr.body = ast_parse_block_or_expression(pc, token_index, true);
    } else {
        ast_invalid_token_error(pc, semi_or_rparen);
    }


    return node;
}

static AstNode *ast_parse_symbol(ParseContext *pc, size_t *token_index) {
    Token *token = ast_eat_token(pc, token_index, TokenIdSymbol);
    AstNode *node = ast_create_node(pc, NodeTypeSymbol, token);
    node->data.symbol_expr.symbol = token_buf(token);
    return node;
}

/*
ForExpression(body) = option("inline") "for" "(" Expression ")" option("|" option("*") Symbol option("," Symbol) "|") body
*/
static AstNode *ast_parse_for_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *first_token = &pc->tokens->at(*token_index);
    Token *for_token;

    bool is_inline;
    if (first_token->id == TokenIdKeywordInline) {
        is_inline = true;
        for_token = &pc->tokens->at(*token_index + 1);
        if (for_token->id == TokenIdKeywordFor) {
            *token_index += 2;
        } else if (mandatory) {
            ast_expect_token(pc, first_token, TokenIdKeywordFor);
            zig_unreachable();
        } else {
            return nullptr;
        }
    } else if (first_token->id == TokenIdKeywordFor) {
        for_token = first_token;
        is_inline = false;
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, first_token, TokenIdKeywordFor);
        zig_unreachable();
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeForExpr, for_token);
    node->data.for_expr.is_inline = is_inline;

    ast_eat_token(pc, token_index, TokenIdLParen);
    node->data.for_expr.array_expr = ast_parse_expression(pc, token_index, true);
    ast_eat_token(pc, token_index, TokenIdRParen);

    Token *maybe_bar = &pc->tokens->at(*token_index);
    if (maybe_bar->id == TokenIdBinOr) {
        *token_index += 1;

        Token *maybe_star = &pc->tokens->at(*token_index);
        if (maybe_star->id == TokenIdStar) {
            *token_index += 1;
            node->data.for_expr.elem_is_ptr = true;
        }

        node->data.for_expr.elem_node = ast_parse_symbol(pc, token_index);

        Token *maybe_comma = &pc->tokens->at(*token_index);
        if (maybe_comma->id == TokenIdComma) {
            *token_index += 1;

            node->data.for_expr.index_node = ast_parse_symbol(pc, token_index);
        }

        ast_eat_token(pc, token_index, TokenIdBinOr);
    }

    node->data.for_expr.body = ast_parse_block_or_expression(pc, token_index, true);

    return node;
}

/*
SwitchExpression = "switch" "(" Expression ")" "{" many(SwitchProng) "}"
SwitchProng = (list(SwitchItem, ",") | "else") "=>" option("|" option("*") Symbol "|") Expression ","
SwitchItem : Expression | (Expression "..." Expression)
*/
static AstNode *ast_parse_switch_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *switch_token = &pc->tokens->at(*token_index);
    if (switch_token->id == TokenIdKeywordSwitch) {
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, switch_token, TokenIdKeywordSwitch);
        zig_unreachable();
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeSwitchExpr, switch_token);

    ast_eat_token(pc, token_index, TokenIdLParen);
    node->data.switch_expr.expr = ast_parse_expression(pc, token_index, true);
    ast_eat_token(pc, token_index, TokenIdRParen);
    ast_eat_token(pc, token_index, TokenIdLBrace);

    for (;;) {
        Token *token = &pc->tokens->at(*token_index);

        if (token->id == TokenIdRBrace) {
            *token_index += 1;

            return node;
        }

        AstNode *prong_node = ast_create_node(pc, NodeTypeSwitchProng, token);
        node->data.switch_expr.prongs.append(prong_node);

        if (token->id == TokenIdKeywordElse) {
            *token_index += 1;
        } else for (;;) {
            AstNode *expr1 = ast_parse_expression(pc, token_index, true);
            Token *ellipsis_tok = &pc->tokens->at(*token_index);
            if (ellipsis_tok->id == TokenIdEllipsis) {
                *token_index += 1;

                AstNode *range_node = ast_create_node(pc, NodeTypeSwitchRange, ellipsis_tok);
                prong_node->data.switch_prong.items.append(range_node);

                range_node->data.switch_range.start = expr1;
                range_node->data.switch_range.end = ast_parse_expression(pc, token_index, true);

                prong_node->data.switch_prong.any_items_are_range = true;
            } else {
                prong_node->data.switch_prong.items.append(expr1);
            }
            Token *comma_tok = &pc->tokens->at(*token_index);
            if (comma_tok->id == TokenIdComma) {
                *token_index += 1;
                continue;
            }
            break;
        }

        ast_eat_token(pc, token_index, TokenIdFatArrow);

        Token *maybe_bar = &pc->tokens->at(*token_index);
        if (maybe_bar->id == TokenIdBinOr) {
            *token_index += 1;

            Token *star_or_symbol = &pc->tokens->at(*token_index);
            AstNode *var_symbol_node;
            bool var_is_ptr;
            if (star_or_symbol->id == TokenIdStar) {
                *token_index += 1;
                var_is_ptr = true;
                var_symbol_node = ast_parse_symbol(pc, token_index);
            } else {
                var_is_ptr = false;
                var_symbol_node = ast_parse_symbol(pc, token_index);
            }

            prong_node->data.switch_prong.var_symbol = var_symbol_node;
            prong_node->data.switch_prong.var_is_ptr = var_is_ptr;
            ast_eat_token(pc, token_index, TokenIdBinOr);
        }

        prong_node->data.switch_prong.expr = ast_parse_expression(pc, token_index, true);
        ast_eat_token(pc, token_index, TokenIdComma);

    }
}

/*
BlockExpression(body) = Block | IfExpression(body) | TryExpression(body) | WhileExpression(body) | ForExpression(body) | SwitchExpression | CompTimeExpression(body)
*/
static AstNode *ast_parse_block_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);

    AstNode *if_expr = ast_parse_if_expr(pc, token_index, false);
    if (if_expr)
        return if_expr;

    AstNode *while_expr = ast_parse_while_expr(pc, token_index, false);
    if (while_expr)
        return while_expr;

    AstNode *for_expr = ast_parse_for_expr(pc, token_index, false);
    if (for_expr)
        return for_expr;

    AstNode *switch_expr = ast_parse_switch_expr(pc, token_index, false);
    if (switch_expr)
        return switch_expr;

    AstNode *block = ast_parse_block(pc, token_index, false);
    if (block)
        return block;

    AstNode *comptime_node = ast_parse_comptime_expr(pc, token_index, false, false);
    if (comptime_node)
        return comptime_node;

    AstNode *try_node = ast_parse_try_expr(pc, token_index, false);
    if (try_node)
        return try_node;

    if (mandatory)
        ast_invalid_token_error(pc, token);

    return nullptr;
}

static BinOpType tok_to_ass_op(Token *token) {
    switch (token->id) {
        case TokenIdEq: return BinOpTypeAssign;
        case TokenIdTimesEq: return BinOpTypeAssignTimes;
        case TokenIdTimesPercentEq: return BinOpTypeAssignTimesWrap;
        case TokenIdDivEq: return BinOpTypeAssignDiv;
        case TokenIdModEq: return BinOpTypeAssignMod;
        case TokenIdPlusEq: return BinOpTypeAssignPlus;
        case TokenIdPlusPercentEq: return BinOpTypeAssignPlusWrap;
        case TokenIdMinusEq: return BinOpTypeAssignMinus;
        case TokenIdMinusPercentEq: return BinOpTypeAssignMinusWrap;
        case TokenIdBitShiftLeftEq: return BinOpTypeAssignBitShiftLeft;
        case TokenIdBitShiftLeftPercentEq: return BinOpTypeAssignBitShiftLeftWrap;
        case TokenIdBitShiftRightEq: return BinOpTypeAssignBitShiftRight;
        case TokenIdBitAndEq: return BinOpTypeAssignBitAnd;
        case TokenIdBitXorEq: return BinOpTypeAssignBitXor;
        case TokenIdBitOrEq: return BinOpTypeAssignBitOr;
        default: return BinOpTypeInvalid;
    }
}

/*
AssignmentOperator = "=" | "*=" | "/=" | "%=" | "+=" | "-=" | "<<=" | ">>=" | "&=" | "^=" | "|=" | "*%=" | "+%=" | "-%=" | "<<%="
*/
static BinOpType ast_parse_ass_op(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);
    BinOpType result = tok_to_ass_op(token);
    if (result == BinOpTypeInvalid) {
        if (mandatory) {
            ast_invalid_token_error(pc, token);
        } else {
            return BinOpTypeInvalid;
        }
    }
    *token_index += 1;
    return result;
}

/*
UnwrapExpression : BoolOrExpression (UnwrapMaybe | UnwrapError) | BoolOrExpression
UnwrapMaybe : "??" BoolOrExpression
UnwrapError : "%%" option("|" "Symbol" "|") BoolOrExpression
*/
static AstNode *ast_parse_unwrap_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *lhs = ast_parse_bool_or_expr(pc, token_index, mandatory);
    if (!lhs)
        return nullptr;

    Token *token = &pc->tokens->at(*token_index);

    if (token->id == TokenIdDoubleQuestion) {
        *token_index += 1;

        AstNode *rhs = ast_parse_expression(pc, token_index, true);

        AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
        node->data.bin_op_expr.op1 = lhs;
        node->data.bin_op_expr.bin_op = BinOpTypeUnwrapMaybe;
        node->data.bin_op_expr.op2 = rhs;

        return node;
    } else if (token->id == TokenIdPercentPercent) {
        *token_index += 1;

        AstNode *node = ast_create_node(pc, NodeTypeUnwrapErrorExpr, token);
        node->data.unwrap_err_expr.op1 = lhs;

        Token *maybe_bar_tok = &pc->tokens->at(*token_index);
        if (maybe_bar_tok->id == TokenIdBinOr) {
            *token_index += 1;
            node->data.unwrap_err_expr.symbol = ast_parse_symbol(pc, token_index);
            ast_eat_token(pc, token_index, TokenIdBinOr);
        }
        node->data.unwrap_err_expr.op2 = ast_parse_expression(pc, token_index, true);

        return node;
    } else {
        return lhs;
    }
}

/*
AssignmentExpression : UnwrapExpression AssignmentOperator UnwrapExpression | UnwrapExpression
*/
static AstNode *ast_parse_ass_expr(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *lhs = ast_parse_unwrap_expr(pc, token_index, mandatory);
    if (!lhs)
        return nullptr;

    Token *token = &pc->tokens->at(*token_index);
    BinOpType ass_op = ast_parse_ass_op(pc, token_index, false);
    if (ass_op == BinOpTypeInvalid)
        return lhs;

    AstNode *rhs = ast_parse_unwrap_expr(pc, token_index, true);

    AstNode *node = ast_create_node(pc, NodeTypeBinOpExpr, token);
    node->data.bin_op_expr.op1 = lhs;
    node->data.bin_op_expr.bin_op = ass_op;
    node->data.bin_op_expr.op2 = rhs;

    return node;
}

static AstNode *ast_parse_block_expr_or_expression(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *block_expr = ast_parse_block_expr(pc, token_index, false);
    if (block_expr)
        return block_expr;

    return ast_parse_expression(pc, token_index, mandatory);
}

/*
BlockOrExpression = Block | Expression
*/
static AstNode *ast_parse_block_or_expression(ParseContext *pc, size_t *token_index, bool mandatory) {
    AstNode *block_expr = ast_parse_block(pc, token_index, false);
    if (block_expr)
        return block_expr;

    return ast_parse_expression(pc, token_index, mandatory);
}

/*
Expression = ReturnExpression | AssignmentExpression
*/
static AstNode *ast_parse_expression(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *token = &pc->tokens->at(*token_index);

    AstNode *return_expr = ast_parse_return_expr(pc, token_index);
    if (return_expr)
        return return_expr;

    AstNode *ass_expr = ast_parse_ass_expr(pc, token_index, false);
    if (ass_expr)
        return ass_expr;

    if (mandatory)
        ast_invalid_token_error(pc, token);

    return nullptr;
}

/*
Label: token(Symbol) token(Colon)
*/
static AstNode *ast_parse_label(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *symbol_token = &pc->tokens->at(*token_index);
    if (symbol_token->id != TokenIdSymbol) {
        if (mandatory) {
            ast_expect_token(pc, symbol_token, TokenIdSymbol);
        } else {
            return nullptr;
        }
    }

    Token *colon_token = &pc->tokens->at(*token_index + 1);
    if (colon_token->id != TokenIdColon) {
        if (mandatory) {
            ast_expect_token(pc, colon_token, TokenIdColon);
        } else {
            return nullptr;
        }
    }

    *token_index += 2;

    AstNode *node = ast_create_node(pc, NodeTypeLabel, symbol_token);
    node->data.label.name = token_buf(symbol_token);
    return node;
}

static bool statement_terminates_without_semicolon(AstNode *node) {
    switch (node->type) {
        case NodeTypeIfBoolExpr:
            if (node->data.if_bool_expr.else_node)
                return statement_terminates_without_semicolon(node->data.if_bool_expr.else_node);
            return node->data.if_bool_expr.then_block->type == NodeTypeBlock;
        case NodeTypeIfVarExpr:
            if (node->data.if_var_expr.else_node)
                return statement_terminates_without_semicolon(node->data.if_var_expr.else_node);
            return node->data.if_var_expr.then_block->type == NodeTypeBlock;
        case NodeTypeTryExpr:
            if (node->data.try_expr.else_node)
                return statement_terminates_without_semicolon(node->data.try_expr.else_node);
            return node->data.try_expr.then_node->type == NodeTypeBlock;
        case NodeTypeWhileExpr:
            return node->data.while_expr.body->type == NodeTypeBlock;
        case NodeTypeForExpr:
            return node->data.for_expr.body->type == NodeTypeBlock;
        case NodeTypeCompTime:
            return node->data.comptime_expr.expr->type == NodeTypeBlock;
        case NodeTypeDefer:
            return node->data.defer.expr->type == NodeTypeBlock;
        case NodeTypeSwitchExpr:
        case NodeTypeBlock:
        case NodeTypeLabel:
            return true;
        default:
            return false;
    }
}

/*
Block = "{" many(Statement) option(Expression) "}"
Statement = Label | VariableDeclaration ";" | Defer(Block) | Defer(Expression) ";" | BlockExpression(Block) | Expression ";" | ";"
*/
static AstNode *ast_parse_block(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *last_token = &pc->tokens->at(*token_index);

    if (last_token->id != TokenIdLBrace) {
        if (mandatory) {
            ast_expect_token(pc, last_token, TokenIdLBrace);
        } else {
            return nullptr;
        }
    }
    *token_index += 1;

    AstNode *node = ast_create_node(pc, NodeTypeBlock, last_token);

    for (;;) {
        AstNode *statement_node = ast_parse_label(pc, token_index, false);
        if (!statement_node)
            statement_node = ast_parse_variable_declaration_expr(pc, token_index, false, VisibModPrivate);
        if (!statement_node)
            statement_node = ast_parse_defer_expr(pc, token_index);
        if (!statement_node)
            statement_node = ast_parse_block_expr(pc, token_index, false);
        if (!statement_node)
            statement_node = ast_parse_expression(pc, token_index, false);

        bool semicolon_expected = true;
        if (statement_node) {
            node->data.block.statements.append(statement_node);
            if (statement_terminates_without_semicolon(statement_node)) {
                semicolon_expected = false;
            } else {
                if (statement_node->type == NodeTypeDefer) {
                    // defer without a block body requires a semicolon
                    Token *token = &pc->tokens->at(*token_index);
                    ast_expect_token(pc, token, TokenIdSemicolon);
                }
            }
        }

        node->data.block.last_statement_is_result_expression = statement_node && !(
            statement_node->type == NodeTypeLabel ||
            statement_node->type == NodeTypeDefer);

        last_token = &pc->tokens->at(*token_index);
        if (last_token->id == TokenIdRBrace) {
            *token_index += 1;
            return node;
        } else if (!semicolon_expected) {
            continue;
        } else if (last_token->id == TokenIdSemicolon) {
            *token_index += 1;
        } else {
            ast_invalid_token_error(pc, last_token);
        }
    }
    zig_unreachable();
}

/*
FnProto = option("coldcc" | "nakedcc") "fn" option(Symbol) ParamDeclList option("->" TypeExpr)
*/
static AstNode *ast_parse_fn_proto(ParseContext *pc, size_t *token_index, bool mandatory, VisibMod visib_mod) {
    Token *first_token = &pc->tokens->at(*token_index);
    Token *fn_token;

    bool is_coldcc = false;
    bool is_nakedcc = false;
    if (first_token->id == TokenIdKeywordColdCC) {
        *token_index += 1;
        fn_token = ast_eat_token(pc, token_index, TokenIdKeywordFn);
        is_coldcc = true;
    } else if (first_token->id == TokenIdKeywordNakedCC) {
        *token_index += 1;
        fn_token = ast_eat_token(pc, token_index, TokenIdKeywordFn);
        is_nakedcc = true;
    } else if (first_token->id == TokenIdKeywordFn) {
        fn_token = first_token;
        *token_index += 1;
    } else if (mandatory) {
        ast_expect_token(pc, first_token, TokenIdKeywordFn);
        zig_unreachable();
    } else {
        return nullptr;
    }

    AstNode *node = ast_create_node(pc, NodeTypeFnProto, fn_token);
    node->data.fn_proto.visib_mod = visib_mod;
    node->data.fn_proto.is_coldcc = is_coldcc;
    node->data.fn_proto.is_nakedcc = is_nakedcc;

    Token *fn_name = &pc->tokens->at(*token_index);
    if (fn_name->id == TokenIdSymbol) {
        *token_index += 1;
        node->data.fn_proto.name = token_buf(fn_name);
    } else {
        node->data.fn_proto.name = pc->empty_buf;
    }

    ast_parse_param_decl_list(pc, token_index, &node->data.fn_proto.params, &node->data.fn_proto.is_var_args);

    Token *next_token = &pc->tokens->at(*token_index);
    if (next_token->id == TokenIdArrow) {
        *token_index += 1;
        node->data.fn_proto.return_type = ast_parse_type_expr(pc, token_index, false);
    } else {
        node->data.fn_proto.return_type = ast_create_void_type_node(pc, next_token);
    }

    return node;
}

/*
FnDef = option("inline" | "extern") FnProto Block
*/
static AstNode *ast_parse_fn_def(ParseContext *pc, size_t *token_index, bool mandatory, VisibMod visib_mod) {
    Token *first_token = &pc->tokens->at(*token_index);
    bool is_inline;
    bool is_extern;
    if (first_token->id == TokenIdKeywordInline) {
        *token_index += 1;
        is_inline = true;
        is_extern = false;
    } else if (first_token->id == TokenIdKeywordExtern) {
        *token_index += 1;
        is_extern = true;
        is_inline = false;
    } else {
        is_inline = false;
        is_extern = false;
    }

    AstNode *fn_proto = ast_parse_fn_proto(pc, token_index, mandatory, visib_mod);
    if (!fn_proto) {
        if (is_inline || is_extern) {
            *token_index -= 1;
        }
        return nullptr;
    }

    fn_proto->data.fn_proto.is_inline = is_inline;
    fn_proto->data.fn_proto.is_extern = is_extern;

    Token *semi_token = &pc->tokens->at(*token_index);
    if (semi_token->id == TokenIdSemicolon) {
        *token_index += 1;
        return fn_proto;
    }

    AstNode *node = ast_create_node(pc, NodeTypeFnDef, first_token);
    node->data.fn_def.fn_proto = fn_proto;
    node->data.fn_def.body = ast_parse_block(pc, token_index, true);
    fn_proto->data.fn_proto.fn_def_node = node;
    return node;
}

/*
ExternDecl = "extern" (FnProto | VariableDeclaration) ";"
*/
static AstNode *ast_parse_extern_decl(ParseContext *pc, size_t *token_index, bool mandatory, VisibMod visib_mod) {
    Token *extern_kw = &pc->tokens->at(*token_index);
    if (extern_kw->id != TokenIdKeywordExtern) {
        if (mandatory) {
            ast_expect_token(pc, extern_kw, TokenIdKeywordExtern);
        } else {
            return nullptr;
        }
    }
    *token_index += 1;

    AstNode *fn_proto_node = ast_parse_fn_proto(pc, token_index, false, visib_mod);
    if (fn_proto_node) {
        ast_eat_token(pc, token_index, TokenIdSemicolon);

        fn_proto_node->data.fn_proto.is_extern = true;

        return fn_proto_node;
    }

    AstNode *var_decl_node = ast_parse_variable_declaration_expr(pc, token_index, false, visib_mod);
    if (var_decl_node) {
        ast_eat_token(pc, token_index, TokenIdSemicolon);

        var_decl_node->data.variable_declaration.is_extern = true;

        return var_decl_node;
    }

    Token *token = &pc->tokens->at(*token_index);
    ast_invalid_token_error(pc, token);
}

/*
UseDecl = "use" Expression ";"
*/
static AstNode *ast_parse_use(ParseContext *pc, size_t *token_index, VisibMod visib_mod) {
    Token *use_kw = &pc->tokens->at(*token_index);
    if (use_kw->id != TokenIdKeywordUse)
        return nullptr;
    *token_index += 1;

    AstNode *node = ast_create_node(pc, NodeTypeUse, use_kw);
    node->data.use.visib_mod = visib_mod;
    node->data.use.expr = ast_parse_expression(pc, token_index, true);

    ast_eat_token(pc, token_index, TokenIdSemicolon);

    return node;
}

/*
ContainerDecl = option("extern" | "packed") ("struct" | "enum" | "union") "{" many(ContainerMember) "}"
ContainerMember = (ContainerField | FnDef | GlobalVarDecl)
ContainerField = Symbol option(":" Expression) ","
*/
static AstNode *ast_parse_container_decl(ParseContext *pc, size_t *token_index, bool mandatory) {
    Token *first_token = &pc->tokens->at(*token_index);
    Token *container_kind_token;

    ContainerLayout layout;
    if (first_token->id == TokenIdKeywordExtern) {
        container_kind_token = &pc->tokens->at(*token_index + 1);
        layout = ContainerLayoutExtern;
    } else if (first_token->id == TokenIdKeywordPacked) {
        container_kind_token = &pc->tokens->at(*token_index + 1);
        layout = ContainerLayoutPacked;
    } else {
        container_kind_token = first_token;
        layout = ContainerLayoutAuto;
    }

    ContainerKind kind;
    if (container_kind_token->id == TokenIdKeywordStruct) {
        kind = ContainerKindStruct;
    } else if (container_kind_token->id == TokenIdKeywordEnum) {
        kind = ContainerKindEnum;
    } else if (container_kind_token->id == TokenIdKeywordUnion) {
        kind = ContainerKindUnion;
    } else if (mandatory) {
        ast_invalid_token_error(pc, container_kind_token);
    } else {
        return nullptr;
    }
    *token_index += (layout == ContainerLayoutAuto) ? 1 : 2;

    AstNode *node = ast_create_node(pc, NodeTypeContainerDecl, first_token);
    node->data.container_decl.layout = layout;
    node->data.container_decl.kind = kind;

    ast_eat_token(pc, token_index, TokenIdLBrace);

    for (;;) {
        Token *visib_tok = &pc->tokens->at(*token_index);
        VisibMod visib_mod;
        if (visib_tok->id == TokenIdKeywordPub) {
            *token_index += 1;
            visib_mod = VisibModPub;
        } else if (visib_tok->id == TokenIdKeywordExport) {
            *token_index += 1;
            visib_mod = VisibModExport;
        } else {
            visib_mod = VisibModPrivate;
        }

        AstNode *fn_def_node = ast_parse_fn_def(pc, token_index, false, visib_mod);
        if (fn_def_node) {
            node->data.container_decl.decls.append(fn_def_node);
            continue;
        }

        AstNode *var_decl_node = ast_parse_variable_declaration_expr(pc, token_index, false, visib_mod);
        if (var_decl_node) {
            ast_eat_token(pc, token_index, TokenIdSemicolon);
            node->data.container_decl.decls.append(var_decl_node);
            continue;
        }

        Token *token = &pc->tokens->at(*token_index);

        if (token->id == TokenIdRBrace) {
            *token_index += 1;
            break;
        } else if (token->id == TokenIdSymbol) {
            AstNode *field_node = ast_create_node(pc, NodeTypeStructField, token);
            *token_index += 1;

            field_node->data.struct_field.visib_mod = visib_mod;
            field_node->data.struct_field.name = token_buf(token);

            Token *expr_or_comma = &pc->tokens->at(*token_index);
            if (expr_or_comma->id == TokenIdComma) {
                field_node->data.struct_field.type = ast_create_void_type_node(pc, expr_or_comma);
                *token_index += 1;
            } else {
                ast_eat_token(pc, token_index, TokenIdColon);
                field_node->data.struct_field.type = ast_parse_expression(pc, token_index, true);
                ast_eat_token(pc, token_index, TokenIdComma);
            }

            node->data.container_decl.fields.append(field_node);
        } else {
            ast_invalid_token_error(pc, token);
        }
    }

    return node;
}

/*
ErrorValueDecl : "error" "Symbol" ";"
*/
static AstNode *ast_parse_error_value_decl(ParseContext *pc, size_t *token_index) {
    Token *first_token = &pc->tokens->at(*token_index);

    if (first_token->id != TokenIdKeywordError) {
        return nullptr;
    }
    *token_index += 1;

    Token *name_tok = ast_eat_token(pc, token_index, TokenIdSymbol);
    ast_eat_token(pc, token_index, TokenIdSemicolon);

    AstNode *node = ast_create_node(pc, NodeTypeErrorValueDecl, first_token);
    node->data.error_value_decl.name = token_buf(name_tok);

    return node;
}

/*
TestDecl = "test" String Block
*/
static AstNode *ast_parse_test_decl_node(ParseContext *pc, size_t *token_index) {
    Token *first_token = &pc->tokens->at(*token_index);

    if (first_token->id != TokenIdKeywordTest) {
        return nullptr;
    }
    *token_index += 1;

    Token *name_tok = ast_eat_token(pc, token_index, TokenIdStringLiteral);

    AstNode *node = ast_create_node(pc, NodeTypeTestDecl, first_token);
    node->data.test_decl.name = token_buf(name_tok);
    node->data.test_decl.body = ast_parse_block(pc, token_index, true);

    return node;
}

/*
TopLevelItem = ErrorValueDecl | CompTimeExpression(Block) | TopLevelDecl | TestDecl
TopLevelDecl = option(VisibleMod) (FnDef | ExternDecl | GlobalVarDecl | UseDecl)
*/
static void ast_parse_top_level_decls(ParseContext *pc, size_t *token_index, ZigList<AstNode *> *top_level_decls) {
    for (;;) {
        AstNode *comptime_expr_node = ast_parse_comptime_expr(pc, token_index, true, false);
        if (comptime_expr_node) {
            top_level_decls->append(comptime_expr_node);
            continue;
        }

        AstNode *error_value_node = ast_parse_error_value_decl(pc, token_index);
        if (error_value_node) {
            top_level_decls->append(error_value_node);
            continue;
        }

        AstNode *test_decl_node = ast_parse_test_decl_node(pc, token_index);
        if (test_decl_node) {
            top_level_decls->append(test_decl_node);
            continue;
        }

        Token *visib_tok = &pc->tokens->at(*token_index);
        VisibMod visib_mod;
        if (visib_tok->id == TokenIdKeywordPub) {
            *token_index += 1;
            visib_mod = VisibModPub;
        } else if (visib_tok->id == TokenIdKeywordExport) {
            *token_index += 1;
            visib_mod = VisibModExport;
        } else {
            visib_mod = VisibModPrivate;
        }

        AstNode *fn_def_node = ast_parse_fn_def(pc, token_index, false, visib_mod);
        if (fn_def_node) {
            top_level_decls->append(fn_def_node);
            continue;
        }

        AstNode *fn_proto_node = ast_parse_extern_decl(pc, token_index, false, visib_mod);
        if (fn_proto_node) {
            top_level_decls->append(fn_proto_node);
            continue;
        }

        AstNode *use_node = ast_parse_use(pc, token_index, visib_mod);
        if (use_node) {
            top_level_decls->append(use_node);
            continue;
        }

        AstNode *var_decl_node = ast_parse_variable_declaration_expr(pc, token_index, false, visib_mod);
        if (var_decl_node) {
            ast_eat_token(pc, token_index, TokenIdSemicolon);
            top_level_decls->append(var_decl_node);
            continue;
        }

        return;
    }
    zig_unreachable();
}

/*
Root = many(TopLevelItem) "EOF"
 */
static AstNode *ast_parse_root(ParseContext *pc, size_t *token_index) {
    AstNode *node = ast_create_node(pc, NodeTypeRoot, &pc->tokens->at(*token_index));

    ast_parse_top_level_decls(pc, token_index, &node->data.root.top_level_decls);

    if (*token_index != pc->tokens->length - 1) {
        ast_invalid_token_error(pc, &pc->tokens->at(*token_index));
    }

    return node;
}

AstNode *ast_parse(Buf *buf, ZigList<Token> *tokens, ImportTableEntry *owner,
        ErrColor err_color, uint32_t *next_node_index)
{
    ParseContext pc = {0};
    pc.void_buf = buf_create_from_str("void");
    pc.empty_buf = buf_create_from_str("");
    pc.err_color = err_color;
    pc.owner = owner;
    pc.buf = buf;
    pc.tokens = tokens;
    pc.next_node_index = next_node_index;
    size_t token_index = 0;
    pc.root = ast_parse_root(&pc, &token_index);
    return pc.root;
}

static void visit_field(AstNode **node, void (*visit)(AstNode **, void *context), void *context) {
    if (*node) {
        visit(node, context);
    }
}

static void visit_node_list(ZigList<AstNode *> *list, void (*visit)(AstNode **, void *context), void *context) {
    if (list) {
        for (size_t i = 0; i < list->length; i += 1) {
            visit(&list->at(i), context);
        }
    }
}

void ast_visit_node_children(AstNode *node, void (*visit)(AstNode **, void *context), void *context) {
    switch (node->type) {
        case NodeTypeRoot:
            visit_node_list(&node->data.root.top_level_decls, visit, context);
            break;
        case NodeTypeFnProto:
            visit_field(&node->data.fn_proto.return_type, visit, context);
            visit_node_list(&node->data.fn_proto.params, visit, context);
            break;
        case NodeTypeFnDef:
            visit_field(&node->data.fn_def.fn_proto, visit, context);
            visit_field(&node->data.fn_def.body, visit, context);
            break;
        case NodeTypeFnDecl:
            visit_field(&node->data.fn_decl.fn_proto, visit, context);
            break;
        case NodeTypeParamDecl:
            visit_field(&node->data.param_decl.type, visit, context);
            break;
        case NodeTypeBlock:
            visit_node_list(&node->data.block.statements, visit, context);
            break;
        case NodeTypeGroupedExpr:
            visit_field(&node->data.grouped_expr, visit, context);
            break;
        case NodeTypeReturnExpr:
            visit_field(&node->data.return_expr.expr, visit, context);
            break;
        case NodeTypeDefer:
            visit_field(&node->data.defer.expr, visit, context);
            break;
        case NodeTypeVariableDeclaration:
            visit_field(&node->data.variable_declaration.type, visit, context);
            visit_field(&node->data.variable_declaration.expr, visit, context);
            break;
        case NodeTypeErrorValueDecl:
            // none
            break;
        case NodeTypeTestDecl:
            visit_field(&node->data.test_decl.body, visit, context);
            break;
        case NodeTypeBinOpExpr:
            visit_field(&node->data.bin_op_expr.op1, visit, context);
            visit_field(&node->data.bin_op_expr.op2, visit, context);
            break;
        case NodeTypeUnwrapErrorExpr:
            visit_field(&node->data.unwrap_err_expr.op1, visit, context);
            visit_field(&node->data.unwrap_err_expr.symbol, visit, context);
            visit_field(&node->data.unwrap_err_expr.op2, visit, context);
            break;
        case NodeTypeNumberLiteral:
            // none
            break;
        case NodeTypeStringLiteral:
            // none
            break;
        case NodeTypeCharLiteral:
            // none
            break;
        case NodeTypeSymbol:
            // none
            break;
        case NodeTypePrefixOpExpr:
            visit_field(&node->data.prefix_op_expr.primary_expr, visit, context);
            break;
        case NodeTypeFnCallExpr:
            visit_field(&node->data.fn_call_expr.fn_ref_expr, visit, context);
            visit_node_list(&node->data.fn_call_expr.params, visit, context);
            break;
        case NodeTypeArrayAccessExpr:
            visit_field(&node->data.array_access_expr.array_ref_expr, visit, context);
            visit_field(&node->data.array_access_expr.subscript, visit, context);
            break;
        case NodeTypeSliceExpr:
            visit_field(&node->data.slice_expr.array_ref_expr, visit, context);
            visit_field(&node->data.slice_expr.start, visit, context);
            visit_field(&node->data.slice_expr.end, visit, context);
            break;
        case NodeTypeFieldAccessExpr:
            visit_field(&node->data.field_access_expr.struct_expr, visit, context);
            break;
        case NodeTypeUse:
            visit_field(&node->data.use.expr, visit, context);
            break;
        case NodeTypeBoolLiteral:
            // none
            break;
        case NodeTypeNullLiteral:
            // none
            break;
        case NodeTypeUndefinedLiteral:
            // none
            break;
        case NodeTypeThisLiteral:
            // none
            break;
        case NodeTypeIfBoolExpr:
            visit_field(&node->data.if_bool_expr.condition, visit, context);
            visit_field(&node->data.if_bool_expr.then_block, visit, context);
            visit_field(&node->data.if_bool_expr.else_node, visit, context);
            break;
        case NodeTypeIfVarExpr:
            visit_field(&node->data.if_var_expr.var_decl.type, visit, context);
            visit_field(&node->data.if_var_expr.var_decl.expr, visit, context);
            visit_field(&node->data.if_var_expr.then_block, visit, context);
            visit_field(&node->data.if_var_expr.else_node, visit, context);
            break;
        case NodeTypeTryExpr:
            visit_field(&node->data.try_expr.target_node, visit, context);
            visit_field(&node->data.try_expr.then_node, visit, context);
            visit_field(&node->data.try_expr.else_node, visit, context);
            break;
        case NodeTypeWhileExpr:
            visit_field(&node->data.while_expr.condition, visit, context);
            visit_field(&node->data.while_expr.body, visit, context);
            break;
        case NodeTypeForExpr:
            visit_field(&node->data.for_expr.elem_node, visit, context);
            visit_field(&node->data.for_expr.array_expr, visit, context);
            visit_field(&node->data.for_expr.index_node, visit, context);
            visit_field(&node->data.for_expr.body, visit, context);
            break;
        case NodeTypeSwitchExpr:
            visit_field(&node->data.switch_expr.expr, visit, context);
            visit_node_list(&node->data.switch_expr.prongs, visit, context);
            break;
        case NodeTypeSwitchProng:
            visit_node_list(&node->data.switch_prong.items, visit, context);
            visit_field(&node->data.switch_prong.var_symbol, visit, context);
            visit_field(&node->data.switch_prong.expr, visit, context);
            break;
        case NodeTypeSwitchRange:
            visit_field(&node->data.switch_range.start, visit, context);
            visit_field(&node->data.switch_range.end, visit, context);
            break;
        case NodeTypeLabel:
            // none
            break;
        case NodeTypeGoto:
            // none
            break;
        case NodeTypeCompTime:
            visit_field(&node->data.comptime_expr.expr, visit, context);
            break;
        case NodeTypeBreak:
            // none
            break;
        case NodeTypeContinue:
            // none
            break;
        case NodeTypeUnreachable:
            // none
            break;
        case NodeTypeAsmExpr:
            for (size_t i = 0; i < node->data.asm_expr.input_list.length; i += 1) {
                AsmInput *asm_input = node->data.asm_expr.input_list.at(i);
                visit_field(&asm_input->expr, visit, context);
            }
            for (size_t i = 0; i < node->data.asm_expr.output_list.length; i += 1) {
                AsmOutput *asm_output = node->data.asm_expr.output_list.at(i);
                visit_field(&asm_output->return_type, visit, context);
            }
            break;
        case NodeTypeContainerDecl:
            visit_node_list(&node->data.container_decl.fields, visit, context);
            visit_node_list(&node->data.container_decl.decls, visit, context);
            break;
        case NodeTypeStructField:
            visit_field(&node->data.struct_field.type, visit, context);
            break;
        case NodeTypeContainerInitExpr:
            visit_field(&node->data.container_init_expr.type, visit, context);
            visit_node_list(&node->data.container_init_expr.entries, visit, context);
            break;
        case NodeTypeStructValueField:
            visit_field(&node->data.struct_val_field.expr, visit, context);
            break;
        case NodeTypeArrayType:
            visit_field(&node->data.array_type.size, visit, context);
            visit_field(&node->data.array_type.child_type, visit, context);
            break;
        case NodeTypeErrorType:
            // none
            break;
        case NodeTypeVarLiteral:
            // none
            break;
        case NodeTypeInlineExpr:
            visit_field(&node->data.inline_expr.body, visit, context);
    }
}
