/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 轻量 JSON 解析辅助: 只服务固定键名协议 (配套前端, 格式受控),
 * 不做完整语法校验。Zephyr 版同文件移植。
 */

#ifndef __WEB_JSON_H__
#define __WEB_JSON_H__

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 在 buf[0..len) 中定位 "key" 的值起始位置 (跳过空白与 ':'), 失败返回 NULL */
static inline const char *json_find_value(const char *buf, size_t len, const char *key)
{
	size_t klen = strlen(key);
	size_t i = 0;

	while (i + klen + 2 <= len) {
		if (buf[i] == '"' && memcmp(&buf[i + 1], key, klen) == 0 &&
		    buf[i + klen + 1] == '"') {
			size_t j = i + klen + 2;

			while (j < len && (buf[j] == ' ' || buf[j] == '\t')) {
				j++;
			}
			if (j < len && buf[j] == ':') {
				j++;
				while (j < len && (buf[j] == ' ' || buf[j] == '\t')) {
					j++;
				}
				return &buf[j];
			}
			/* 键后无 ':', 继续找下一处 */
		}
		i++;
	}
	return NULL;
}

/* 取整数 (含 bool: true→1 / false→0) */
static inline bool json_get_i32(const char *buf, size_t len, const char *key, int32_t *out)
{
	const char *v = json_find_value(buf, len, key);

	if (v == NULL || v >= buf + len) {
		return false;
	}
	if (strncmp(v, "true", 4) == 0) {
		*out = 1;
		return true;
	}
	if (strncmp(v, "false", 5) == 0) {
		*out = 0;
		return true;
	}
	char *end;
	long val = strtol(v, &end, 0);

	if (end == v) {
		return false;
	}
	*out = (int32_t)val;
	return true;
}

/* 取字符串值 (不含引号), 截断到 outsz-1 */
static inline bool json_get_str(const char *buf, size_t len, const char *key, char *out,
				size_t outsz)
{
	const char *v = json_find_value(buf, len, key);

	if (v == NULL || v >= buf + len || *v != '"') {
		return false;
	}
	v++;
	size_t n = 0;

	while (v + n < buf + len && v[n] != '"' && v[n] != '\0') {
		if (n + 1 < outsz) {
			out[n] = v[n];
		}
		n++;
	}
	if (v + n >= buf + len || v[n] != '"') {
		return false;
	}
	out[n < outsz ? n : outsz - 1] = '\0';
	return true;
}

/* 取无转义的 URL 查询参数值 (?a=b&c=d 中的 b), 不做百分号解码
 * (本服务参数均为 data_XXXX.raw / 十六进制等安全字符) */
static inline bool url_query_get(const char *query, const char *key, char *out, size_t outsz)
{
	size_t klen = strlen(key);
	const char *p = query;

	while (p != NULL && *p != '\0') {
		if (*p == '?' || *p == '&') {
			p++;
		}
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			p += klen + 1;
			size_t n = 0;

			while (p[n] != '\0' && p[n] != '&') {
				if (n + 1 < outsz) {
					out[n] = p[n];
				}
				n++;
			}
			out[n < outsz ? n : outsz - 1] = '\0';
			return true;
		}
		p = strchr(p, '&');
	}
	return false;
}

#ifdef __cplusplus
}
#endif

#endif /* __WEB_JSON_H__ */
