cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# RSA 软件回退路径的滑窗表内存:默认 6 需要 ~33KB 连续内存;C3 的 RSA 硬件加速
# 上限是 3072 位,RSA-4096 证书(如 Let's Encrypt 链)回退软件后在此分配失败,
# 表现为所有 HTTPS 验签失败(-0x4290)。降到 2 只需 ~2KB,验签慢约 1 秒,
# 每小时一次的热力图拉取完全可接受。该宏不在 Kconfig,只能全局编译定义。
idf_build_set_property(C_COMPILE_OPTIONS "-DMBEDTLS_MPI_WINDOW_SIZE=2" APPEND)

project(FoloToy-AI-Passport)
