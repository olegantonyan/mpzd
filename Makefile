CMAKE_BUILD_DIR=build

run: build
	$(CMAKE_BUILD_DIR)/src/mpzd -l /run/media/oleg/c3996ce0-a379-4403-9d64-7d4c0536463f/music/ -l /run/media/oleg/c3996ce0-a379-4403-9d64-7d4c0536463f/unsorted/

build:
	cmake -S . -B $(CMAKE_BUILD_DIR)
	cmake --build $(CMAKE_BUILD_DIR)

test: build
	ctest --test-dir $(CMAKE_BUILD_DIR) --output-on-failure

clean:
	rm -rf $(CMAKE_BUILD_DIR)

.PHONY: build run test clean

include _testing.mk
