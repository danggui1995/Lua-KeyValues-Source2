
#include        <stdio.h>
#include        "lua/lua.h"
#include        "lua/lualib.h"
#include        "lua/lauxlib.h"
lua_State* L;
int main(int argc, char* argv[])
{
    L = luaL_newstate();
    luaL_openlibs(L);
    luaL_dofile(L, "test.lua");
    lua_close(L);
    return 0;
}
