#include "include/common.h"
typedef enum {
    T_OBJ_BEGIN,
    T_OBJ_END,
    T_ARR_BEGIN,
    T_ARR_END,
    T_STRING,
    T_NUMBER,
    T_BOOLEAN,
    T_NULL,
    T_COLON,
    T_COMMA,
    T_END,
    T_WHITESPACE,
    T_ERROR,
    T_UNKNOWN
} ckv3_token_type_t;

static const char *ckv3_token_type_name[] = {
    "T_OBJ_BEGIN",
    "T_OBJ_END",
    "T_ARR_BEGIN",
    "T_ARR_END",
    "T_STRING",
    "T_NUMBER",
    "T_BOOLEAN",
    "T_NULL",
    "T_COLON",
    "T_COMMA",
    "T_END",
    "T_WHITESPACE",
    "T_ERROR",
    "T_UNKNOWN",
    NULL
};

static const char* ckv3_array_struct[] = {
    "vector2",
    "vector3",
    "vector4",
    "vector2_array",
    "vector3_array",
    "vector4_array",
    "quaternion",
    "quaternion_array",
    "time_array",
    NULL
};

typedef struct {
    ckv3_token_type_t ch2token[256];
    char escape2char[256];  /* Decoding */

    /* encode_buf is only allocated and used when
     * encode_keep_buffer is set */
    strbuf_t encode_buf;

    int encode_sparse_convert;
    int encode_sparse_ratio;
    int encode_sparse_safe;
    int encode_max_depth;
    int encode_invalid_numbers;     /* 2 => Encode as "null" */
    int encode_number_precision;
    int encode_keep_buffer;

    int decode_invalid_numbers;
    int decode_max_depth;
} ckv3_config_t;

typedef struct {
    const char *data;
    const char *ptr;
    strbuf_t *tmp;    /* Temporary storage for strings */
    ckv3_config_t *cfg;
    int current_depth;
} ckv3_parse_t;

typedef struct {
    ckv3_token_type_t type;
    int index;
    union {
        const char *string;
        double number;
        int boolean;
    } value;
    int string_len;
} ckv3_token_t;



/* ===== CONFIGURATION ===== */

static ckv3_config_t *ckv3_fetch_config(lua_State *l)
{
    ckv3_config_t *cfg;

    cfg = (ckv3_config_t *)lua_touserdata(l, lua_upvalueindex(1));
    if (!cfg)
        luaL_error(l, "BUG: Unable to fetch CKV1 configuration");

    return cfg;
}

#if defined(DISABLE_INVALID_NUMBERS) && !defined(USE_INTERNAL_FPCONV)
void ckv3_verify_invalid_number_setting(lua_State *l, int *setting)
{
    if (*setting == 1) {
        *setting = 0;
        luaL_error(l, "Infinity, NaN, and/or hexadecimal numbers are not supported.");
    }
}
#else
#define ckv3_verify_invalid_number_setting(l, s)    do { } while(0)
#endif

static int ckv3_destroy_config(lua_State *l)
{
    ckv3_config_t *cfg;

    cfg = (ckv3_config_t *)lua_touserdata(l, 1);
    if (cfg)
        strbuf_free(&cfg->encode_buf);
    cfg = NULL;

    return 0;
}

static void ckv3_create_config(lua_State *l)
{
    ckv3_config_t *cfg;
    int i;

    cfg = (ckv3_config_t *)lua_newuserdata(l, sizeof(*cfg));

    /* Create GC method to clean up strbuf */
    lua_newtable(l);
    lua_pushcfunction(l, ckv3_destroy_config);
    lua_setfield(l, -2, "__gc");
    lua_setmetatable(l, -2);

    cfg->encode_sparse_convert = DEFAULT_SPARSE_CONVERT;
    cfg->encode_sparse_ratio = DEFAULT_SPARSE_RATIO;
    cfg->encode_sparse_safe = DEFAULT_SPARSE_SAFE;
    cfg->encode_max_depth = DEFAULT_ENCODE_MAX_DEPTH;
    cfg->decode_max_depth = DEFAULT_DECODE_MAX_DEPTH;
    cfg->encode_invalid_numbers = DEFAULT_ENCODE_INVALID_NUMBERS;
    cfg->decode_invalid_numbers = DEFAULT_DECODE_INVALID_NUMBERS;
    cfg->encode_keep_buffer = DEFAULT_ENCODE_KEEP_BUFFER;
    cfg->encode_number_precision = DEFAULT_ENCODE_NUMBER_PRECISION;

#if DEFAULT_ENCODE_KEEP_BUFFER > 0
    strbuf_init(&cfg->encode_buf, 0);
#endif

    /* Decoding init */

    /* Tag all characters as an error */
    for (i = 0; i < 256; i++)
        cfg->ch2token[i] = T_ERROR;

    /* Set tokens that require no further processing */
    cfg->ch2token['{'] = T_OBJ_BEGIN;
    cfg->ch2token['}'] = T_OBJ_END;
    cfg->ch2token['['] = T_ARR_BEGIN;
    cfg->ch2token[']'] = T_ARR_END;
    cfg->ch2token[','] = T_COMMA;
    cfg->ch2token['='] = T_COLON;
    cfg->ch2token['\0'] = T_END;
    cfg->ch2token[' '] = T_WHITESPACE;
    cfg->ch2token['\t'] = T_WHITESPACE;
    cfg->ch2token['\n'] = T_WHITESPACE;
    cfg->ch2token['\r'] = T_WHITESPACE;

    /* Update characters that require further processing */
    cfg->ch2token['"'] = T_UNKNOWN;     /* string? */
    cfg->ch2token['+'] = T_UNKNOWN;     /* number? */
    cfg->ch2token['-'] = T_UNKNOWN;
    cfg->ch2token['<'] = T_UNKNOWN;
    for (i = 0; i < 10; i++)
        cfg->ch2token['0' + i] = T_UNKNOWN;

    for (i = 0; i < 26; ++i)
    {
        cfg->ch2token['a' + i] = T_UNKNOWN;
        cfg->ch2token['A' + i] = T_UNKNOWN;
    }

    /* Lookup table for parsing escape characters */
    for (i = 0; i < 256; i++)
        cfg->escape2char[i] = 0;          /* String error */
    cfg->escape2char['"'] = '"';
    cfg->escape2char['\\'] = '\\';
    cfg->escape2char['/'] = '/';
    cfg->escape2char['b'] = '\b';
    cfg->escape2char['t'] = '\t';
    cfg->escape2char['n'] = '\n';
    cfg->escape2char['f'] = '\f';
    cfg->escape2char['r'] = '\r';
    cfg->escape2char['u'] = 'u';          /* Unicode parsing required */
}

/* ===== ENCODING ===== */

static void ckv3_encode_exception(lua_State *l, ckv3_config_t *cfg, strbuf_t *ckv3, int lindex,
                                  const char *reason)
{
    if (!cfg->encode_keep_buffer)
        strbuf_free(ckv3);
    luaL_error(l, "Cannot serialise %s: %s",
                  lua_typename(l, lua_type(l, lindex)), reason);
}

/* ckv3_append_string args:
 * - lua_State
 * - KV strbuf
 * - String (Lua stack index)
 *
 * Returns nothing. Doesn't remove string from Lua stack */
static void ckv3_append_string(lua_State *l, strbuf_t *ckv3, int lindex)
{
    const char *escstr;
    const char *str;
    size_t len;
    size_t i;

    str = lua_tolstring(l, lindex, &len);

    /* Worst case is len * 6 (all unicode escapes).
     * This buffer is reused constantly for small strings
     * If there are any excess pages, they won't be hit anyway.
     * This gains ~5% speedup. */
    strbuf_ensure_empty_length(ckv3, len * 6 + 2);
    strbuf_append_char_unsafe(ckv3, '\"');
    
    for (i = 0; i < len; i++) {
        escstr = char2escape[(unsigned char)str[i]];
        if (escstr)
            strbuf_append_string(ckv3, escstr);
        else
            strbuf_append_char_unsafe(ckv3, str[i]);
    }
    strbuf_append_char_unsafe(ckv3, '\"');
}

static void ckv3_check_encode_depth(lua_State *l, ckv3_config_t *cfg,
                                    int current_depth, strbuf_t *ckv3)
{
    /* Ensure there are enough slots free to traverse a table (key,
     * value) and push a string for a potential error message.
     *
     * Unlike "decode", the key and value are still on the stack when
     * lua_checkstack() is called.  Hence an extra slot for luaL_error()
     * below is required just in case the next check to lua_checkstack()
     * fails.
     *
     * While this won't cause a crash due to the EXTRA_STACK reserve
     * slots, it would still be an improper use of the API. */
    if (current_depth <= cfg->encode_max_depth && lua_checkstack(l, 3))
        return;

    if (!cfg->encode_keep_buffer)
        strbuf_free(ckv3);

    luaL_error(l, "Cannot serialise, excessive nesting (%d)",
               current_depth);
}

static void ckv3_append_data(lua_State *l, ckv3_config_t *cfg,
                             int current_depth, strbuf_t *ckv3);

/* ckv3_append_array args:
 * - lua_State
 * - KV strbuf
 * - Size of passwd Lua array (top of stack) */
static void ckv3_append_array(lua_State *l, ckv3_config_t *cfg, int current_depth,
                              strbuf_t *ckv3, int array_length)
{
    strbuf_append_char(ckv3, '[');
    
    for (int i = 1; i <= array_length; i++) {
        strbuf_append_char(ckv3, '\n');
        lua_rawgeti(l, -1, i);
        
        ckv3_append_data(l, cfg, current_depth, ckv3);
        if (i < array_length)
        {
            strbuf_append_char(ckv3, ',');
        }
        lua_pop(l, 1);
    }

    strbuf_append_char(ckv3, '\n');
    for (int j = 1; j < current_depth; j++)
    {
        strbuf_append_char(ckv3, '\t');
    }
    strbuf_append_char(ckv3, ']');
}

static void ckv3_append_object(lua_State *l, ckv3_config_t *cfg,
                               int current_depth, strbuf_t *ckv3)
{
    int w, it_idx = 0;
    /* Object */
    strbuf_append_char(ckv3, '{');
    current_depth++;
    lua_pushnil(l);
    /* table, startkey */
    while (lua_next(l, -2) != 0) {
        strbuf_append_char(ckv3, '\n');
        
        for (w = 2; w < current_depth; w++)
        {
            strbuf_append_char(ckv3, '\t');
        }
        /* table, key, value */

        
        ckv3_append_string(l, ckv3, -2);
        strbuf_append_char(ckv3, ' ');

        const int array_length = lua_rawlen(l, -1);
        ckv3_append_array(l, cfg, current_depth, ckv3, array_length);
        
        lua_pop(l, 1);
    }
    strbuf_append_char(ckv3, '\n');
    for (w = 3; w < current_depth; w++)
    {
        strbuf_append_char(ckv3, '\t');
    }
    strbuf_append_char(ckv3, '}');
}

/* Serialise Lua data into KV string. */
static void ckv3_append_data(lua_State *l, ckv3_config_t *cfg,
                             int current_depth, strbuf_t *ckv3)
{
    switch (lua_type(l, -1)) {
    case LUA_TSTRING:
        ckv3_append_string(l, ckv3, -1);
        break;
    case LUA_TTABLE:
        current_depth++;
        ckv3_check_encode_depth(l, cfg, current_depth, ckv3);

        const int array_length = lua_rawlen(l, -1);
        if (array_length > 0)
        {
            ckv3_append_array(l, cfg, current_depth, ckv3, array_length);
        }
        else
        {
            ckv3_append_object(l, cfg, current_depth, ckv3);
        }
        break;
    default:
        /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
         * and LUA_TLIGHTUSERDATA) cannot be serialised */
        ckv3_encode_exception(l, cfg, ckv3, -1, "type not supported");
        /* never returns */
    }
}

static int ckv3_encode(lua_State *l)
{
    ckv3_config_t *cfg = ckv3_fetch_config(l);
    strbuf_t local_encode_buf;
    strbuf_t *encode_buf;
    char *ckv3;
    int len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    if (!cfg->encode_keep_buffer) {
        /* Use private buffer */
        encode_buf = &local_encode_buf;
        strbuf_init(encode_buf, 0);
    } else {
        /* Reuse existing buffer */
        encode_buf = &cfg->encode_buf;
        strbuf_reset(encode_buf);
    }

    lua_pushnil(l);
    
    int keytype, ln = 0;
    while (lua_next(l, -2) != 0) {
        if (ln == 1)
        {
            strbuf_append_char(encode_buf, '\n');
        }
        else
        {
            ln = 1;
        }

        /* table, key, value */
        keytype = lua_type(l, -2);
        if (keytype == LUA_TSTRING) {
            ckv3_append_string(l, encode_buf, -2);
            strbuf_append_char(encode_buf, ' ');
        } else {
            ckv3_encode_exception(l, cfg, encode_buf, -2,
                                  "table key must be a string");
            /* never returns */
        }

        /* table, key, value */
        ckv3_append_data(l, cfg, 0, encode_buf);
        lua_pop(l, 1);
        /* table, key */
    }

    ckv3 = strbuf_string(encode_buf, &len);
    lua_pushlstring(l, ckv3, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(encode_buf);

    return 1;
}
/* ===== DECODING ===== */

static void ckv3_process_value(lua_State *l, ckv3_parse_t *ckv3,
                               ckv3_token_t *token);

static void ckv3_set_token_error(ckv3_token_t *token, ckv3_parse_t *ckv3,
                                 const char *errtype)
{
    token->type = T_ERROR;
    token->index = ckv3->ptr - ckv3->data;
    token->value.string = errtype;
}

static void ckv3_next_string_token(ckv3_parse_t *ckv3, ckv3_token_t *token)
{
    char *escape2char = ckv3->cfg->escape2char;
    char ch;

    /* Caller must ensure a string is next */
    assert(*ckv3->ptr == '"');

    /* Skip " */
    ckv3->ptr++;

    /* ckv3->tmp is the temporary strbuf used to accumulate the
     * decoded string value.
     * ckv3->tmp is sized to handle KV containing only a string value.
     */
    strbuf_reset(ckv3->tmp);

    while ((ch = *ckv3->ptr) != '"') {
        if (!ch) {
            /* Premature end of the string */
            ckv3_set_token_error(token, ckv3, "unexpected end of string");
            return;
        }

        int hasBackSlash = 0;
        while (ch == '\\')
        {
            ckv3->ptr++;
            ch = *ckv3->ptr;
            hasBackSlash = 1;
        }
        if (hasBackSlash)
        {
            strbuf_append_char_unsafe(ckv3->tmp, '/');
        }
        strbuf_append_char_unsafe(ckv3->tmp, ch);

        ckv3->ptr++;
    }

    ckv3->ptr++;    /* Eat final quote (") */

    strbuf_ensure_null(ckv3->tmp);

    token->type = T_STRING;
    token->value.string = strbuf_string(ckv3->tmp, &token->string_len);
}

char mark_begin[] = {'!', '-', '-'};
char mark_end[] = {'-','-','>'};
/* Fills in the token struct.
 * T_STRING will return a pointer to the ckv3_parse_t temporary string
 * T_ERROR will leave the ckv3->ptr pointer at the error.
 */
static void ckv3_next_token(ckv3_parse_t *ckv3, ckv3_token_t *token)
{
    const ckv3_token_type_t *ch2token = ckv3->cfg->ch2token;
    int ch;

    /* Eat whitespace. */
    while (1) {
        ch = (unsigned char)*(ckv3->ptr);
        token->type = ch2token[ch];
        if (token->type != T_WHITESPACE)
            break;
        ckv3->ptr++;
    }

    if (ch == '<')
    {
        int isMark = 1;
        //skip mark
        for (int i = 0; i < 3; i++)
        {
            ch = (unsigned char)*(ckv3->ptr + i + 1);
            if (ch != mark_begin[i])
            {
                isMark = 0;
                break;
            }
        }

        if (isMark)
        {
            ckv3->ptr += 4;
            
            int offset = 0;
            while (1)
            {
                ch = (unsigned char)*(ckv3->ptr);
                ckv3->ptr++;
                if (ch == mark_end[offset++])
                {
                    if (offset == 3)
                    {
                        break;
                    }
                }
                else
                {
                    offset = 0;
                }
            }

            while (1) {
                ch = (unsigned char)*(ckv3->ptr);
                token->type = ch2token[ch];
                if (token->type != T_WHITESPACE)
                    break;
                ckv3->ptr++;
            }
        }
    }

    /* Store location of new token. Required when throwing errors
     * for unexpected tokens (syntax errors). */
    token->index = ckv3->ptr - ckv3->data;

    /* Don't advance the pointer for an error or the end */
    if (token->type == T_ERROR) {
        ckv3_set_token_error(token, ckv3, "invalid token");
        return;
    }

    if (token->type == T_END) {
        return;
    }
    
    /* Found a known single character token, advance index and return */
    if (token->type != T_UNKNOWN) {
        ckv3->ptr++;
        return;
    }

    /* Process characters which triggered T_UNKNOWN
     *
     * Must use strncmp() to match the front of the KV string.
     * KV identifier must be lowercase.
     * When strict_numbers if disabled, either case is allowed for
     * Infinity/NaN (since we are no longer following the spec..) */
    if (ch == '"') {
        ckv3_next_string_token(ckv3, token);
        return;
    }

    /* Token starts with t/f/n but isn't recognised above. */
    ckv3_set_token_error(token, ckv3, "invalid token");
}

/* This function does not return.
 * DO NOT CALL WITH DYNAMIC MEMORY ALLOCATED.
 * The only supported exception is the temporary parser string
 * ckv3->tmp struct.
 * ckv3 and token should exist on the stack somewhere.
 * luaL_error() will long_jmp and release the stack */
static void ckv3_throw_parse_error(lua_State *l, ckv3_parse_t *ckv3,
                                   const char *exp, ckv3_token_t *token)
{
    const char *found;

    strbuf_free(ckv3->tmp);

    if (token->type == T_ERROR)
        found = token->value.string;
    else
        found = ckv3_token_type_name[token->type];

    /* Note: token->index is 0 based, display starting from 1 */
    luaL_error(l, "Expected %s but found %s at character %d",
               exp, found, token->index + 1);
}

static inline void ckv3_decode_ascend(ckv3_parse_t *ckv3)
{
    ckv3->current_depth--;
}

static void ckv3_decode_descend(lua_State *l, ckv3_parse_t *ckv3, int slots)
{
    ckv3->current_depth++;

    if (ckv3->current_depth <= ckv3->cfg->decode_max_depth &&
        lua_checkstack(l, slots)) {
        return;
    }

    strbuf_free(ckv3->tmp);
    luaL_error(l, "Found too many nested data structures (%d) at character %d",
        ckv3->current_depth, ckv3->ptr - ckv3->data);
}

static void parse_object_internal(lua_State *l, ckv3_token_t *token, ckv3_parse_t *ckv3)
{
    //key
    lua_pushlstring(l, token->value.string, token->string_len);

    ckv3_next_token(ckv3, token);
    if (token->type == T_STRING)
    {
        lua_newtable(l);
        //type
        lua_pushlstring(l, token->value.string, token->string_len);
        // Print_Stack;
        lua_rawseti(l, -2, 1);

        //value
        ckv3_next_token(ckv3, token);
        ckv3_process_value(l, ckv3, token);

        // Print_Stack;
        lua_rawseti(l, -2, 2);
    }
    else if (token->type == T_OBJ_BEGIN)
    {
        ckv3_process_value(l, ckv3, token);
    }
    else if (token->type == T_ARR_BEGIN)
    {
        ckv3_process_value(l, ckv3, token);
    }
    else
    {
        ckv3_throw_parse_error(l, ckv3, "unexpected token", token);
    }
            
    /* Set key = value */
    lua_rawset(l, -3);
}

static void ckv3_parse_object_context(lua_State *l, ckv3_parse_t *ckv3)
{
    ckv3_token_t token;

    /* 3 slots required:
     * .., table, key, value */
    ckv3_decode_descend(l, ckv3, 3);

    lua_newtable(l);

    ckv3_next_token(ckv3, &token);
    
    /* Handle empty objects */
    if (token.type == T_OBJ_END) {
        ckv3_decode_ascend(ckv3);
        return;
    }
    
    while (1) {
        if (token.type != T_STRING)
            ckv3_throw_parse_error(l, ckv3, "object key string", &token);

        parse_object_internal(l, &token, ckv3);
        
        //key or T_OBJ_END?
        ckv3_next_token(ckv3, &token);

        if (token.type == T_OBJ_END) {
            ckv3_decode_ascend(ckv3);
            return;
        }
    }
}

/* Handle the array context */
static void ckv3_parse_array_context(lua_State *l, ckv3_parse_t *ckv3)
{
    ckv3_token_t token;
    int i;

    /* 2 slots required:
     * .., table, value */
    const int slot = 2;
    ckv3_decode_descend(l, ckv3, slot);

    lua_newtable(l);
    ckv3_next_token(ckv3, &token);

    /* Handle empty arrays */
    if (token.type == T_ARR_END) {
        ckv3_decode_ascend(ckv3);
        return;
    }

    for (i = 1; ; i++)
    {
        //push string
        ckv3_process_value(l, ckv3, &token);
        
        // maybe pure array, maybe element_array
        ckv3_next_token(ckv3, &token, 0);
        if (token.type == T_COMMA)
        {
            //是逗号 进下一个循环
            lua_rawseti(l, -2, i);
            ckv3_next_token(ckv3, &token, 0);
            //兼容一下末尾是,]的情况
            if (token.type == T_ARR_END)
            {
                ckv3_decode_ascend(ckv3);
                return;
            }
        }
        else if (token.type == T_ARR_END)
        {
            //数组的末尾
            lua_rawseti(l, -2, i);
            ckv3_decode_ascend(ckv3);
            return;
        }
        else
        {
            //不是逗号，那么是数据类型或者直接跟容器
            size_t len;
            const char * keyName = lua_tolstring(l, -1, &len);
            lua_pop(l, 1);

            lua_newtable(l);
            lua_pushlstring(l, keyName, len);

            //array[1] = type
            lua_rawseti(l, -2, 1);

            //array[2] = obj
            ckv3_process_value(l, ckv3, &token);
            lua_rawseti(l, -2, 2);
            
            lua_rawseti(l, -2, i);
            ckv3_next_token(ckv3, &token, 0);
            if (token.type == T_COMMA)
            {
                //是逗号，直接进入下一个循环
                ckv3_next_token(ckv3, &token, 0);
                //兼容一下末尾是,]的情况
                if (token.type == T_ARR_END)
                {
                    ckv3_decode_ascend(ckv3);
                    return;
                }
            }
            else if (token.type == T_ARR_END)
            {
                ckv3_decode_ascend(ckv3);
                return;
            }
        }
    }
}

/* Handle the "value" context */
static void ckv3_process_value(lua_State *l, ckv3_parse_t *ckv3,
                               ckv3_token_t *token)
{
    switch (token->type) {
    case T_STRING:
        lua_pushlstring(l, token->value.string, token->string_len);
        break;;
    case T_OBJ_BEGIN:
        ckv3_parse_object_context(l, ckv3);
        break;;
    case T_ARR_BEGIN:
        ckv3_parse_array_context(l, ckv3);
        break;;
    default:
        ckv3_throw_parse_error(l, ckv3, "value", token);
    }
}

static int ckv3_decode(lua_State *l)
{
    ckv3_parse_t ckv3;
    ckv3_token_t token;
    size_t ckv3_len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    ckv3.cfg = ckv3_fetch_config(l);
    ckv3.data = luaL_checklstring(l, 1, &ckv3_len);
    lua_pop(l, 1);
    ckv3.current_depth = 0;
    ckv3.ptr = ckv3.data;

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CKV1 can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (ckv3_len >= 2 && (!ckv3.data[0] || !ckv3.data[1]))
        luaL_error(l, "KV parser does not support UTF-16 or UTF-32");

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire ckv3 string */
    ckv3.tmp = strbuf_new(ckv3_len);

    lua_newtable(l);
    
    ckv3_next_token(&ckv3, &token, 1);
    if (token.type == T_STRING)
    {
        while (1) {
            parse_object_internal(l, &token, &ckv3);
            
            ckv3_next_token(&ckv3, &token, 1);

            if (token.type == T_END) {
                break;
            }
        }
    }
    else
    {
        ckv3_throw_parse_error(l, &ckv3, "Must begin with string", &token);
    }
    
    strbuf_free(ckv3.tmp);

    return 1;
}


/* Return ckv3 module table */
static int lua_ckv3_new(lua_State *l)
{
    luaL_Reg reg[] = {
        { "encode", ckv3_encode },
        { "decode", ckv3_decode },
        { NULL, NULL }
    };

    /* Initialise number conversions */
    fpconv_init();

    /* ckv3 module table */
    lua_newtable(l);

    /* Register functions with config data as upvalue */
    ckv3_create_config(l);
    luaL_setfuncs(l, reg, 1);

    return 1;
}

#if __DEBUG_KV__
LUAMOD_API int luaopen_ckv3(lua_State *l)
#else
LUALIB_API int luaopen_ckv3(lua_State *l)
#endif
{
    lua_ckv3_new(l);
    return 1;
}