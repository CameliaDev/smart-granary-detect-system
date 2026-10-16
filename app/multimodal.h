#ifndef MULTIMODAL_H
#define MULTIMODAL_H

#include "state.h"

/*
 * 多模态大模型图像识别（火山方舟 Responses API，唯一实现）。
 *
 * 接口：POST {MULTIMODAL_API_BASE}/responses（流式 SSE）
 *   默认 BASE : https://ark.cn-beijing.volces.com/api/v3
 *   默认模型 : doubao-seed-2-1-lite-260915
 *   密钥     : MULTIMODAL_API_KEY（环境变量，切勿写进代码）
 *   提示词   : MULTIMODAL_PROMPT（可覆盖内置巡检提示词）
 *
 * 无密钥 / 未链接 libcurl / 网络失败时：填充 graceful stub
 * 并清除 AI 告警标志，不影响传感器告警（E01/E02/E05）与其余功能。
 */
int multimodal_analyze_jpeg(app_state_t *st, const unsigned char *jpeg,
                            size_t jpeg_len);

#endif
