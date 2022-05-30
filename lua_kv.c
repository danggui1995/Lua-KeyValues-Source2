#include "include/common.h"

#ifndef CKV_MODNAME
#define CKV_MODNAME   "ckv"
#endif

#ifndef CKV_VERSION
#define CKV_VERSION   "1.0.0"
#endif

typedef enum {
    T_OBJ_BEGIN,
    T_OBJ_END,
    T_STRING,
    T_NUMBER,
    T_BOOLEAN,
    T_NULL,
    T_COLON,
    T_COMMA,
    T_REF,
    T_COMMENT,
    T_END,
    T_WHITESPACE,
    T_ERROR,
    T_UNKNOWN
} ckv_token_type_t;

static const char *ckv_token_type_name[] = {
    "T_OBJ_BEGIN",
    "T_OBJ_END",
    "T_STRING",
    "T_NUMBER",
    "T_BOOLEAN",
    "T_NULL",
    "T_COLON",
    "T_COMMA",
    "T_REF",
    "T_COMMENT",
    "T_END",
    "T_WHITESPACE",
    "T_ERROR",
    "T_UNKNOWN",
    NULL
};

typedef struct {
    ckv_token_type_t ch2token[256];
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
    int keepln;
} ckv_config_t;

typedef struct {
    const char *data;
    const char *ptr;
    strbuf_t *tmp;    /* Temporary storage for strings */
    ckv_config_t *cfg;
    int current_depth;
} ckv_parse_t;

typedef struct {
    ckv_token_type_t type;
    int index;
    union {
        const char *string;
        double number;
        int boolean;
    } value;
    int string_len;
} ckv_token_t;


/* ===== CONFIGURATION ===== */

static ckv_config_t *ckv_fetch_config(lua_State *l)
{
    ckv_config_t *cfg;

    cfg = (ckv_config_t *)lua_touserdata(l, lua_upvalueindex(1));
    if (!cfg)
        luaL_error(l, "BUG: Unable to fetch CKV configuration");

    return cfg;
}

/* Ensure the correct number of arguments have been provided.
 * Pad with nil to allow other functions to simply check arg[i]
 * to find whether an argument was provided */
static ckv_config_t *ckv_arg_init(lua_State *l, int args)
{
    luaL_argcheck(l, lua_gettop(l) <= args, args + 1,
                  "found too many arguments");

    while (lua_gettop(l) < args)
        lua_pushnil(l);

    return ckv_fetch_config(l);
}

/* Process integer options for configuration functions */
static int ckv_integer_option(lua_State *l, int optindex, int *setting,
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
static int ckv_enum_option(lua_State *l, int optindex, int *setting,
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

/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int ckv_cfg_encode_max_depth(lua_State *l)
{
    ckv_config_t *cfg = ckv_arg_init(l, 1);

    return ckv_integer_option(l, 1, &cfg->encode_max_depth, 1, INT_MAX);
}

/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int ckv_cfg_decode_max_depth(lua_State *l)
{
    ckv_config_t *cfg = ckv_arg_init(l, 1);

    return ckv_integer_option(l, 1, &cfg->decode_max_depth, 1, INT_MAX);
}

/* Configures number precision when converting doubles to text */
static int ckv_cfg_encode_number_precision(lua_State *l)
{
    ckv_config_t *cfg = ckv_arg_init(l, 1);

    return ckv_integer_option(l, 1, &cfg->encode_number_precision, 1, 14);
}

/* Configures ckv encoding buffer persistence */
static int ckv_cfg_encode_keep_buffer(lua_State *l)
{
    ckv_config_t *cfg = ckv_arg_init(l, 1);
    int old_value;

    old_value = cfg->encode_keep_buffer;

    ckv_enum_option(l, 1, &cfg->encode_keep_buffer, NULL, 1);

    /* Init / free the buffer if the setting has changed */
    if (old_value ^ cfg->encode_keep_buffer) {
        if (cfg->encode_keep_buffer)
            strbuf_init(&cfg->encode_buf, 0);
        else
            strbuf_free(&cfg->encode_buf);
    }

    return 1;
}

#if defined(DISABLE_INVALID_NUMBERS) && !defined(USE_INTERNAL_FPCONV)
void ckv_verify_invalid_number_setting(lua_State *l, int *setting)
{
    if (*setting == 1) {
        *setting = 0;
        luaL_error(l, "Infinity, NaN, and/or hexadecimal numbers are not supported.");
    }
}
#else
#define ckv_verify_invalid_number_setting(l, s)    do { } while(0)
#endif

static int ckv_cfg_encode_invalid_numbers(lua_State *l)
{
    static const char *options[] = { "off", "on", "null", NULL };
    ckv_config_t *cfg = ckv_arg_init(l, 1);

    ckv_enum_option(l, 1, &cfg->encode_invalid_numbers, options, 1);

    ckv_verify_invalid_number_setting(l, &cfg->encode_invalid_numbers);

    return 1;
}

static int ckv_cfg_decode_invalid_numbers(lua_State *l)
{
    ckv_config_t *cfg = ckv_arg_init(l, 1);

    ckv_enum_option(l, 1, &cfg->decode_invalid_numbers, NULL, 1);

    ckv_verify_invalid_number_setting(l, &cfg->encode_invalid_numbers);

    return 1;
}

static int ckv_destroy_config(lua_State *l)
{
    ckv_config_t *cfg;

    cfg = (ckv_config_t *)lua_touserdata(l, 1);
    if (cfg)
        strbuf_free(&cfg->encode_buf);
    cfg = NULL;

    return 0;
}

static void ckv_create_config(lua_State *l)
{
    ckv_config_t *cfg;
    int i;

    cfg = (ckv_config_t *)lua_newuserdata(l, sizeof(*cfg));

    /* Create GC method to clean up strbuf */
    lua_newtable(l);
    lua_pushcfunction(l, ckv_destroy_config);
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
    cfg->keepln = DEFAULT_ENCODE_KEEPLN;

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
    cfg->ch2token[','] = T_COMMA;
    //cfg->ch2token[':'] = T_COLON;
    cfg->ch2token['\0'] = T_END;
    cfg->ch2token['#'] = T_REF;
    cfg->ch2token[' '] = T_WHITESPACE;
    cfg->ch2token['\t'] = T_WHITESPACE;
    cfg->ch2token['\n'] = T_WHITESPACE;
    cfg->ch2token['\r'] = T_WHITESPACE;
    cfg->ch2token['/'] = T_COMMENT;
    /* Update characters that require further processing */
    cfg->ch2token['f'] = T_UNKNOWN;     /* false? */
    cfg->ch2token['i'] = T_UNKNOWN;     /* inf, ininity? */
    cfg->ch2token['I'] = T_UNKNOWN;
    cfg->ch2token['n'] = T_UNKNOWN;     /* null, nan? */
    cfg->ch2token['N'] = T_UNKNOWN;
    cfg->ch2token['t'] = T_UNKNOWN;     /* true? */
    cfg->ch2token['"'] = T_UNKNOWN;     /* string? */
    cfg->ch2token['+'] = T_UNKNOWN;     /* number? */
    cfg->ch2token['-'] = T_UNKNOWN;
    for (i = 0; i < 10; i++)
        cfg->ch2token['0' + i] = T_UNKNOWN;

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

static void ckv_encode_exception(lua_State *l, ckv_config_t *cfg, strbuf_t *ckv, int lindex,
                                  const char *reason)
{
    if (!cfg->encode_keep_buffer)
        strbuf_free(ckv);
    luaL_error(l, "Cannot serialise %s: %s",
                  lua_typename(l, lua_type(l, lindex)), reason);
}

/* ckv_append_string args:
 * - lua_State
 * - ckv strbuf
 * - String (Lua stack index)
 *
 * Returns nothing. Doesn't remove string from Lua stack */
static void ckv_append_string(lua_State *l, strbuf_t *ckv, int lindex)
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
    strbuf_ensure_empty_length(ckv, len * 6 + 2);

    strbuf_append_char_unsafe(ckv, '\"');
    for (i = 0; i < len; i++) {
        escstr = char2escape[(unsigned char)str[i]];
        if (escstr)
            strbuf_append_string(ckv, escstr);
        else
            strbuf_append_char_unsafe(ckv, str[i]);
    }
    strbuf_append_char_unsafe(ckv, '\"');
}


static void ckv_append_number(lua_State *l, ckv_config_t *cfg,
                               strbuf_t *ckv, int lindex)
{
    double num = lua_tonumber(l, lindex);
    int len;

    if (cfg->encode_invalid_numbers == 0) {
        /* Prevent encoding invalid numbers */
        if (isinf(num) || isnan(num))
            ckv_encode_exception(l, cfg, ckv, lindex,
                                  "must not be NaN or Infinity");
    } else if (cfg->encode_invalid_numbers == 1) {
        /* Encode NaN/Infinity separately to ensure Javascript compatible
         * values are used. */
        if (isnan(num)) {
            strbuf_append_mem(ckv, "NaN", 3);
            return;
        }
        if (isinf(num)) {
            if (num < 0)
                strbuf_append_mem(ckv, "-Infinity", 9);
            else
                strbuf_append_mem(ckv, "Infinity", 8);
            return;
        }
    } else {
        /* Encode invalid numbers as "null" */
        if (isinf(num) || isnan(num)) {
            strbuf_append_mem(ckv, "null", 4);
            return;
        }
    }

    strbuf_ensure_empty_length(ckv, FPCONV_G_FMT_BUFSIZE);
    len = fpconv_g_fmt(strbuf_empty_ptr(ckv), num, cfg->encode_number_precision);
    strbuf_extend_length(ckv, len);
}

static void ckv_check_encode_depth(lua_State *l, ckv_config_t *cfg,
                                    int current_depth, strbuf_t *ckv)
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
        strbuf_free(ckv);

    luaL_error(l, "Cannot serialise, excessive nesting (%d)",
               current_depth);
}

static int lua_array_length(lua_State *l, ckv_config_t *cfg, strbuf_t *ckv)
{
    double k;
    int max;
    int items;

    max = 0;
    items = 0;

    lua_pushnil(l);
    /* table, startkey */
    while (lua_next(l, -2) != 0) {
        /* table, key, value */
        if (lua_type(l, -2) == LUA_TNUMBER &&
            (k = lua_tonumber(l, -2))) {
            /* Integer >= 1 ? */
            if (floor(k) == k && k >= 1) {
                if (k > max)
                    max = k;
                items++;
                lua_pop(l, 1);
                continue;
            }
        }

        /* Must not be an array (non integer key) */
        lua_pop(l, 2);
        return -1;
    }

    /* Encode excessively sparse arrays as objects (if enabled) */
    if (cfg->encode_sparse_ratio > 0 &&
        max > items * cfg->encode_sparse_ratio &&
        max > cfg->encode_sparse_safe) {
        if (!cfg->encode_sparse_convert)
            ckv_encode_exception(l, cfg, ckv, -1, "excessively sparse array");

        return -1;
    }

    return max;
}

static void ckv_append_data(lua_State *l, ckv_config_t *cfg,
                             int current_depth, strbuf_t *ckv);
static void ckv_append_data2(lua_State *l, ckv_config_t *cfg,
                             int current_depth, strbuf_t *ckv);

static void ckv_append_object(lua_State *l, ckv_config_t *cfg,
                               int current_depth, strbuf_t *ckv)
{
    if (cfg->keepln)
    {
        strbuf_append_char(ckv, '\n');
        for (int i = 0; i < current_depth - 1; i++)
        {
            strbuf_append_char(ckv, '\t');
        }
    }

    strbuf_append_char(ckv, '{');
    if (cfg->keepln)
    {
        strbuf_append_char(ckv, '\n');
    }

    lua_pushnil(l);
    while (lua_next(l, -2) != 0) 
    {
        if (cfg->keepln)
        {
            for (int i = 0; i < current_depth; i++)
            {
                strbuf_append_char(ckv, '\t');
            }
        }
        
        ckv_append_string(l, ckv, -2);
        strbuf_append_char(ckv, '\t');

        //value
        ckv_append_data(l, cfg, current_depth, ckv);
        lua_pop(l, 1);

        if (cfg->keepln)
        {
            strbuf_append_char(ckv, '\n');
        }
    }

    if (cfg->keepln)
    {
        for (int i = 0; i < current_depth - 1; i++)
        {
            strbuf_append_char(ckv, '\t');
        }
    }
    strbuf_append_char(ckv, '}');
}

static void ckv_append_array(lua_State *l, ckv_config_t *cfg, int current_depth,
                              strbuf_t *ckv, int array_length)
{
    if (cfg->keepln)
    {
        strbuf_append_char(ckv, '\n');
        for (int i = 0; i < current_depth - 1; i++)
        {
            strbuf_append_char(ckv, '\t');
        }
    }

    strbuf_append_char(ckv, '{');
    if (cfg->keepln)
    {
        strbuf_append_char(ckv, '\n');
    }


    for (int i = 1; i <= array_length; i+=2) {
        if (cfg->keepln)
        {
            for (int i = 0; i < current_depth; i++)
            {
                strbuf_append_char(ckv, '\t');
            }
        }
        lua_rawgeti(l, -1, i);
        ckv_append_data2(l, cfg, current_depth, ckv);
        lua_pop(l, 1);

        strbuf_append_char(ckv, '\t');

        lua_rawgeti(l, -1, i + 1);
        ckv_append_data2(l, cfg, current_depth, ckv);
        lua_pop(l, 1);
        
        if (cfg->keepln)
        {
            strbuf_append_char(ckv, '\n');
        }
    }

    if (cfg->keepln)
    {
        for (int i = 0; i < current_depth - 1; i++)
        {
            strbuf_append_char(ckv, '\t');
        }
    }
    strbuf_append_char(ckv, '}');
}

/* Serialise Lua data into ckv string. */
static void ckv_append_data(lua_State *l, ckv_config_t *cfg,
                             int current_depth, strbuf_t *ckv)
{
    switch (lua_type(l, -1)) 
    {
        case LUA_TSTRING:
            ckv_append_string(l, ckv, -1);
            break;
        case LUA_TTABLE:
        {
            current_depth++;
            ckv_check_encode_depth(l, cfg, current_depth, ckv);
            ckv_append_object(l, cfg, current_depth, ckv);
            break;
        }
            
        case LUA_TNIL:
            strbuf_append_mem(ckv, "null", 4);
            break;

        case LUA_TNUMBER:
            ckv_append_number(l, cfg, ckv, -1);
            break;

        case LUA_TLIGHTUSERDATA:
            if (lua_touserdata(l, -1) == NULL) {
                strbuf_append_mem(ckv, "null", 4);
                break;
            }
        default:
            /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
             * and LUA_TLIGHTUSERDATA) cannot be serialised */
            ckv_encode_exception(l, cfg, ckv, -1, "type not supported");
            /* never returns */
    }
}

/* Serialise Lua data into ckv string. */
static void ckv_append_data2(lua_State *l, ckv_config_t *cfg,
                             int current_depth, strbuf_t *ckv)
{
    switch (lua_type(l, -1)) 
    {
        case LUA_TSTRING:
            ckv_append_string(l, ckv, -1);
            break;
        case LUA_TTABLE:
        {
            current_depth++;
            ckv_check_encode_depth(l, cfg, current_depth, ckv);
            int len = lua_array_length(l, cfg, ckv);
            ckv_append_array(l, cfg, current_depth, ckv, len);
            break;
        }
            
        case LUA_TNIL:
            strbuf_append_mem(ckv, "null", 4);
            break;

        case LUA_TNUMBER:
            ckv_append_number(l, cfg, ckv, -1);
            break;

        case LUA_TLIGHTUSERDATA:
            if (lua_touserdata(l, -1) == NULL) {
                strbuf_append_mem(ckv, "null", 4);
                break;
            }
        default:
            /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
             * and LUA_TLIGHTUSERDATA) cannot be serialised */
            ckv_encode_exception(l, cfg, ckv, -1, "type not supported");
            /* never returns */
    }
}


static int ckv_encode(lua_State *l)
{
    ckv_config_t *cfg = ckv_fetch_config(l);
    strbuf_t local_encode_buf;
    strbuf_t *encode_buf;
    char *ckv;
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
    lua_next(l, -2);

    //key
    ckv_append_string(l, encode_buf, -2);
    strbuf_append_char(encode_buf, '\t');

    //value
    ckv_append_data(l, cfg, 0, encode_buf);
    ckv = strbuf_string(encode_buf, &len);

    lua_pushlstring(l, ckv, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(encode_buf);

    return 1;
}

static int ckv_encode2(lua_State *l)
{
    ckv_config_t *cfg = ckv_fetch_config(l);
    strbuf_t local_encode_buf;
    strbuf_t *encode_buf;
    char *ckv;
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
    lua_next(l, -2);

    //key
    ckv_append_string(l, encode_buf, -2);
    strbuf_append_char(encode_buf, '\t');

    //value
    ckv_append_data2(l, cfg, 0, encode_buf);
    ckv = strbuf_string(encode_buf, &len);

    lua_pushlstring(l, ckv, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(encode_buf);

    return 1;
}

/* ===== DECODING ===== */

static void ckv_process_value(lua_State *l, ckv_parse_t *ckv,
                               ckv_token_t *token);
static void ckv_process_value2(lua_State *l, ckv_parse_t *ckv,
                               ckv_token_t *token);

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
static int ckv_append_unicode_escape(ckv_parse_t *ckv)
{
    char utf8[4];       /* Surrogate pairs require 4 UTF-8 bytes */
    int codepoint;
    int surrogate_low;
    int len;
    int escape_len = 6;

    /* Fetch UTF-16 code unit */
    codepoint = decode_hex4(ckv->ptr + 2);
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
        if (*(ckv->ptr + escape_len) != '\\' ||
            *(ckv->ptr + escape_len + 1) != 'u') {
            return -1;
        }

        /* Fetch the next codepoint */
        surrogate_low = decode_hex4(ckv->ptr + 2 + escape_len);
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
    strbuf_append_mem_unsafe(ckv->tmp, utf8, len);
    ckv->ptr += escape_len;

    return 0;
}

static void ckv_set_token_error(ckv_token_t *token, ckv_parse_t *ckv,
                                 const char *errtype)
{
    token->type = T_ERROR;
    token->index = ckv->ptr - ckv->data;
    token->value.string = errtype;
#if __DEBUG_KV__
    printf("token error : %s", errtype);
#endif
}

static void ckv_next_string_token(ckv_parse_t *ckv, ckv_token_t *token)
{
    char *escape2char = ckv->cfg->escape2char;
    char ch;

    /* Caller must ensure a string is next */
    assert(*ckv->ptr == '"');

    /* Skip " */
    ckv->ptr++;
    /* ckv->tmp is the temporary strbuf used to accumulate the
     * decoded string value.
     * ckv->tmp is sized to handle ckv containing only a string value.
     */
    strbuf_reset(ckv->tmp);

    while ((ch = *ckv->ptr) != '"') {
        if (!ch) {
            /* Premature end of the string */
            ckv_set_token_error(token, ckv, "unexpected end of string");
            return;
        }

        /* Handle escapes */
        if (ch == '\\') {
            /* Fetch escape character */
            ch = *(ckv->ptr + 1);

            /* Translate escape code and append to tmp string */
            ch = escape2char[(unsigned char)ch];
            if (ch == 'u') {
                if (ckv_append_unicode_escape(ckv) == 0)
                    continue;

                ckv_set_token_error(token, ckv,
                                     "invalid unicode escape code");
                return;
            }
            if (!ch) {
                ckv_set_token_error(token, ckv, "invalid escape code");
                return;
            }

            /* Skip '\' */
            ckv->ptr++;
        }

        /* Append normal character or translated single character
         * Unicode escapes are handled above */
        strbuf_append_char_unsafe(ckv->tmp, ch);
        ckv->ptr++;
    }
    ckv->ptr++;    /* Eat final quote (") */

    strbuf_ensure_null(ckv->tmp);

    token->type = T_STRING;
    token->value.string = strbuf_string(ckv->tmp, &token->string_len);
}


/* ckv numbers should take the following form:
 *      -?(0|[1-9]|[1-9][0-9]+)(.[0-9]+)?([eE][-+]?[0-9]+)?
 *
 * ckv_next_number_token() uses strtod() which allows other forms:
 * - numbers starting with '+'
 * - NaN, -NaN, infinity, -infinity
 * - hexadecimal numbers
 * - numbers with leading zeros
 *
 * ckv_is_invalid_number() detects "numbers" which may pass strtod()'s
 * error checking, but should not be allowed with strict ckv.
 *
 * ckv_is_invalid_number() may pass numbers which cause strtod()
 * to generate an error.
 */
static int ckv_is_invalid_number(ckv_parse_t *ckv)
{
    const char *p = ckv->ptr;

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

static void ckv_next_number_token(ckv_parse_t *ckv, ckv_token_t *token)
{
    char *endptr;

    token->type = T_NUMBER;
    token->value.number = fpconv_strtod(ckv->ptr, &endptr);
    if (ckv->ptr == endptr)
        ckv_set_token_error(token, ckv, "invalid number");
    else
        ckv->ptr = endptr;     /* Skip the processed number */
}


/* Fills in the token struct.
 * T_STRING will return a pointer to the ckv_parse_t temporary string
 * T_ERROR will leave the ckv->ptr pointer at the error.
 */
static void ckv_next_token(ckv_parse_t *ckv, ckv_token_t *token)
{
    const ckv_token_type_t *ch2token = ckv->cfg->ch2token;
    int ch;

    /* Eat whitespace. */
    while (1) {
        ch = (unsigned char)*(ckv->ptr);
        token->type = ch2token[ch];
        if (token->type != T_WHITESPACE)
        {
            if (token->type == T_COMMENT)
            {
                while (1)
                {
                    ckv->ptr++;
                    ch = (unsigned char)*(ckv->ptr);
                    if (ch == '\r' || ch == '\n')
                    {
                        break;
                    }
                }
            }
#if __DEBUG_KV__
            else if (token->type == T_ERROR)
            {
                printf("error token: %c = %d in %s \n", ch, ch, (ckv->ptr));
            }
#endif
            else
            {
                break;
            }
        }
        ckv->ptr++;
    }

    /* Store location of new token. Required when throwing errors
     * for unexpected tokens (syntax errors). */
    token->index = ckv->ptr - ckv->data;

    /* Don't advance the pointer for an error or the end */
    if (token->type == T_ERROR) {
        ckv_set_token_error(token, ckv, "invalid token 1057");
        return;
    }

    if (token->type == T_END) {
        return;
    }

    /* Found a known single character token, advance index and return */
    if (token->type != T_UNKNOWN) {
        ckv->ptr++;
        return;
    }

    if (ch == '"') {
        ckv_next_string_token(ckv, token);
        return;
    }
    else if (ch == '-' || ('0' <= ch && ch <= '9')) {
        if (!ckv->cfg->decode_invalid_numbers && ckv_is_invalid_number(ckv)) {
            ckv_set_token_error(token, ckv, "invalid number");
            return;
        }
        ckv_next_number_token(ckv, token);
        return;
    } 
    else if (ckv->cfg->decode_invalid_numbers &&
               ckv_is_invalid_number(ckv)) {
        /* When decode_invalid_numbers is enabled, only attempt to process
         * numbers we know are invalid ckv (Inf, NaN, hex)
         * This is required to generate an appropriate token error,
         * otherwise all bad tokens will register as "invalid number"
         */
        ckv_next_number_token(ckv, token);
        return;
    }

    /* Token starts with t/f/n but isn't recognised above. */
    ckv_set_token_error(token, ckv, "invalid token 1");
}

/* This function does not return.
 * DO NOT CALL WITH DYNAMIC MEMORY ALLOCATED.
 * The only supported exception is the temporary parser string
 * ckv->tmp struct.
 * ckv and token should exist on the stack somewhere.
 * luaL_error() will long_jmp and release the stack */
static void ckv_throw_parse_error(lua_State *l, ckv_parse_t *ckv,
                                   const char *exp, ckv_token_t *token)
{
    const char *found;

    strbuf_free(ckv->tmp);

    if (token->type == T_ERROR)
        found = token->value.string;
    else
        found = ckv_token_type_name[token->type];

    /* Note: token->index is 0 based, display starting from 1 */
    luaL_error(l, "Expected %s but found %s at character %d",
               exp, found, token->index + 1);

#if __DEBUG_KV__
    printf("Expected %s but found %s at character %d",
               exp, found, token->index + 1);
#endif
}

static inline void ckv_decode_ascend(ckv_parse_t *ckv)
{
    ckv->current_depth--;
}

static void ckv_decode_descend(lua_State *l, ckv_parse_t *ckv, int slots)
{
    ckv->current_depth++;

    if (ckv->current_depth <= ckv->cfg->decode_max_depth &&
        lua_checkstack(l, slots)) {
        return;
    }

    strbuf_free(ckv->tmp);
    luaL_error(l, "Found too many nested data structures (%d) at character %d",
        ckv->current_depth, ckv->ptr - ckv->data);
}

static void ckv_parse_object_context(lua_State *l, ckv_parse_t *ckv)
{
    ckv_token_t token;

    ckv_decode_descend(l, ckv, 3);

    lua_newtable(l);

    ckv_next_token(ckv, &token);

    /* Handle empty objects */
    if (token.type == T_OBJ_END) {
        ckv_decode_ascend(ckv);
        return;
    }

    while (1) {
        if (token.type != T_STRING)
            ckv_throw_parse_error(l, ckv, "object key string", &token);

        /* Push key */
        lua_pushlstring(l, token.value.string, token.string_len);

        /* Fetch value */
        ckv_next_token(ckv, &token);
        ckv_process_value(l, ckv, &token);

        /* Set key = value */
        lua_rawset(l, -3);

        ckv_next_token(ckv, &token);

        if (token.type == T_OBJ_END) {
            ckv_decode_ascend(ckv);
            return;
        }
    }
}


/* Handle the "value" context */
static void ckv_process_value(lua_State *l, ckv_parse_t *ckv,
                               ckv_token_t *token)
{
    switch (token->type) {
        case T_STRING:
            lua_pushlstring(l, token->value.string, token->string_len);
            break;;
        case T_OBJ_BEGIN:
            ckv_parse_object_context(l, ckv);
            break;;
        case T_NUMBER:
            lua_pushnumber(l, token->value.number);
            break;;
        case T_BOOLEAN:
            lua_pushboolean(l, token->value.boolean);
        case T_NULL:
            /* In Lua, setting "t[k] = nil" will delete k from the table.
             * Hence a NULL pointer lightuserdata object is used instead */
            lua_pushlightuserdata(l, NULL);
            break;;
        default:
            ckv_throw_parse_error(l, ckv, "value", token);
    }
}

/* Handle the array context */
static void ckv_parse_array_context(lua_State *l, ckv_parse_t *ckv)
{
    ckv_token_t token;
    int i;

    /* 2 slots required:
     * .., table, value */
    ckv_decode_descend(l, ckv, 2);

    lua_newtable(l);

    ckv_next_token(ckv, &token);

    /* Handle empty arrays */
    if (token.type == T_OBJ_END) {
        ckv_decode_ascend(ckv);
        return;
    }

    for (i = 1; ; i++) {
        ckv_process_value2(l, ckv, &token);
        lua_rawseti(l, -2, i);            /* arr[i] = value */

        ckv_next_token(ckv, &token);

        if (token.type == T_OBJ_END) {
            ckv_decode_ascend(ckv);
            return;
        }
    }
}

/* Handle the "value" context */
static void ckv_process_value2(lua_State *l, ckv_parse_t *ckv,
                               ckv_token_t *token)
{
    switch (token->type) {
        case T_STRING:
            lua_pushlstring(l, token->value.string, token->string_len);
            break;;
        case T_OBJ_BEGIN:
            ckv_parse_array_context(l, ckv);
            break;;
        case T_NUMBER:
            lua_pushnumber(l, token->value.number);
            break;;
        case T_BOOLEAN:
            lua_pushboolean(l, token->value.boolean);
            break;;
        case T_NULL:
            /* In Lua, setting "t[k] = nil" will delete k from the table.
             * Hence a NULL pointer lightuserdata object is used instead */
            lua_pushlightuserdata(l, NULL);
            break;;
        default:
            ckv_throw_parse_error(l, ckv, "value", token);
    }
}

static int ckv_decode(lua_State *l)
{
    ckv_parse_t ckv;
    ckv_token_t token;
    size_t ckv_len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    ckv.cfg = ckv_fetch_config(l);
    ckv.data = luaL_checklstring(l, 1, &ckv_len);
    lua_pop(l, 1);
    ckv.current_depth = 0;
    ckv.ptr = ckv.data;
    
    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CKV can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (ckv_len >= 2 && (!ckv.data[0] || !ckv.data[1]))
        luaL_error(l, "ckv parser does not support UTF-16 or UTF-32");

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire ckv string */
    ckv.tmp = strbuf_new(ckv_len);

    lua_newtable(l);
        
    //key
    ckv_next_token(&ckv, &token);
    if (token.type != T_END)
    {
        ckv_process_value(l, &ckv, &token);

        //value
        ckv_next_token(&ckv, &token);
        ckv_process_value(l, &ckv, &token);

        lua_rawset(l, -3);
    }
    
    strbuf_free(ckv.tmp);

    return 1;
}

static void ckv_checkref(ckv_parse_t *ckv, ckv_token_t *token, lua_State* l, const char* filename);

static const char* ckv_get_filename(const char* fullpath, char filename[128])
{
    const int len = strlen(fullpath);
    int i = len - 1;
    int start = 0;
    for(;i >= 0; i--)
    {
        if (fullpath[i] == '\\' || fullpath[i] == '/')
        {
            start = i + 1;
            break;
        }
    }

    i = start;
    start = 0;
    for(; i < len; i++)
    {
        filename[start++] = fullpath[i];
    }
    filename[start] = '\0';
    return filename;
}

static void ckv_decode_file(lua_State* l, const char* fullpath)
{
    ckv_parse_t ckv;
    ckv_token_t token;
    FILE* fp = NULL;
    fopen_s(&fp, fullpath, "rb");
    if (fp == NULL)
    {
        luaL_fileresult(l, 0, fullpath);
        return;
    }
    
    size_t nr;
    strbuf_t* filebuffer = strbuf_new(LUAL_BUFFERSIZE);
    
    do
    {
        char tempbuffer[LUAL_BUFFERSIZE];
        nr = fread(tempbuffer, sizeof(char), LUAL_BUFFERSIZE, fp);
        strbuf_append_mem(filebuffer, tempbuffer, nr);
    } while (nr == LUAL_BUFFERSIZE);

    const int res = fclose(fp);
    if (res != 0)
    {
        luaL_fileresult(l, 0, "fclose failed");
    }
    
    ckv.cfg = ckv_fetch_config(l);
    ckv.data = filebuffer->buf;
    ckv.current_depth = 0;
    ckv.ptr = ckv.data;
    const size_t ckv_len = filebuffer->size;

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CKV can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */


    short checkUTF8 = 0;
    if (ckv_len > 2)
    {
        unsigned char bt1 = (unsigned char)(ckv.data[0]);
        unsigned char bt2 = (unsigned char)(ckv.data[1]);
        unsigned char bt3 = (unsigned char)(ckv.data[2]);
#if __DEBUG_KV__
        printf("encode byte = %d %d %d \n", bt1, bt2, bt3);
#endif
        if (bt1 == 239 && bt2 == 187 && bt3 == 191)
        {
            //EF BB BF
            ckv.ptr += 3;
            checkUTF8 = 1;
        }
    }
    if (checkUTF8 == 0)
    {
        const char ch = (unsigned char)*(ckv.ptr);
        if (ckv.cfg->ch2token[ch] == T_ERROR)
        {
            luaL_error(l, "ckv parser just support UTF-8");
            return;
        }
    }
    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire ckv string */
    ckv.tmp = strbuf_new(ckv_len);

    //引入了其他kv文件
    ckv_checkref(&ckv, &token, l, fullpath);
    //在外面包一层
    lua_newtable(l);

    char filename[128];
    ckv_get_filename(fullpath, filename);
    lua_pushlstring(l, filename, strlen(filename));

    {
        lua_newtable(l);
        //第一个永远是表的名字
        ckv_next_token(&ckv, &token);
        if (token.type != T_END)
        {
            ckv_process_value(l, &ckv, &token);
    
            //value
            ckv_next_token(&ckv, &token);
            ckv_process_value2(l, &ckv, &token);
        
            lua_rawset(l, -3);
        }
    }
    
    lua_rawset(l, -3);
    strbuf_free(ckv.tmp);
    strbuf_free(filebuffer);
}

static void ckv_checkref(ckv_parse_t *ckv, ckv_token_t *token, lua_State* l, const char* fullpath)
{
    const ckv_token_type_t *ch2token = ckv->cfg->ch2token;
    int ch = 0, i = 0;
    const int len = strlen(fullpath);
    
    while (1) {
        ch = (unsigned char)*(ckv->ptr);
        token->type = ch2token[ch];
        if (token->type != T_WHITESPACE)
        {
            if (token->type == T_COMMENT)
            {
                while (1)
                {
                    ckv->ptr++;
                    ch = (unsigned char)*(ckv->ptr);
                    if (ch == '\r' || ch == '\n')
                    {
                        break;
                    }
                }
            }
            else if (token->type == T_REF)
            {
                while (1)
                {
                    ckv->ptr++;
                    ch = (unsigned char)*(ckv->ptr);
                    if (ch == '\"')
                    {
                        strbuf_reset(ckv->tmp);
                        while (1)
                        {
                            ckv->ptr++;
                            ch = (unsigned char)*(ckv->ptr);
                            if (ch == '\"')
                            {
                                strbuf_append_char_unsafe(ckv->tmp, '\0');
                                char* buf = strbuf_string(ckv->tmp, &token->string_len);
                                char dir[256];

                                int dindex = -1;
                                for(i = len - 1; i >= 0; i--)
                                {
                                    if ((fullpath[i] == '\\' || fullpath[i] == '/') && dindex < 0)
                                    {
                                        dindex = i;
                                        dir[i + 1] = '\0';
                                    }
                                    
                                    if (dindex >= 0)
                                    {
                                        dir[i] = fullpath[i];
                                    }
                                }
                                
                                char newfilepath[256];
                                sprintf_s(newfilepath, 256, "%s/%s", dir, buf);
                                ckv_decode_file(l, newfilepath);
                                
                                break;
                            }
                            else
                            {
                                strbuf_append_char_unsafe(ckv->tmp, ch);
                            }
                        }
                        break;
                    }
                }
            }
            else
            {
                break;
            }
        }
        ckv->ptr++;
    }
}

static int ckv_decode2(lua_State *l)
{
    ckv_parse_t ckv;
    ckv_token_t token;
    size_t ckv_len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    ckv.cfg = ckv_fetch_config(l);
    ckv.data = luaL_checklstring(l, 1, &ckv_len);
    lua_pop(l, 1);
    ckv.current_depth = 0;
    ckv.ptr = ckv.data;

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CKV can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (ckv_len >= 2 && (!ckv.data[0] || !ckv.data[1]))
        luaL_error(l, "ckv parser does not support UTF-16 or UTF-32");

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire ckv string */
    ckv.tmp = strbuf_new(ckv_len);

    lua_newtable(l);
        
    //第一个永远是表的名字
    ckv_next_token(&ckv, &token);

    if (token.type != T_END)
    {
        ckv_process_value(l, &ckv, &token);

        //value
        ckv_next_token(&ckv, &token);
        ckv_process_value2(l, &ckv, &token);

        lua_rawset(l, -3);
    }
    
    strbuf_free(ckv.tmp);

    return 1;
}

static int ckv_decode_file_array(lua_State *l)
{
    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");
    
    size_t filepath_len;
    const char *filepath = luaL_checklstring(l, 1, &filepath_len);
    lua_pop(l, 1);
    lua_newtable(l);
    ckv_decode_file(l, filepath);

    return 1;
}

/* ===== INITIALISATION ===== */

/* Return ckv module table */
static int lua_ckv_new(lua_State *l)
{
    luaL_Reg reg[] = {
        { "encode", ckv_encode },
        { "decode", ckv_decode },
        { "encode2", ckv_encode2 },
        { "decode2", ckv_decode2 },
        { "decode_file_array", ckv_decode_file_array },
        { NULL, NULL }
    };

    fpconv_init();

    lua_newtable(l);

    ckv_create_config(l);
    luaL_setfuncs(l, reg, 1);
    
    return 1;
}

#if __DEBUG_KV__
LUAMOD_API int luaopen_ckv(lua_State *l)
#else
LUALIB_API int luaopen_ckv(lua_State *l)
#endif
{
    lua_ckv_new(l);
    return 1;
}