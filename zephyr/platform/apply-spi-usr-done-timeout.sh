#!/usr/bin/env bash
# ESP32-S3 display SPI can wedge forever in spi_esp32_transfer()'s busy-wait
# (MSPI vs octal PSRAM vs BLE). Soft hang: timers freeze, silent_hang on reset.
#
# v281: bounded wait + outer ret break — but replace(...,1) hit the *ISR* outer
# loop (dead: CONFIG_SPI_ESP32_INTERRUPT=n). Polled transceive still ignored
# -ETIMEDOUT → desk GDB v285: flush_y=0, g_display_busy=1, OS frozen.
#
# v286: patch the polled (#else) outer loop only; keep ISR untouched.
# v287: irq_lock across the polled USR/DMA section. v316 halt was flush_y=121
# with cpu0 in btdm_controller_task and a corrupt A0 — the BT controller
# runs during the busy-wait and smashes the window. Lock IRQs only for
# that transfer (a few ms per strip); unlock before k_free.
set -euo pipefail

ZEPHYR_ROOT="${1:?usage: apply-spi-usr-done-timeout.sh ZEPHYR_ROOT}"
SPI="$ZEPHYR_ROOT/drivers/spi/spi_esp32_spim.c"

if [[ ! -f "$SPI" ]]; then
	echo "ERROR: missing $SPI" >&2
	exit 1
fi

if grep -q 'SPI bus quiet v288' "$SPI" && \
	grep -q 'SPI USR-done outer polled v286' "$SPI"; then
	echo "SPI bus quiet v288 already applied (irq lock, msync after unlock)"
	exit 0
fi

# Start from clean upstream if an older timeout patch is present
if grep -qE 'SPI USR-done timeout' "$SPI"; then
	cd "$ZEPHYR_ROOT"
	git checkout -- drivers/spi/spi_esp32_spim.c
fi

python3 - "$SPI" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
t = p.read_text()
if "#include <zephyr/arch/cpu.h>" not in t:
    t = t.replace(
        "#include <zephyr/drivers/spi.h>\n",
        "#include <zephyr/arch/cpu.h>\n#include <zephyr/drivers/spi.h>\n",
        1,
    )

old_wait = """\t/* send data */
\tspi_hal_user_start(hal);
\tspi_context_update_tx(&data->ctx, data->dfs, transfer_len_frames);

\twhile (!spi_hal_usr_is_done(hal)) {
\t\t/* nop */
\t}

#if defined(SOC_GDMA_SUPPORTED)
\tif (cfg->dma_enabled) {
\t\tif (hal_trans->rcv_buffer) {
\t\t\tdma_stop(cfg->dma_dev, cfg->dma_rx_ch);
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32S2)
\t\t\t/* Invalidate cache for RX buffer - S3/S2 have data cache that
\t\t\t * needs to be invalidated after DMA writes to memory
\t\t\t */
\t\t\tesp_cache_msync(hal_trans->rcv_buffer, transfer_len_bytes,
\t\t\t\t\tESP_CACHE_MSYNC_FLAG_DIR_M2C);
#endif
\t\t}
\t\tif (hal_trans->send_buffer) {
\t\t\tdma_stop(cfg->dma_dev, cfg->dma_tx_ch);
\t\t}
\t}
#endif
"""

new_wait = """\t/* send data */
\t{
\t\t/* SPI bus quiet v288 — BT controller ISR must not run while GPSPI
\t\t * USR/DMA is in progress (v316: flush_y mid-frame, A0 corrupt in
\t\t * btdm_controller_task). Cache sync is after irq_unlock so a window spill cannot livelock in _handle_excint. SPI USR-done timeout v286 still applies. No LOG_*.
\t\t */
\t\tconst unsigned int irq_key = irq_lock();

\t\tspi_hal_user_start(hal);
\t\tspi_context_update_tx(&data->ctx, data->dfs, transfer_len_frames);

\t\tconst uint32_t t0 = k_cycle_get_32();
\t\tconst uint32_t budget = k_ms_to_cyc_ceil32(8);
\t\tuint32_t spins = 0;

\t\twhile (!spi_hal_usr_is_done(hal)) {
\t\t\tspins++;
\t\t\tif ((k_cycle_get_32() - t0) > budget || spins > 2000000U) {
\t\t\t\terr = -ETIMEDOUT;
#if defined(SOC_GDMA_SUPPORTED)
\t\t\t\tif (cfg->dma_enabled) {
\t\t\t\t\tif (hal_trans->rcv_buffer) {
\t\t\t\t\t\tdma_stop(cfg->dma_dev, cfg->dma_rx_ch);
\t\t\t\t\t}
\t\t\t\t\tif (hal_trans->send_buffer) {
\t\t\t\t\t\tdma_stop(cfg->dma_dev, cfg->dma_tx_ch);
\t\t\t\t\t}
\t\t\t\t}
#endif
\t\t\t\tirq_unlock(irq_key);
\t\t\t\tdata->ctx.tx_len = 0;
\t\t\t\tdata->ctx.rx_len = 0;
\t\t\t\tgoto free;
\t\t\t}
\t\t}

#if defined(SOC_GDMA_SUPPORTED)
\t\tif (cfg->dma_enabled) {
\t\t\tif (hal_trans->rcv_buffer) {
\t\t\t\tdma_stop(cfg->dma_dev, cfg->dma_rx_ch);
\t\t\t}
\t\t\tif (hal_trans->send_buffer) {
\t\t\t\tdma_stop(cfg->dma_dev, cfg->dma_tx_ch);
\t\t\t}
\t\t}
#endif
\t\tirq_unlock(irq_key);
#if defined(SOC_GDMA_SUPPORTED)
#if defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32S2)
\t\tif (cfg->dma_enabled && hal_trans->rcv_buffer) {
\t\t\tesp_cache_msync(hal_trans->rcv_buffer, transfer_len_bytes,
\t\t\t\t\tESP_CACHE_MSYNC_FLAG_DIR_M2C);
\t\t}
#endif
#endif
\t}
"""

if old_wait not in t:
	sys.stderr.write("ERROR: spi_esp32 USR busy-wait site not found\n")
	sys.exit(1)
t = t.replace(old_wait, new_wait, 1)

# ONLY the polled path inside transceive (#else). ISR copy must stay untouched
# (no `ret` in that function).
old_outer = """#else

	do {
		spi_esp32_transfer(dev);
	} while (spi_esp32_transfer_ongoing(data));

	spi_esp32_complete(dev, data, cfg->spi, 0);

#endif  /* CONFIG_SPI_ESP32_INTERRUPT */
"""

new_outer = """#else

	/* SPI USR-done outer polled v286 — honour transfer() -ETIMEDOUT */
	do {
		ret = spi_esp32_transfer(dev);
		if (ret != 0) {
			break;
		}
	} while (spi_esp32_transfer_ongoing(data));

	spi_esp32_complete(dev, data, cfg->spi, ret);

#endif  /* CONFIG_SPI_ESP32_INTERRUPT */
"""

if old_outer not in t:
	sys.stderr.write("ERROR: spi_esp32 polled outer transfer loop not found\n")
	sys.exit(1)
t = t.replace(old_outer, new_outer, 1)

p.write_text(t)
print("SPI bus quiet v288 applied (irq lock, msync after unlock)")
PY
