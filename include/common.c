#include "common.h"

void printLuaStack(lua_State* l)
{
    int stackTop = lua_gettop(l);
    int nType;

    printf("--start(v)(%d)--\n", stackTop);
    //显示栈中的元素
    while (stackTop > 0)
    {
        nType = lua_type(l, stackTop);
        printf("[%d] %s = %.*s\n", stackTop, lua_typename(l, nType), 128, lua_tostring(l, stackTop));
        stackTop--;
    }

    printf("--stop--\n");
    fflush(stdout);
}