# hp_vision/sound_gate.cmake -- the TR_HP_SOUND gate of the combined HP image (included by
# hp_vision/CMakeLists.txt before find_package(Zephyr); also run alone by
# tests/host/test_hp_combined.sh:  cmake -DTR_SND_REWORKED_U46=... [-DTR_HP_SOUND=...] -P hp_vision/sound_gate.cmake).
#
# TR_HP_SOUND=ON links the game sound (sound/ GAME firmware: the tr_audio synth draining the
# HE -> HP ring into I2S3 -> both TAS2563 amps) into hp_vision, and with it the i2s3 node on
# P9_3/4/5. On a carrier whose U46 is the stock 74LVC157 that contends with U46's outputs the
# moment i2s_dw muxes the pins at kernel init -- so, exactly as sound/CMakeLists.txt, the sound
# exists only with -DTR_SND_REWORKED_U46=ON. TR_HP_SOUND is opt-in (default OFF): a plain
# hp_vision, for any carrier, is unchanged.
if(NOT DEFINED TR_SND_REWORKED_U46)
	set(TR_SND_REWORKED_U46 OFF)
endif()
if(NOT DEFINED TR_HP_SOUND OR "${TR_HP_SOUND}" STREQUAL "")
	set(TR_HP_SOUND OFF)
endif()
if(TR_HP_SOUND AND NOT TR_SND_REWORKED_U46)
	message(FATAL_ERROR "TR_HP_SOUND=ON drives I2S3 through U46: build only for a carrier with the "
		"reworked U46 (-DTR_SND_REWORKED_U46=ON). Stock 74LVC157 carriers contend on P9_3/4/5. "
		"A stock-U46 hp_vision: leave TR_HP_SOUND off (the default).")
endif()
if(CMAKE_SCRIPT_MODE_FILE)
	message(STATUS "TR_HP_SOUND=${TR_HP_SOUND}")
endif()
