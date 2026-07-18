idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
	SRCS
		"album_art.c"
		"app_settings.c"
		"audio_analysis.c"
		"audio_player.c"
		"effects.c"
		"jellyfin_client.c"
		"lastfm_scrobbler.c"
		"main.c"
		"media_library.c"
		"wifi_setup.c"
	PRIV_REQUIRES
		badge-bsp
		bt
		driver
		esp-dsp
		esp-tls
		esp_audio_codec
		esp_driver_sdmmc
		esp_http_client
		esp_jpeg
		esp_lcd
		esp_netif
		esp_timer
		fatfs
		json
		mbedtls
		nvs_flash
		pax-gfx
		sdmmc
		tanmatsu-wifi
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_component_set_property(robotman2412__pax-gfx WHOLE_ARCHIVE TRUE)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
