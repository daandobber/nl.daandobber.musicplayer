idf_component_register(
	SRCS
		"album_art.c"
		"app_settings.c"
		"audio_analysis.c"
		"audio_player.c"
		"effects.c"
		"main.c"
		"media_library.c"
	PRIV_REQUIRES
		badge-bsp
		driver
		esp-dsp
		esp_audio_codec
		esp_driver_sdmmc
		esp_jpeg
		esp_lcd
		esp_timer
		fatfs
		nvs_flash
		sdmmc
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
