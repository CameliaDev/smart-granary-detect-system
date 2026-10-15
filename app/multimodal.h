#ifndef MULTIMODAL_H
#define MULTIMODAL_H

#include "state.h"

/**
 * Analyze JPEG via multimodal HTTP API (OpenAI-compatible chat/completions).
 * Env: MULTIMODAL_API_BASE, MULTIMODAL_API_KEY, MULTIMODAL_MODEL.
 * Without key/network, fills a graceful stub and clears AI alarm flags.
 */
int multimodal_analyze_jpeg(app_state_t *st, const unsigned char *jpeg,
                            size_t jpeg_len);

#endif
