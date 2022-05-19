## CMakeLists.txt:

```cmake
#begin lua_kv
set (CKV_SRC 
lua_kv/lua_kv.c
lua_kv/lua_kv1.c
lua_kv/include/fpconv.c
lua_kv/include/strbuf.c
)
set_property(
	SOURCE ${CKV_SRC}
	APPEND
	PROPERTY COMPILE_DEFINITIONS
	LUA_LIB
)
list(APPEND THIRDPART_INC  lua_kv/include)
set (THIRDPART_SRC ${THIRDPART_SRC} ${CKV_SRC})
if(MSVC)
    add_definitions(-Dstrncasecmp=_strnicmp)
endif()

#end lua_kv
```



## usage:

```lua
--处理npc目录下的文件，功能比较单一
local ckv = require('ckv')
ckv.encode(tb) --字典
ckv.encode2(tb) --数组
ckv.decode(tb) --字典
ckv.decode2(tb) --数组
ckv.ckv_decode_file_array(filepath)  --支持#base 引用其他文件

--处理soundevent等文件，能同时兼容超过3种格式（vsb）
local ckv1 = require('ckv1')
ckv1.encode(tb) --字典
ckv1.encode_array(tb) --数组
ckv1.decode(tb) --字典
ckv1.decode_array(tb) --数组
```

这2种形式主要是数组可以存在多个key一样的



## VisualStudio/Rider 本地调试：

```c
#define __DEBUG_KV__ 1
#define LUA_IDSIZE 120

#define LUA_KVLIBNAME	"ckv"
LUAMOD_API int (luaopen_ckv) (lua_State *L);
```



