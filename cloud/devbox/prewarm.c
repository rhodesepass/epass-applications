/* 预热 emcc 缓存用: 两条构建路径的 flag 组合各编一次, 把 sysroot 变体和
 * freetype/libpng/zlib ports 烘进镜像层。之后 EM_FROZEN_CACHE=1, 运行期
 * 若还想写缓存说明预热漏了 —— 当场报错好过静默重编到超时。 */
#include <stdio.h>

int main(void)
{
    printf("prewarm\n");
    return 0;
}
