# Hang-hunt helpers for ESP32-S3 USB-JTAG (no GDB Python required).
#
# Expected failure modes (v278 renderer):
#   1) Mid-SPI / mid-PSRAM-clear wedge: g_display_busy==1 or g_flush_cur_y>=0 stuck
#   2) Main loop frozen while 1 Hz timer still ticks (silent_hang class)
#   3) Hard fault / assert → z_fatal_error
#
# Commands: hang_vars / hang_dump / hang_arm / hang_arm_flush

set pagination off
set confirm off
set print pretty on

define hang_vars
  printf "\n=== hang probe ===\n"
  printf "g_main_loops=%u  hb_render_frames=%u  g_render_frames_total=%u\n", \
    g_main_loops, hb_render_frames, g_render_frames_total
  printf "g_render_stage=%u  g_render_stage_ms=%u\n", g_render_stage, g_render_stage_ms
  printf "g_display_busy=%u  g_in_frame=%u  g_flush_cur_y=%d\n", g_display_busy, g_in_frame, g_flush_cur_y
  printf "g_flush_cur_start_ms=%u  g_flush_last_elapsed_ms=%u  g_flush_last_ret=%d\n", \
    g_flush_cur_start_ms, g_flush_last_elapsed_ms, g_flush_last_ret
  printf "g_spi_tripped=%u  g_spi_enabled=%u\n", g_spi_tripped, g_spi_enabled
  printf "g_alive.uptime_ms=%u  timer_ticks=%u  dirty=%u  step='%s' rstage=%u\n", \
    g_alive.uptime_ms, g_alive.timer_ticks, g_alive.dirty, g_alive.main_step, g_alive.render_stage
  if g_main_step
    printf "g_main_step=%s\n", g_main_step
  end
  printf "mt200 want=%u busy=%u phase=%u quiet_spi=%u\n", \
    g_mt200_want, g_mt200_busy, g_mt200_phase, g_mt200_quiet_spi_armed
end

define hang_dump
  hang_vars
  printf "\n=== cpu0 bt ===\n"
  bt 24
  printf "\n=== registers ===\n"
  info registers pc a0 a1 a2 a3 PS WINDOWBASE WINDOWSTART
  printf "\n=== threads ===\n"
  info threads
  thread apply all bt 10
end

define hang_arm
  delete breakpoints

  hbreak z_fatal_error
  commands
    silent
    printf "\n*** HIT z_fatal_error ***\n"
    hang_dump
  end

  hbreak assert_post_action
  commands
    silent
    printf "\n*** HIT assert_post_action ***\n"
    hang_dump
  end

  printf "hang_arm: z_fatal_error + assert_post_action armed\n"
end

define hang_arm_flush
  hbreak flush_fb if g_flush_cur_y >= 0
  commands
    silent
    printf "\n*** flush_fb while g_flush_cur_y=%d (nested/stuck?) ***\n", g_flush_cur_y
    hang_dump
  end
  printf "hang_arm_flush: conditional hbreak flush_fb if g_flush_cur_y>=0\n"
end

define hang_arm_busy_watch
  awatch g_display_busy
  commands
    silent
    printf "g_display_busy -> %u  flush_y=%d in_frame=%u\n", g_display_busy, g_flush_cur_y, g_in_frame
    continue
  end
  printf "hang_arm_busy_watch: awatch g_display_busy (noisy — prefer poller)\n"
end

printf "loaded hang-hunt helpers (hang_arm / hang_dump / hang_vars)\n"
