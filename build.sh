export Zephyr_DIR=../zephyr/share/zephyr-package/cmake/
west build -s ../zmk/app -d $(pwd)/.build/demo \
    -b nice_nano_v2 -- -DSHIELD=corne_right -DZMK_CONFIG=$(pwd)/config -DZMK_EXTRA_MODULES=$(pwd)/ \
    -DDTC_OVERLAY_FILE=$(pwd)/config/cornix_right_mono.overlay \
    -DCONFIG_RGBLED_WIDGET_MONO=y \
    -DCONFIG_RGBLED_WIDGET=y
