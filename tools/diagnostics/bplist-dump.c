/* 宿主侧验证工具：dump "返回车机" requestUI 的 bplist 字节。
 * 只链接 airplay_bplist.c，不依赖任何设备头文件。 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

int airplay_bplist_encode_car_ui_request(uint8_t *output, size_t capacity,
                                         size_t *output_len);

int main(void)
{
	uint8_t buffer[256];
	size_t len = 0;
	size_t i;

	if (airplay_bplist_encode_car_ui_request(buffer, sizeof(buffer), &len) < 0) {
		printf("encode failed\n");
		return 1;
	}
	printf("len=%lu\n", (unsigned long)len);
	printf("hex=");
	for (i = 0; i < len; i++)
		printf("%02x", buffer[i]);
	printf("\n");
	return 0;
}
