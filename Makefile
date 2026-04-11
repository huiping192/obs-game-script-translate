BUILD_DIR := obs-plugin/build

.PHONY: build install clean rebuild

build:
	@[ -f $(BUILD_DIR)/Makefile ] || cmake -S obs-plugin -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

install: build
	cmake --build $(BUILD_DIR) --target install-plugin

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build
