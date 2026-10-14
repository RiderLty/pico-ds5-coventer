#ifndef DS5_HIDKIT_USER_CONFIG_H
#define DS5_HIDKIT_USER_CONFIG_H

/*
 * 本工程的 hidkit 库配置覆写（经 -DHIDKIT_USER_CONFIG 引入，见 src/CMakeLists.txt）。
 *
 * 为什么不走 -DHIDKIT_HOT(f)=… 编译定义：带括号的宏形状定义会被 CMake 的
 * target_compile_definitions 静默丢弃（flags.make 里根本不出现），库热路径
 * 就悄悄留在 flash 了 —— 官方给的扩展点就是 HIDKIT_USER_CONFIG 配置头。
 *
 * HIDKIT_HOT = noinline + not_in_flash：noinline 防 static/inline 函数被内联回
 * flash 段的调用者里；not_in_flash 是热路径不停摆的保证。宿主侧 hidkit_input_*
 * 出口已在 input_app.c 用同一宏标注，整条输入链才真正不在 RAM 之外。
 */

#include "pico.h"

#define HIDKIT_HOT(f) __no_inline_not_in_flash_func(f)

#endif /* DS5_HIDKIT_USER_CONFIG_H */
