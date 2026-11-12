# Explicit production manifest: adding a source file does not silently link it.
# Legacy examples and host test doubles must never enter the kernel image.
ARCH_C_SOURCES := \
    arch/armv8/address_space.c \
    arch/armv8/irq.c \
    arch/armv8/mmu.c \
    arch/armv8/mmu_tables.c \
    arch/armv8/parallel_port.c \
    arch/armv8/task_port.c \
    arch/armv8/trap.c

ARCH_ASM_SOURCES := \
    arch/armv8/boot.s \
    arch/armv8/exception.s \
    arch/armv8/secondary.s \
    arch/armv8/task_context.s

MEMORY_SOURCES := \
    kernel/mm/boot.c \
    kernel/mm/fdt.c \
    kernel/mm/heap.c \
    kernel/mm/pages.c \
    kernel/mm/ranges.c \
    kernel/mm/runtime.c

FILESYSTEM_SOURCES := \
    kernel/fs/block_store.c \
    kernel/fs/file_syscall.c \
    kernel/fs/files.c \
    kernel/fs/storage.c

NETWORK_SOURCES := \
    kernel/net/dhcp.c \
    kernel/net/dns.c \
    kernel/net/ip.c \
    kernel/net/net_syscall.c \
    kernel/net/profile.c \
    kernel/net/service.c \
    kernel/net/sntp.c \
    kernel/net/socket.c \
    kernel/net/tcp.c

SCHEDULER_SOURCES := \
    kernel/parallel/worker.c \
    kernel/sched/process.c \
    kernel/sched/self_test.c \
    kernel/sched/task.c

UI_SOURCES := \
    kernel/ui/canvas.c \
    kernel/ui/raster.c \
    kernel/ui/render.c \
    kernel/ui/window_manager.c

DRIVER_SOURCES := \
    drivers/ethernet/genet.c \
    drivers/hw/property.c \
    drivers/hw/wait.c \
    drivers/input/key.c \
    drivers/input/touch.c \
    drivers/storage/sd.c \
    drivers/video/v3d.c \
    drivers/wifi/bus.c \
    drivers/wifi/cyw43455.c \
    drivers/wifi/nvram.c \
    drivers/wifi/sdio.c

PLATFORM_SOURCES := \
    platform/pi4/display.c \
    platform/pi4/ethernet.c \
    platform/pi4/hardware.c \
    platform/pi4/i2c.c \
    platform/pi4/io.c \
    platform/pi4/net.c \
    platform/pi4/sd.c \
    platform/pi4/startup.c \
    platform/pi4/storage.c \
    platform/pi4/timer.c

# The terminal is a separate EL0 program: linked on its own at user addresses
# and embedded into the kernel as a flat image, never linked against it.
USER_SOURCES := \
    user/apps/terminal/app.c \
    user/apps/terminal/net_cmd.c \
    user/apps/terminal/net_get.c \
    user/apps/terminal/net_profiles.c \
    user/apps/terminal/net_tools.c \
    user/apps/terminal/terminal.c \
    user/lib/tls.c \
    user/lib/user_lib.c
USER_ASM_SOURCES := \
    user/lib/start.s
# The TLS client's trust anchors when no CA bundle is supplied (TLS_TRUST_C),
# and the parts of BearSSL it uses; see docs/TLS.md.
TLS_TRUST_NONE := user/lib/tls_trust_none.c
BEARSSL_SOURCES := $(addprefix third_party/bearssl/src/, \
    codec/ccopy.c \
    codec/dec32be.c \
    codec/dec32le.c \
    codec/dec64be.c \
    codec/enc32be.c \
    codec/enc32le.c \
    codec/enc64be.c \
    ec/ec_all_m31.c \
    ec/ec_c25519_m64.c \
    ec/ec_p256_m64.c \
    ec/ec_prime_i31.c \
    ec/ec_secp256r1.c \
    ec/ec_secp384r1.c \
    ec/ec_secp521r1.c \
    ec/ecdsa_atr.c \
    ec/ecdsa_i31_bits.c \
    ec/ecdsa_i31_vrfy_asn1.c \
    ec/ecdsa_i31_vrfy_raw.c \
    hash/ghash_ctmul64.c \
    hash/multihash.c \
    hash/sha2big.c \
    hash/sha2small.c \
    int/i31_add.c \
    int/i31_bitlen.c \
    int/i31_decmod.c \
    int/i31_decode.c \
    int/i31_encode.c \
    int/i31_fmont.c \
    int/i31_iszero.c \
    int/i31_modpow.c \
    int/i31_montmul.c \
    int/i31_muladd.c \
    int/i31_ninv31.c \
    int/i31_rshift.c \
    int/i31_sub.c \
    int/i31_tmont.c \
    int/i32_div32.c \
    int/i62_modpow2.c \
    mac/hmac.c \
    rand/hmac_drbg.c \
    rand/sysrng.c \
    rsa/rsa_default_pkcs1_vrfy.c \
    rsa/rsa_i62_pkcs1_vrfy.c \
    rsa/rsa_i62_pub.c \
    rsa/rsa_pkcs1_sig_unpad.c \
    ssl/prf.c \
    ssl/prf_sha256.c \
    ssl/prf_sha384.c \
    ssl/ssl_client.c \
    ssl/ssl_engine.c \
    ssl/ssl_engine_default_aesgcm.c \
    ssl/ssl_engine_default_chapol.c \
    ssl/ssl_engine_default_ecdsa.c \
    ssl/ssl_engine_default_rsavrfy.c \
    ssl/ssl_hs_client.c \
    ssl/ssl_rec_chapol.c \
    ssl/ssl_rec_gcm.c \
    symcipher/aes_ct64.c \
    symcipher/aes_ct64_ctr.c \
    symcipher/aes_ct64_enc.c \
    symcipher/chacha20_ct.c \
    symcipher/poly1305_ctmul.c \
    symcipher/poly1305_ctmulq.c \
    x509/x509_minimal.c)

C_SOURCES := $(sort $(ARCH_C_SOURCES) $(MEMORY_SOURCES) $(FILESYSTEM_SOURCES) \
    $(NETWORK_SOURCES) $(SCHEDULER_SOURCES) $(UI_SOURCES) $(DRIVER_SOURCES) $(PLATFORM_SOURCES) \
    kernel/boot/main.c kernel/syscall/dispatch.c lib/memory.c)
S_SOURCES := $(ARCH_ASM_SOURCES) kernel/boot/terminal_image.s
