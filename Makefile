BUILD_DIR := obs-plugin/build

.PHONY: build install test clean rebuild

build:
	@[ -f $(BUILD_DIR)/Makefile ] || cmake -S obs-plugin -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

install: build
	cmake --build $(BUILD_DIR) --target install-plugin

test:
	cmake -S obs-plugin -B $(BUILD_DIR) -DBUILD_TESTS=ON
	cmake --build $(BUILD_DIR)
	cd $(BUILD_DIR) && ctest --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build
