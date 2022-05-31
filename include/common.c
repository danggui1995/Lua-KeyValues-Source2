#include "common.h"

void printLuaStack(lua_State* l)
{
    int stackTop = lua_gettop(l);
    int nType;

    printf("--start(%d)--\n", stackTop);
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

void InitTabCache()
{
    if (tabCache[0] != NULL)
    {
        return;
    }
    int i,j;
    for(i = 0; i < MAX_TAB; i++)
    {
        char* buf = malloc(i + 1);
        for(j = 0; j < i; j++)
        {
            buf[j] = '\t';
        }
        buf[j] = '\0';
        tabCache[i] = buf;
    }
}