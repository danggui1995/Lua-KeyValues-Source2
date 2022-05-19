
-- local file = io.open("C:/Users/Administrator/Desktop/dota2tools/soundevent/game_sounds.vsndevts", "r")
-- local data = file:read("*a")
-- local ckv1 = require "ckv1"
-- local tb = ckv1.decode_array(data)

-- function dump(tb, dump_metatable, max_level)
-- 	local lookup_table = {}
-- 	local level = 0
-- 	local rep = string.rep
-- 	local dump_metatable = dump_metatable

-- 	local function _dump(tb, level)
-- 		local str = "\n" .. rep("\t", level) .. "{\n"
-- 		for k,v in pairs(tb) do
-- 			local k_is_str = type(k) == "string" and 1 or 0
-- 			local v_is_str = type(v) == "string" and 1 or 0
-- 			str = str..rep("\t", level + 1).."["..rep("\"", k_is_str)..(tostring(k) or type(k))..rep("\"", k_is_str).."]".." = "
-- 			if type(v) == "table" then
-- 				if not lookup_table[v] and ((not max_level) or level < max_level) then
-- 					lookup_table[v] = true
-- 					str = str.._dump(v, level + 1, dump_metatable).."\n"
-- 				else
-- 					str = str..(tostring(v) or type(v))..",\n"
-- 				end
-- 			else 
-- 				str = str..rep("\"", v_is_str)..(tostring(v) or type(v))..rep("\"", v_is_str)..",\n"
-- 			end
-- 		end
-- 		if dump_metatable then
-- 			local mt = getmetatable(tb)
-- 			if mt ~= nil and type(mt) == "table" then
-- 				str = str..rep("\t", level + 1).."[\"__metatable\"]".." = "
-- 				if not lookup_table[mt] and ((not max_level) or level < max_level) then
-- 					lookup_table[mt] = true
-- 					str = str.._dump(mt, level + 1, dump_metatable).."\n"
-- 				else
-- 					str = str..(tostring(v) or type(v))..",\n"
-- 				end
-- 			end
-- 		end
-- 		str = str..rep("\t", level) .. "},"
-- 		return str
-- 	end
	
-- 	print(_dump(tb, level))
-- end

--[[

]#define LUA_IDSIZE 120

]]

local a = {nil,1, nil,2,nil,3,nil,4}
local b = {a=1,b=3}
print(#b)