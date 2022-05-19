#include "include/common.h"

#ifndef CKV1_MODNAME
#define CKV1_MODNAME   "ckv1"
#endif

#ifndef CKV1_VERSION
#define CKV1_VERSION   "1.0.0"
#endif

typedef enum
{
    LoadType_Map,
    LoadType_Array
}LoadType;

static LoadType loadType;

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
} ckv1_token_type_t;

static const char *ckv1_token_type_name[] = {
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

typedef struct {
    ckv1_token_type_t ch2token[256];
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
} ckv1_config_t;

typedef struct {
    const char *data;
    const char *ptr;
    strbuf_t *tmp;    /* Temporary storage for strings */
    ckv1_config_t *cfg;
    int current_depth;
} ckv1_parse_t;

typedef struct {
    ckv1_token_type_t type;
    int index;
    union {
        const char *string;
        double number;
        int boolean;
    } value;
    int string_len;
} ckv1_token_t;



/* ===== CONFIGURATION ===== */

static ckv1_config_t *ckv1_fetch_config(lua_State *l)
{
    ckv1_config_t *cfg;

    cfg = (ckv1_config_t *)lua_touserdata(l, lua_upvalueindex(1));
    if (!cfg)
        luaL_error(l, "BUG: Unable to fetch CKV1 configuration");

    return cfg;
}

/* Ensure the correct number of arguments have been provided.
 * Pad with nil to allow other functions to simply check arg[i]
 * to find whether an argument was provided */
static ckv1_config_t *ckv1_arg_init(lua_State *l, int args)
{
    luaL_argcheck(l, lua_gettop(l) <= args, args + 1,
                  "found too many arguments");

    while (lua_gettop(l) < args)
        lua_pushnil(l);

    return ckv1_fetch_config(l);
}

/* Process integer options for configuration functions */
static int ckv1_integer_option(lua_State *l, int optindex, int *setting,
                               int min, int max)
{
    char errmsg[64];
    int value;

    if (!lua_isnil(l, optindex)) {
        value = luaL_checkinteger(l, optindex);
        snprintf(errmsg, sizeof(errmsg), "expected integer between %d and %d", min, max);
        luaL_argcheck(l, min <= value && value <= max, 1, errmsg);
        *setting = value;
    }

    lua_pushinteger(l, *setting);

    return 1;
}

/* Process enumerated arguments for a configuration function */
static int ckv1_enum_option(lua_State *l, int optindex, int *setting,
                            const char **options, int bool_true)
{
    static const char *bool_options[] = { "off", "on", NULL };

    if (!options) {
        options = bool_options;
        bool_true = 1;
    }

    if (!lua_isnil(l, optindex)) {
        if (bool_true && lua_isboolean(l, optindex))
            *setting = lua_toboolean(l, optindex) * bool_true;
        else
            *setting = luaL_checkoption(l, optindex, NULL, options);
    }

    if (bool_true && (*setting == 0 || *setting == bool_true))
        lua_pushboolean(l, *setting);
    else
        lua_pushstring(l, options[*setting]);

    return 1;
}

#if defined(DISABLE_INVALID_NUMBERS) && !defined(USE_INTERNAL_FPCONV)
void ckv1_verify_invalid_number_setting(lua_State *l, int *setting)
{
    if (*setting == 1) {
        *setting = 0;
        luaL_error(l, "Infinity, NaN, and/or hexadecimal numbers are not supported.");
    }
}
#else
#define ckv1_verify_invalid_number_setting(l, s)    do { } while(0)
#endif

static int ckv1_destroy_config(lua_State *l)
{
    ckv1_config_t *cfg;

    cfg = (ckv1_config_t *)lua_touserdata(l, 1);
    if (cfg)
        strbuf_free(&cfg->encode_buf);
    cfg = NULL;

    return 0;
}

static void ckv1_create_config(lua_State *l)
{
    ckv1_config_t *cfg;
    int i;

    cfg = (ckv1_config_t *)lua_newuserdata(l, sizeof(*cfg));

    /* Create GC method to clean up strbuf */
    lua_newtable(l);
    lua_pushcfunction(l, ckv1_destroy_config);
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

static void ckv1_encode_exception(lua_State *l, ckv1_config_t *cfg, strbuf_t *ckv1, int lindex,
                                  const char *reason)
{
    if (!cfg->encode_keep_buffer)
        strbuf_free(ckv1);
    luaL_error(l, "Cannot serialise %s: %s",
                  lua_typename(l, lua_type(l, lindex)), reason);
}

/* ckv1_append_string args:
 * - lua_State
 * - KV strbuf
 * - String (Lua stack index)
 *
 * Returns nothing. Doesn't remove string from Lua stack */
static void ckv1_append_string(lua_State *l, strbuf_t *ckv1, int lindex, int needQuote)
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
    strbuf_ensure_empty_length(ckv1, len * 6 + 2);

    if (needQuote == 1)
        strbuf_append_char_unsafe(ckv1, '\"');
    
    for (i = 0; i < len; i++) {
        escstr = char2escape[(unsigned char)str[i]];
        if (escstr)
            strbuf_append_string(ckv1, escstr);
        else
            strbuf_append_char_unsafe(ckv1, str[i]);
    }
    if (needQuote == 1)
        strbuf_append_char_unsafe(ckv1, '\"');
}

static void ckv1_check_encode_depth(lua_State *l, ckv1_config_t *cfg,
                                    int current_depth, strbuf_t *ckv1)
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
        strbuf_free(ckv1);

    luaL_error(l, "Cannot serialise, excessive nesting (%d)",
               current_depth);
}

static void ckv1_append_data(lua_State *l, ckv1_config_t *cfg,
                             int current_depth, strbuf_t *ckv1, int quot);

/* ckv1_append_array args:
 * - lua_State
 * - KV strbuf
 * - Size of passwd Lua array (top of stack) */
static void ckv1_append_array(lua_State *l, ckv1_config_t *cfg, int current_depth,
                              strbuf_t *ckv1, int array_length)
{
    strbuf_append_char(ckv1, '[');
    
    for (int i = 1; i <= array_length; i++) {
        strbuf_append_char(ckv1, '\n');
        lua_rawgeti(l, -1, i);
        
        for (int j = 0; j < current_depth; j++)
        {
            strbuf_append_char(ckv1, '\t');
        }
        
        ckv1_append_data(l, cfg, current_depth, ckv1, 1);
        strbuf_append_char(ckv1, ',');
        lua_pop(l, 1);
    }

    strbuf_append_char(ckv1, '\n');
    for (int j = 1; j < current_depth; j++)
    {
        strbuf_append_char(ckv1, '\t');
    }
    strbuf_append_char(ckv1, ']');
}

static void ckv1_append_object_array(lua_State *l, ckv1_config_t *cfg, int current_depth,
                              strbuf_t *ckv1, int array_length)
{
    strbuf_append_char(ckv1, '\n');
    int w;
    for (w = 1; w < current_depth; w++)
    {
        strbuf_append_char(ckv1, '\t');
    }

    lua_rawgeti(l, -1, 1);
    size_t len;
    const char *str = lua_tolstring(l, -1, &len);
    lua_pop(l, 1);
    int isArray = 0;
    if (strcmp(str, "__IsArray__") != 0) 
    {
        strbuf_append_char(ckv1, '{');
        strbuf_append_char(ckv1, '\n');
        for (int i = 1; i <= array_length; i+=2) {

            for (w = 0; w < current_depth; w++)
            {
                strbuf_append_char(ckv1, '\t');
            }

            lua_rawgeti(l, -1, i);
            ckv1_append_data(l, cfg, current_depth, ckv1, 0);
            strbuf_append_char(ckv1, '=');
            lua_pop(l, 1);
        

            lua_rawgeti(l, -1, i + 1);
            ckv1_append_data(l, cfg, current_depth, ckv1, 1);
            lua_pop(l, 1);
            
            strbuf_append_char(ckv1, '\n');
        }
    }
    else
    {
        isArray = 1;
        strbuf_append_char(ckv1, '[');
        strbuf_append_char(ckv1, '\n');
        for (int i = 2; i <= array_length; i++) {

            for (w = 0; w < current_depth; w++)
            {
                strbuf_append_char(ckv1, '\t');
            }
            strbuf_append_char(ckv1, '\"');
            
            lua_rawgeti(l, -1, i);
            ckv1_append_data(l, cfg, current_depth, ckv1, 0);
            lua_pop(l, 1);
            strbuf_append_char(ckv1, '\"');
            
            strbuf_append_char(ckv1, ',');
            strbuf_append_char(ckv1, '\n');
        }
    }
    
    for (w = 1; w < current_depth; w++)
    {
        strbuf_append_char(ckv1, '\t');
    }

    if (!isArray)
    {
        strbuf_append_char(ckv1, '}');
    }
    else
    {
        strbuf_append_char(ckv1, ']');
    }
}

static void ckv1_append_number(lua_State *l, ckv1_config_t *cfg,
                               strbuf_t *ckv1, int lindex)
{
    double num = lua_tonumber(l, lindex);
    int len;

    if (cfg->encode_invalid_numbers == 0) {
        /* Prevent encoding invalid numbers */
        if (isinf(num) || isnan(num))
            ckv1_encode_exception(l, cfg, ckv1, lindex,
                                  "must not be NaN or Infinity");
    } else if (cfg->encode_invalid_numbers == 1) {
        /* Encode NaN/Infinity separately to ensure Javascript compatible
         * values are used. */
        if (isnan(num)) {
            strbuf_append_mem(ckv1, "NaN", 3);
            return;
        }
        if (isinf(num)) {
            if (num < 0)
                strbuf_append_mem(ckv1, "-Infinity", 9);
            else
                strbuf_append_mem(ckv1, "Infinity", 8);
            return;
        }
    } else {
        /* Encode invalid numbers as "null" */
        if (isinf(num) || isnan(num)) {
            strbuf_append_mem(ckv1, "null", 4);
            return;
        }
    }

    strbuf_ensure_empty_length(ckv1, FPCONV_G_FMT_BUFSIZE);
    len = fpconv_g_fmt(strbuf_empty_ptr(ckv1), num, cfg->encode_number_precision);
    strbuf_extend_length(ckv1, len);
}

static void ckv1_append_object(lua_State *l, ckv1_config_t *cfg,
                               int current_depth, strbuf_t *ckv1)
{
    int keytype, w = 0;

    /* Object */
    strbuf_append_char(ckv1, '{');
    current_depth++;
    lua_pushnil(l);
    /* table, startkey */
    while (lua_next(l, -2) != 0) {
        strbuf_append_char(ckv1, '\n');
        
        for (w = 2; w < current_depth; w++)
        {
            strbuf_append_char(ckv1, '\t');
        }
        /* table, key, value */
        keytype = lua_type(l, -2);
        if (keytype == LUA_TNUMBER) {
            ckv1_append_number(l, cfg, ckv1, -2);
            strbuf_append_char(ckv1, '=');
        } else if (keytype == LUA_TSTRING) {
            ckv1_append_string(l, ckv1, -2, 0);
            strbuf_append_char(ckv1, '=');
        } else {
            ckv1_encode_exception(l, cfg, ckv1, -2,
                                  "table key must be a number or string");
            /* never returns */
        }

        /* table, key, value */
        ckv1_append_data(l, cfg, current_depth, ckv1, 1);
        lua_pop(l, 1);
        /* table, key */
    }
    strbuf_append_char(ckv1, '\n');
    for (w = 3; w < current_depth; w++)
    {
        strbuf_append_char(ckv1, '\t');
    }
    strbuf_append_char(ckv1, '}');
}

/* Serialise Lua data into KV string. */
static void ckv1_append_data(lua_State *l, ckv1_config_t *cfg,
                             int current_depth, strbuf_t *ckv1, int needQuot)
{
    switch (lua_type(l, -1)) {
    case LUA_TSTRING:
        ckv1_append_string(l, ckv1, -1, needQuot);
        break;
    case LUA_TNUMBER:
        ckv1_append_number(l, cfg, ckv1, -1);
        break;
    case LUA_TBOOLEAN:
        if (lua_toboolean(l, -1))
            strbuf_append_mem(ckv1, "true", 4);
        else
            strbuf_append_mem(ckv1, "false", 5);
        break;
    case LUA_TTABLE:
        current_depth++;
        ckv1_check_encode_depth(l, cfg, current_depth, ckv1);

        const int array_length = lua_rawlen(l, -1);
        if (loadType == LoadType_Array)
        {
            ckv1_append_object_array(l, cfg, current_depth, ckv1, array_length);
        }
        else
        {
            if (array_length > 0)
            {
                ckv1_append_array(l, cfg, current_depth, ckv1, array_length);
            }
            else
            {
                ckv1_append_object(l, cfg, current_depth, ckv1);
            }
        }
        break;
    case LUA_TNIL:
        strbuf_append_mem(ckv1, "null", 4);
        break;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(l, -1) == NULL) {
            strbuf_append_mem(ckv1, "null", 4);
            break;
        }
    default:
        /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
         * and LUA_TLIGHTUSERDATA) cannot be serialised */
        ckv1_encode_exception(l, cfg, ckv1, -1, "type not supported");
        /* never returns */
    }
}

static int ckv1_encode(lua_State *l)
{
    loadType = LoadType_Map;
    ckv1_config_t *cfg = ckv1_fetch_config(l);
    strbuf_t local_encode_buf;
    strbuf_t *encode_buf;
    char *ckv1;
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
        if (keytype == LUA_TNUMBER) {
            ckv1_append_number(l, cfg, encode_buf, -2);
            strbuf_append_char(encode_buf, '=');
        } else if (keytype == LUA_TSTRING) {
            ckv1_append_string(l, encode_buf, -2, 0);
            strbuf_append_char(encode_buf, '=');
        } else {
            ckv1_encode_exception(l, cfg, encode_buf, -2,
                                  "table key must be a number or string");
            /* never returns */
        }

        /* table, key, value */
        ckv1_append_data(l, cfg, 0, encode_buf, 1);
        lua_pop(l, 1);
        /* table, key */
    }

    ckv1 = strbuf_string(encode_buf, &len);
    lua_pushlstring(l, ckv1, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(encode_buf);

    return 1;
}

static int ckv1_encode_array(lua_State *l)
{
    loadType = LoadType_Array;
    ckv1_config_t *cfg = ckv1_fetch_config(l);
    strbuf_t local_encode_buf;
    strbuf_t *encode_buf;
    char *ckv1;
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
    
    int MAX = lua_rawlen(l, -2); // lua_rawlen always returns value >=
    for (int n = 1; n <= MAX;)
    {
        if (n > 1)
        {
            strbuf_append_char_unsafe(encode_buf, '\n');
        }
        lua_rawgeti(l, -2, n++); // [table, element]
        ckv1_append_data(l, cfg, 0, encode_buf, 1);
        if (lua_type(l, -1) != LUA_TTABLE)
        {
            lua_pop(l, 1); // [table]

            // strbuf_append_char_unsafe(encode_buf, '=');

            lua_rawgeti(l, -2, n++); // [table, element]
            ckv1_append_data(l, cfg, 0, encode_buf, 1);
        }
        lua_pop(l, 1); // [table]
    }

    ckv1 = strbuf_string(encode_buf, &len);
    lua_pushlstring(l, ckv1, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(encode_buf);

    return 1;
}

/* ===== DECODING ===== */

static void ckv1_process_value(lua_State *l, ckv1_parse_t *ckv1,
                               ckv1_token_t *token);

static int hexdigit2int(char hex)
{
    if ('0' <= hex  && hex <= '9')
        return hex - '0';

    /* Force lowercase */
    hex |= 0x20;
    if ('a' <= hex && hex <= 'f')
        return 10 + hex - 'a';

    return -1;
}

static int decode_hex4(const char *hex)
{
    int digit[4];
    int i;

    /* Convert ASCII hex digit to numeric digit
     * Note: this returns an error for invalid hex digits, including
     *       NULL */
    for (i = 0; i < 4; i++) {
        digit[i] = hexdigit2int(hex[i]);
        if (digit[i] < 0) {
            return -1;
        }
    }

    return (digit[0] << 12) +
           (digit[1] << 8) +
           (digit[2] << 4) +
            digit[3];
}

/* Converts a Unicode codepoint to UTF-8.
 * Returns UTF-8 string length, and up to 4 bytes in *utf8 */
static int codepoint_to_utf8(char *utf8, int codepoint)
{
    /* 0xxxxxxx */
    if (codepoint <= 0x7F) {
        utf8[0] = codepoint;
        return 1;
    }

    /* 110xxxxx 10xxxxxx */
    if (codepoint <= 0x7FF) {
        utf8[0] = (codepoint >> 6) | 0xC0;
        utf8[1] = (codepoint & 0x3F) | 0x80;
        return 2;
    }

    /* 1110xxxx 10xxxxxx 10xxxxxx */
    if (codepoint <= 0xFFFF) {
        utf8[0] = (codepoint >> 12) | 0xE0;
        utf8[1] = ((codepoint >> 6) & 0x3F) | 0x80;
        utf8[2] = (codepoint & 0x3F) | 0x80;
        return 3;
    }

    /* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint <= 0x1FFFFF) {
        utf8[0] = (codepoint >> 18) | 0xF0;
        utf8[1] = ((codepoint >> 12) & 0x3F) | 0x80;
        utf8[2] = ((codepoint >> 6) & 0x3F) | 0x80;
        utf8[3] = (codepoint & 0x3F) | 0x80;
        return 4;
    }

    return 0;
}


/* Called when index pointing to beginning of UTF-16 code escape: \uXXXX
 * \u is guaranteed to exist, but the remaining hex characters may be
 * missing.
 * Translate to UTF-8 and append to temporary token string.
 * Must advance index to the next character to be processed.
 * Returns: 0   success
 *          -1  error
 */
static int ckv1_append_unicode_escape(ckv1_parse_t *ckv1)
{
    char utf8[4];       /* Surrogate pairs require 4 UTF-8 bytes */
    int codepoint;
    int surrogate_low;
    int len;
    int escape_len = 6;

    /* Fetch UTF-16 code unit */
    codepoint = decode_hex4(ckv1->ptr + 2);
    if (codepoint < 0)
        return -1;

    /* UTF-16 surrogate pairs take the following 2 byte form:
     *      11011 x yyyyyyyyyy
     * When x = 0: y is the high 10 bits of the codepoint
     *      x = 1: y is the low 10 bits of the codepoint
     *
     * Check for a surrogate pair (high or low) */
    if ((codepoint & 0xF800) == 0xD800) {
        /* Error if the 1st surrogate is not high */
        if (codepoint & 0x400)
            return -1;

        /* Ensure the next code is a unicode escape */
        if (*(ckv1->ptr + escape_len) != '\\' ||
            *(ckv1->ptr + escape_len + 1) != 'u') {
            return -1;
        }

        /* Fetch the next codepoint */
        surrogate_low = decode_hex4(ckv1->ptr + 2 + escape_len);
        if (surrogate_low < 0)
            return -1;

        /* Error if the 2nd code is not a low surrogate */
        if ((surrogate_low & 0xFC00) != 0xDC00)
            return -1;

        /* Calculate Unicode codepoint */
        codepoint = (codepoint & 0x3FF) << 10;
        surrogate_low &= 0x3FF;
        codepoint = (codepoint | surrogate_low) + 0x10000;
        escape_len = 12;
    }

    /* Convert codepoint to UTF-8 */
    len = codepoint_to_utf8(utf8, codepoint);
    if (!len)
        return -1;

    /* Append bytes and advance parse index */
    strbuf_append_mem_unsafe(ckv1->tmp, utf8, len);
    ckv1->ptr += escape_len;

    return 0;
}

static void ckv1_set_token_error(ckv1_token_t *token, ckv1_parse_t *ckv1,
                                 const char *errtype)
{
    token->type = T_ERROR;
    token->index = ckv1->ptr - ckv1->data;
    token->value.string = errtype;
}

static void ckv1_next_string_token(ckv1_parse_t *ckv1, ckv1_token_t *token)
{
    char *escape2char = ckv1->cfg->escape2char;
    char ch;

    /* Caller must ensure a string is next */
    assert(*ckv1->ptr == '"');

    /* Skip " */
    ckv1->ptr++;

    /* ckv1->tmp is the temporary strbuf used to accumulate the
     * decoded string value.
     * ckv1->tmp is sized to handle KV containing only a string value.
     */
    strbuf_reset(ckv1->tmp);

    while ((ch = *ckv1->ptr) != '"') {
        if (!ch) {
            /* Premature end of the string */
            ckv1_set_token_error(token, ckv1, "unexpected end of string");
            return;
        }

        int hasBackSlash = 0;
        while (ch == '\\')
        {
            ckv1->ptr++;
            ch = *ckv1->ptr;
            hasBackSlash = 1;
        }
        if (hasBackSlash)
        {
            strbuf_append_char_unsafe(ckv1->tmp, '/');
        }
        strbuf_append_char_unsafe(ckv1->tmp, ch);

        ckv1->ptr++;
    }

    ckv1->ptr++;    /* Eat final quote (") */

    strbuf_ensure_null(ckv1->tmp);

    token->type = T_STRING;
    token->value.string = strbuf_string(ckv1->tmp, &token->string_len);
}

static void ckv1_next_string_token_noquote(ckv1_parse_t *ckv1, ckv1_token_t *token)
{
    /* ckv1->tmp is the temporary strbuf used to accumulate the
     * decoded string value.
     * ckv1->tmp is sized to handle KV containing only a string value.
     */
    strbuf_reset(ckv1->tmp);
    char ch = *ckv1->ptr;
    while (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '=') {
        if (!ch) {
            /* Premature end of the string */
            ckv1_set_token_error(token, ckv1, "unexpected end of string");
            return;
        }

        int hasBackSlash = 0;
        while (ch == '\\')
        {
            ckv1->ptr++;
            ch = *ckv1->ptr;
            hasBackSlash = 1;
        }
        if (hasBackSlash)
        {
            strbuf_append_char_unsafe(ckv1->tmp, '/');
        }

        strbuf_append_char_unsafe(ckv1->tmp, ch);

        ckv1->ptr++;
        ch = *(ckv1->ptr);
    }

    strbuf_ensure_null(ckv1->tmp);

    token->type = T_STRING;
    token->value.string = strbuf_string(ckv1->tmp, &token->string_len);
}

/* KV numbers should take the following form:
 *      -?(0|[1-9]|[1-9][0-9]+)(.[0-9]+)?([eE][-+]?[0-9]+)?
 *
 * ckv1_next_number_token() uses strtod() which allows other forms:
 * - numbers starting with '+'
 * - NaN, -NaN, infinity, -infinity
 * - hexadecimal numbers
 * - numbers with leading zeros
 *
 * ckv1_is_invalid_number() detects "numbers" which may pass strtod()'s
 * error checking, but should not be allowed with strict KV.
 *
 * ckv1_is_invalid_number() may pass numbers which cause strtod()
 * to generate an error.
 */
static int ckv1_is_invalid_number(ckv1_parse_t *ckv1)
{
    const char *p = ckv1->ptr;

    /* Reject numbers starting with + */
    if (*p == '+')
        return 1;

    /* Skip minus sign if it exists */
    if (*p == '-')
        p++;

    /* Reject numbers starting with 0x, or leading zeros */
    if (*p == '0') {
        int ch2 = *(p + 1);

        if ((ch2 | 0x20) == 'x' ||          /* Hex */
            ('0' <= ch2 && ch2 <= '9'))     /* Leading zero */
            return 1;

        return 0;
    } else if (*p <= '9') {
        return 0;                           /* Ordinary number */
    }

    /* Reject inf/nan */
    if (!strncasecmp(p, "inf", 3))
        return 1;
    if (!strncasecmp(p, "nan", 3))
        return 1;

    /* Pass all other numbers which may still be invalid, but
     * strtod() will catch them. */
    return 0;
}

static void ckv1_next_number_token(ckv1_parse_t *ckv1, ckv1_token_t *token)
{
    char *endptr;

    token->type = T_NUMBER;
    token->value.number = fpconv_strtod(ckv1->ptr, &endptr);
    if (ckv1->ptr == endptr)
        ckv1_set_token_error(token, ckv1, "invalid number");
    else
        ckv1->ptr = endptr;     /* Skip the processed number */
}

/* Fills in the token struct.
 * T_STRING will return a pointer to the ckv1_parse_t temporary string
 * T_ERROR will leave the ckv1->ptr pointer at the error.
 */
static void ckv1_next_token(ckv1_parse_t *ckv1, ckv1_token_t *token, int isKey)
{
    const ckv1_token_type_t *ch2token = ckv1->cfg->ch2token;
    int ch;

    /* Eat whitespace. */
    while (1) {
        ch = (unsigned char)*(ckv1->ptr);
        token->type = ch2token[ch];
        if (token->type != T_WHITESPACE)
            break;
        ckv1->ptr++;
    }

    if (ch == '<')
    {
        char skips[] = {'!', '-', '-'};
        int isMark = 1;
        //skip mark
        for (int i = 0; i < 3; i++)
        {
            ch = (unsigned char)*(ckv1->ptr + i + 1);
            if (ch != skips[i])
            {
                isMark = 0;
                break;
            }
        }

        if (isMark)
        {
            ckv1->ptr += 4;
            char skips2[] = {'-','-','>'};
            int offset = 0;
            while (1)
            {
                ch = (unsigned char)*(ckv1->ptr);
                ckv1->ptr++;
                if (ch == skips2[offset++])
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
                ch = (unsigned char)*(ckv1->ptr);
                token->type = ch2token[ch];
                if (token->type != T_WHITESPACE)
                    break;
                ckv1->ptr++;
            }
        }
    }

    /* Store location of new token. Required when throwing errors
     * for unexpected tokens (syntax errors). */
    token->index = ckv1->ptr - ckv1->data;

    /* Don't advance the pointer for an error or the end */
    if (token->type == T_ERROR) {
        ckv1_set_token_error(token, ckv1, "invalid token");
        return;
    }

    if (token->type == T_END) {
        return;
    }

    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
    {
        ckv1_next_string_token_noquote(ckv1, token);
        return;
    }

    if ((ch >= '0' && ch <= '9') || ch == '-')
    {
        if (isKey == 1)
        {
            ckv1_next_string_token_noquote(ckv1, token);
            return;
        }
    }
    
    /* Found a known single character token, advance index and return */
    if (token->type != T_UNKNOWN) {
        ckv1->ptr++;
        return;
    }

    /* Process characters which triggered T_UNKNOWN
     *
     * Must use strncmp() to match the front of the KV string.
     * KV identifier must be lowercase.
     * When strict_numbers if disabled, either case is allowed for
     * Infinity/NaN (since we are no longer following the spec..) */
    if (ch == '"') {
        ckv1_next_string_token(ckv1, token);
        return;
    } else if (ch == '-' || ('0' <= ch && ch <= '9')) {
        if (!ckv1->cfg->decode_invalid_numbers && ckv1_is_invalid_number(ckv1)) {
            ckv1_set_token_error(token, ckv1, "invalid number");
            return;
        }
        ckv1_next_number_token(ckv1, token);
        return;
    } else if (ckv1->cfg->decode_invalid_numbers &&
               ckv1_is_invalid_number(ckv1)) {
        /* When decode_invalid_numbers is enabled, only attempt to process
         * numbers we know are invalid KV (Inf, NaN, hex)
         * This is required to generate an appropriate token error,
         * otherwise all bad tokens will register as "invalid number"
         */
        ckv1_next_number_token(ckv1, token);
        return;
    }

    /* Token starts with t/f/n but isn't recognised above. */
    ckv1_set_token_error(token, ckv1, "invalid token");
}

/* This function does not return.
 * DO NOT CALL WITH DYNAMIC MEMORY ALLOCATED.
 * The only supported exception is the temporary parser string
 * ckv1->tmp struct.
 * ckv1 and token should exist on the stack somewhere.
 * luaL_error() will long_jmp and release the stack */
static void ckv1_throw_parse_error(lua_State *l, ckv1_parse_t *ckv1,
                                   const char *exp, ckv1_token_t *token)
{
    const char *found;

    strbuf_free(ckv1->tmp);

    if (token->type == T_ERROR)
        found = token->value.string;
    else
        found = ckv1_token_type_name[token->type];

    /* Note: token->index is 0 based, display starting from 1 */
    luaL_error(l, "Expected %s but found %s at character %d",
               exp, found, token->index + 1);
}

static inline void ckv1_decode_ascend(ckv1_parse_t *ckv1)
{
    ckv1->current_depth--;
}

static void ckv1_decode_descend(lua_State *l, ckv1_parse_t *ckv1, int slots)
{
    ckv1->current_depth++;

    if (ckv1->current_depth <= ckv1->cfg->decode_max_depth &&
        lua_checkstack(l, slots)) {
        return;
    }

    strbuf_free(ckv1->tmp);
    luaL_error(l, "Found too many nested data structures (%d) at character %d",
        ckv1->current_depth, ckv1->ptr - ckv1->data);
}

static void ckv1_parse_object_context(lua_State *l, ckv1_parse_t *ckv1)
{
    ckv1_token_t token;

    /* 3 slots required:
     * .., table, key, value */
    ckv1_decode_descend(l, ckv1, 3);

    lua_newtable(l);

    ckv1_next_token(ckv1, &token, 1);

    int HasNestKV3 = 0;
    /* Handle empty objects */
    if (token.type == T_OBJ_END) {
        ckv1_decode_ascend(ckv1);
        return;
    }

    if (token.type == T_OBJ_BEGIN)
    {
        //可能是kv1里面嵌套了kv3 傻逼v蛇
        ckv1_next_token(ckv1, &token, 1);

        ckv1_next_token(ckv1, &token, 1);
        HasNestKV3 = 1;
    }

    while (1) {
        if (token.type != T_STRING)
            ckv1_throw_parse_error(l, ckv1, "object key string", &token);

        /* Push key */
        lua_pushlstring(l, token.value.string, token.string_len);

        ckv1_next_token(ckv1, &token, 0);
        if (token.type != T_COLON)
        {
            ckv1_process_value(l, ckv1, &token);
        }
        else
        {
            /* Fetch value */
            ckv1_next_token(ckv1, &token, 0);
            ckv1_process_value(l, ckv1, &token);
        }
        
        /* Set key = value */
        lua_rawset(l, -3);


        //key or T_OBJ_END?
        ckv1_next_token(ckv1, &token, 1);

        if (token.type == T_OBJ_END) {
            // compatible
            if (HasNestKV3 == 1)
            {
                ckv1_next_token(ckv1, &token, 1);
            }
            ckv1_decode_ascend(ckv1);
            return;
        }
    }
}
const char* ArrayFlag = "__IsArray__";
const int ArrayFlagLen = 11;
/* Handle the array context */
static void ckv1_parse_array_context(lua_State *l, ckv1_parse_t *ckv1, int isObject)
{
    ckv1_token_t token;
    int i;

    /* 2 slots required:
     * .., table, value */
    const int slot = (isObject == 1 ? 3 : 2);
    ckv1_decode_descend(l, ckv1, slot);

    lua_newtable(l);

    ckv1_next_token(ckv1, &token, isObject);

    /* Handle empty arrays */
    if (token.type == T_ARR_END || (isObject == 1 && token.type == T_OBJ_END)) {
        ckv1_decode_ascend(ckv1);
        return;
    }

    int HasNestKV3 = 0;
    if (token.type == T_OBJ_BEGIN)
    {
        //可能是kv1里面嵌套了kv3 傻逼v蛇
        ckv1_next_token(ckv1, &token, 1);

        ckv1_next_token(ckv1, &token, 1);
        HasNestKV3 = 1;
    }
    
    if (loadType == LoadType_Array)
    {
        i = 1;

        if (isObject == 0)
        {
            lua_pushlstring(l, ArrayFlag, ArrayFlagLen);
            lua_rawseti(l, -2, i++);
        }
        
        int v = 1;
        while(1)
        {
            ckv1_process_value(l, ckv1, &token);
            lua_rawseti(l, -2, i++);            /* arr[i] = value */
            
            //= or , 
            ckv1_next_token(ckv1, &token, 0);

            //,
            if (token.type == T_COMMA)
            {
                ckv1_next_token(ckv1, &token, 1);
            }

            if (token.type == T_COLON)
            {
                if (isObject == 0)
                {
                    lua_pushinteger(l, v++);
                    lua_rawseti(l, -2, i++);
                }
                
                ckv1_next_token(ckv1, &token, 0);
                ckv1_process_value(l, ckv1, &token);
                lua_rawseti(l, -2, i++);

                ckv1_next_token(ckv1, &token, 1);
            }


            if (token.type == T_ARR_END || (isObject == 1 && token.type == T_OBJ_END))
            {
                // compatible
                if (HasNestKV3 == 1)
                {
                    ckv1_next_token(ckv1, &token, 1);
                }
                ckv1_decode_ascend(ckv1);
                return;
            }
        }
    }
    else
    {
        for (i = 1; ; i++)
        {
            //,
            ckv1_process_value(l, ckv1, &token);
            lua_rawseti(l, -2, i);
            
            //,
            ckv1_next_token(ckv1, &token, 0);
            if (token.type == T_ARR_END) {
                ckv1_decode_ascend(ckv1);
                return;
            }

            //value or ]
            ckv1_next_token(ckv1, &token, 0);
            if (token.type == T_ARR_END) {
                ckv1_decode_ascend(ckv1);
                return;
            }
        }
    }
}

/* Handle the "value" context */
static void ckv1_process_value(lua_State *l, ckv1_parse_t *ckv1,
                               ckv1_token_t *token)
{
    switch (token->type) {
    case T_STRING:
        lua_pushlstring(l, token->value.string, token->string_len);
        break;;
    case T_NUMBER:
        lua_pushnumber(l, token->value.number);
        break;;
    case T_BOOLEAN:
        lua_pushboolean(l, token->value.boolean);
        break;;
    case T_OBJ_BEGIN:
        if (loadType == LoadType_Array)
        {
            ckv1_parse_array_context(l, ckv1, 1);
        }
        else
        {
            ckv1_parse_object_context(l, ckv1);
        }
        
        break;;
    case T_ARR_BEGIN:
        ckv1_parse_array_context(l, ckv1, 0);
        break;;
    case T_NULL:
        /* In Lua, setting "t[k] = nil" will delete k from the table.
         * Hence a NULL pointer lightuserdata object is used instead */
        lua_pushlightuserdata(l, NULL);
        break;;
    default:
        ckv1_throw_parse_error(l, ckv1, "value", token);
    }
}

static int ckv1_decode(lua_State *l)
{
    loadType = LoadType_Map;
    ckv1_parse_t ckv1;
    ckv1_token_t token;
    size_t ckv1_len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    ckv1.cfg = ckv1_fetch_config(l);
    ckv1.data = luaL_checklstring(l, 1, &ckv1_len);
    ckv1.current_depth = 0;
    ckv1.ptr = ckv1.data;

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CKV1 can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (ckv1_len >= 2 && (!ckv1.data[0] || !ckv1.data[1]))
        luaL_error(l, "KV parser does not support UTF-16 or UTF-32");

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire ckv1 string */
    ckv1.tmp = strbuf_new(ckv1_len);

    lua_newtable(l);
    
    ckv1_next_token(&ckv1, &token, 1);
    if (token.type == T_STRING)
    {
        while (1) {
            /* Push key */
            lua_pushlstring(l, token.value.string, token.string_len);

            ckv1_next_token(&ckv1, &token, 0);
            if (token.type == T_COLON)
            {
                /* Fetch value */
                ckv1_next_token(&ckv1, &token, 0);
                ckv1_process_value(l, &ckv1, &token);
            }
            else
            {
                //兼容
                ckv1_process_value(l, &ckv1, &token);
            }
            
            /* Set key = value */
            lua_rawset(l, -3);
            
            ckv1_next_token(&ckv1, &token, 1);

            if (token.type == T_END) {
                break;
            }
        }
    }
    else if (token.type == T_OBJ_BEGIN)
    {
        ckv1_process_value(l, &ckv1, &token);
        
        ckv1_next_token(&ckv1, &token, 0);
        if (token.type != T_END)
            ckv1_throw_parse_error(l, &ckv1, "the end", &token);
    }
    
    strbuf_free(ckv1.tmp);

    return 1;
}

static int ckv1_decode_array(lua_State *l)
{
    loadType = LoadType_Array;
    ckv1_parse_t ckv1;
    ckv1_token_t token;
    size_t ckv1_len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    ckv1.cfg = ckv1_fetch_config(l);
    ckv1.data = luaL_checklstring(l, 1, &ckv1_len);
    ckv1.current_depth = 0;
    ckv1.ptr = ckv1.data;

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CKV1 can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (ckv1_len >= 2 && (!ckv1.data[0] || !ckv1.data[1]))
        luaL_error(l, "KV parser does not support UTF-16 or UTF-32");

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire ckv1 string */
    ckv1.tmp = strbuf_new(ckv1_len);
    
    lua_newtable(l);
    
    ckv1_next_token(&ckv1, &token, 1);
    if (token.type == T_OBJ_BEGIN)
    {
        ckv1_process_value(l, &ckv1, &token);
        lua_rawseti(l, -2, 1);
        
        ckv1_next_token(&ckv1, &token, 0);
        if (token.type != T_END)
            ckv1_throw_parse_error(l, &ckv1, "the end", &token);
    }
    else if (token.type == T_END)
    {
        
    }
    else
    {
        int i = 1;
        for(; ;)
        {
            ckv1_process_value(l, &ckv1, &token);
            lua_rawseti(l, -2, i++);

            ckv1_next_token(&ckv1, &token, 0);
            if (token.type == T_COLON)
            {
                /* Fetch value */
                ckv1_next_token(&ckv1, &token, 0);
                ckv1_process_value(l, &ckv1, &token);
                lua_rawseti(l, -2, i++);
            }
            else
            {
                //兼容
                ckv1_process_value(l, &ckv1, &token);
                lua_rawseti(l, -2, i++);
            }

            ckv1_next_token(&ckv1, &token, 1);

            if (token.type == T_END) {
                break;
            }
        }
    }
    
    strbuf_free(ckv1.tmp);

    return 1;
}

/* ===== INITIALISATION ===== */

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 502
/* Compatibility for Lua 5.1.
 *
 * luaL_setfuncs() is used to create a module table where the functions have
 * ckv1_config_t as their first upvalue. Code borrowed from Lua 5.2 source. */
static void luaL_setfuncs (lua_State *l, const luaL_Reg *reg, int nup)
{
    int i;

    luaL_checkstack(l, nup, "too many upvalues");
    for (; reg->name != NULL; reg++) {  /* fill the table with given functions */
        for (i = 0; i < nup; i++)  /* copy upvalues to the top */
            lua_pushvalue(l, -nup);
        lua_pushcclosure(l, reg->func, nup);  /* closure with those upvalues */
        lua_setfield(l, -(nup + 2), reg->name);
    }
    lua_pop(l, nup);  /* remove upvalues */
}
#endif

/* Call target function in protected mode with all supplied args.
 * Assumes target function only returns a single non-nil value.
 * Convert and return thrown errors as: nil, "error message" */
static int ckv1_protect_conversion(lua_State *l)
{
    int err;

    /* Deliberately throw an error for invalid arguments */
    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    /* pcall() the function stored as upvalue(1) */
    lua_pushvalue(l, lua_upvalueindex(1));
    lua_insert(l, 1);
    err = lua_pcall(l, 1, 1, 0);
    if (!err)
        return 1;

    if (err == LUA_ERRRUN) {
        lua_pushnil(l);
        lua_insert(l, -2);
        return 2;
    }

    /* Since we are not using a custom error handler, the only remaining
     * errors are memory related */
    return luaL_error(l, "Memory allocation error in CKV1 protected call");
}

/* Return ckv1 module table */
static int lua_ckv1_new(lua_State *l)
{
    luaL_Reg reg[] = {
        { "encode", ckv1_encode },
        { "decode", ckv1_decode },
        { "encode_array", ckv1_encode_array },
        { "decode_array", ckv1_decode_array },
        { NULL, NULL }
    };

    /* Initialise number conversions */
    fpconv_init();

    /* ckv1 module table */
    lua_newtable(l);

    /* Register functions with config data as upvalue */
    ckv1_create_config(l);
    luaL_setfuncs(l, reg, 1);

    return 1;
}

#if __DEBUG_KV__
LUAMOD_API int luaopen_ckv1(lua_State *l)
#else
LUALIB_API int luaopen_ckv1(lua_State *l)
#endif
{
    lua_ckv1_new(l);
    return 1;
}