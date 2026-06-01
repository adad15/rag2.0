#pragma once
namespace logging {
// 初始化全局 logger（控制台 + 时间戳），可重复调用安全。
void init();
}
