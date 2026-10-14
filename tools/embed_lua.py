#!/usr/bin/env python3
"""
把 Lua 脚本嵌入为 C 字符串字面量头文件（固件内置预制脚本）。
由 pico-hid-mapper 的 tools/embed_lua.py 拷贝。

用法:
  embed_lua.py IN.lua OUT.h [--name VAR]

输出 `static const char VAR[]` 字符串字面量，保持逐行拼接的可读形式，
UTF-8 中文原样写出，便于 diff / code review。
"""
import re
import sys


def guard_from_path(out_path):
    """由输出文件名派生头文件保护宏，如 src/lua_default_script.h → LUA_DEFAULT_SCRIPT_H"""
    base = out_path.replace('\\', '/').rsplit('/', 1)[-1]
    return re.sub(r'[^A-Za-z0-9]', '_', base).upper()


def escape(line):
    """转义为 C 字符串字面量内容。行尾换行由调用方补 \\n。"""
    out = []
    for ch in line:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\t':
            out.append('\\t')
        elif ch == '\r':
            pass  # 统一为 LF，丢弃 CR
        else:
            out.append(ch)
    return ''.join(out)


def main():
    args = sys.argv[1:]
    var_name = 'LUA_DEFAULT_SCRIPT'
    if '--name' in args:
        i = args.index('--name')
        var_name = args[i + 1]
        del args[i:i + 2]

    if len(args) != 2:
        sys.exit("用法: embed_lua.py IN.lua OUT.h [--name VAR]")
    in_path, out_path = args

    with open(in_path, 'r', encoding='utf-8') as f:
        lines = f.read().split('\n')

    guard = guard_from_path(out_path)
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('// 自动生成，请勿手动编辑（由 tools/embed_lua.py 从 '
                f'{in_path} 生成）\n')
        f.write(f'#ifndef {guard}\n')
        f.write(f'#define {guard}\n\n')
        f.write(f'static const char {var_name}[] =\n')
        for line in lines:
            f.write(f'    "{escape(line)}\\n"\n')
        f.write('    ;\n\n')
        f.write(f'#endif  // {guard}\n')

    print(f'{in_path} -> {out_path} (var {var_name}, {len(lines)} lines)')


if __name__ == '__main__':
    main()
